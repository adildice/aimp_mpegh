// aimp_mpegh - MPEG-H 3D Audio (mha1 / mhm1 in MP4) input plugin for AIMP 5.x (64-bit)
//
// Demuxing : Fraunhofer mmtisobmff
// Decoding : Fraunhofer FDK MPEG-H decoder (mpeghdec)
//
// The plugin registers an audio decoder extension. For every MP4 file AIMP opens it
// checks whether the file carries an mha1/mhm1 track; all other files are rejected
// immediately so AIMP's own decoders keep handling them.

#include <windows.h>
#include <unknwn.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "apiPlugin.h"
#include "apiDecoders.h"
#include "apiFileManager.h"
#include "apiMUI.h"

#include "ilo/memory.h"
#include "mmtisobmff/types.h"
#include "mmtisobmff/logging.h"
#include "mmtisobmff/reader/input.h"
#include "mmtisobmff/reader/reader.h"
#include "mmtisobmff/reader/trackreader.h"
#include "mpeghdecoder.h"
#include "binaural.h"

using namespace mmt::isobmff;

static const wchar_t* kPluginName = L"MPEG-H 3D Audio decoder (mha1/mhm1) v1.2.1";
static const wchar_t* kPluginAuthor = L"Built with Fraunhofer FDK MPEG-H Software";
static const wchar_t* kPluginShort = L"Plays MPEG-H 3D Audio (mha1 / mhm1) tracks in MP4 / M4A files";
static const wchar_t* kPluginFull =
    L"Decodes MPEG-H 3D Audio, including 360 Reality Audio music files. "
    L"Output layout and loudness are set in the plugin's settings dialog.";

static HMODULE g_module = nullptr;
static IAIMPCore* g_core = nullptr;

// ------------------------------------------------------------------------------------
// Settings (aimp_mpegh.ini next to the DLL, re-read every time a file is opened)
// ------------------------------------------------------------------------------------
struct Settings {
  int cicp = 2;            // target loudspeaker layout (CICP index)
  bool headphones = false; // decode to 7.1.4 and render it binaurally to two channels
  int room = 30;           // room sound for headphones, 0..100
  int targetLoudness = -1; // decoder units (-0.25 LU steps, 40..127), -1 = decoder default
  int drcEffect = -100;    // -1..6, -100 = decoder default
  bool readTags = true;
};

// Default settings file, written on first start if the installer did not copy one.
static const char kDefaultIni[] =
    "; Settings for the MPEG-H 3D Audio plugin for AIMP.\r\n"
    "; Keep this file next to aimp_mpegh.dll. Changes apply to the next track you start.\r\n"
    "\r\n"
    "[MPEG-H]\r\n"
    "\r\n"
    "; Output speaker layout:\r\n"
    ";   headphones - Headphones (3D): the full 3D mix rendered for headphones (default)\r\n"
    ";   stereo  - 2.0 plain speaker downmix (works on any sound card)\r\n"
    ";   5.1     - L R C LFE Ls Rs\r\n"
    ";   7.1     - L R C LFE + side and back surrounds\r\n"
    ";   9.1.6   - 16 channels: L R C LFE Lb Rb Ls Rs Lw Rw + top front, top rear, top middle pairs\r\n"
    ";             (needs a 16-channel output device; AIMP itself may refuse more than 8 channels)\r\n"
    ";   mono\r\n"
    "; A raw MPEG CICP layout number is also accepted (advanced, e.g. 10 = quad).\r\n"
    "Layout=headphones\r\n"
    "\r\n"
    "; Room sound for Headphones (3D), from 0 (dry) to 100. Default 30.\r\n"
    "HeadphoneRoom=30\r\n"
    "\r\n"
    "; Loudness normalisation target in LKFS, from -10 to -31.75.\r\n"
    "; Leave empty for the decoder default (-24). Use -16 for a louder, \"music player\" level.\r\n"
    "TargetLoudness=\r\n"
    "\r\n"
    "; Dynamic range control effect. Leave empty for the default (off unless needed to avoid clipping).\r\n"
    ";   -1 off   1 late night   2 noisy environment   3 limited playback range\r\n"
    ";    4 low playback level   5 dialog enhancement   6 general compression\r\n"
    "DrcEffect=\r\n"
    "\r\n"
    "; 1 = the plugin reports title / artist / album etc. from the MP4 tags.\r\n"
    "; 0 = leave tag reading entirely to AIMP (try this if tags or cover art look wrong).\r\n"
    "ReadTags=1\r\n";

static std::wstring g_iniPath; // resolved in resolveIniPath()

static std::wstring dllDir() {
  wchar_t buf[MAX_PATH * 4] = {0};
  GetModuleFileNameW(g_module, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0]) - 1));
  std::wstring p(buf);
  size_t slash = p.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1);
}

static bool fileExists(const std::wstring& p) {
  DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool writeDefaultIni(const std::wstring& p) {
  HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  BOOL ok = WriteFile(h, kDefaultIni, static_cast<DWORD>(sizeof(kDefaultIni) - 1), &written, nullptr);
  CloseHandle(h);
  return ok && written == sizeof(kDefaultIni) - 1;
}

static std::wstring g_profileIni; // AIMP profile folder candidate (always writable)

// Settings file lookup: AIMP's profile folder first (that is where the settings dialog saves when
// the plugin folder is write-protected), then next to the DLL. If neither exists a default file
// is created next to the DLL or, failing that, in the profile folder.
static void resolveIniPath(IAIMPCore* core) {
  std::wstring nearDll = dllDir() + L"aimp_mpegh.ini";
  g_profileIni.clear();
  if (core) {
    IAIMPString* s = nullptr;
    if (SUCCEEDED(core->GetPath(AIMP_CORE_PATH_PROFILE, &s)) && s) {
      g_profileIni.assign(s->GetData(), static_cast<size_t>(s->GetLength()));
      s->Release();
      if (!g_profileIni.empty()) {
        if (g_profileIni.back() != L'\\' && g_profileIni.back() != L'/') g_profileIni += L'\\';
        g_profileIni += L"aimp_mpegh.ini";
      }
    }
  }
  if (!g_profileIni.empty() && fileExists(g_profileIni)) g_iniPath = g_profileIni;
  else if (fileExists(nearDll)) g_iniPath = nearDll;
  else if (writeDefaultIni(nearDll)) g_iniPath = nearDll;
  else if (!g_profileIni.empty() && writeDefaultIni(g_profileIni)) g_iniPath = g_profileIni;
  else g_iniPath = nearDll;
}

static std::wstring iniPath() { return g_iniPath.empty() ? dllDir() + L"aimp_mpegh.ini" : g_iniPath; }

static std::wstring trimLower(const wchar_t* s) {
  std::wstring v(s);
  size_t c = v.find_first_of(L";#");
  if (c != std::wstring::npos) v.erase(c);
  while (!v.empty() && iswspace(v.back())) v.pop_back();
  size_t i = 0;
  while (i < v.size() && iswspace(v[i])) i++;
  v.erase(0, i);
  for (auto& ch : v) ch = static_cast<wchar_t>(towlower(ch));
  return v;
}

static Settings loadSettings() {
  Settings s;
  std::wstring ini = iniPath();
  wchar_t buf[128];

  GetPrivateProfileStringW(L"MPEG-H", L"Layout", L"headphones", buf, 127, ini.c_str());
  std::wstring v = trimLower(buf);
  if (v == L"headphones" || v == L"binaural" || v == L"3d") {
    s.cicp = 19; // 7.1.4, rendered to two channels afterwards
    s.headphones = true;
  } else if (v == L"mono" || v == L"1.0") s.cicp = 1;
  else if (v == L"stereo" || v == L"2.0" || v.empty()) s.cicp = 2;
  else if (v == L"5.1") s.cicp = 6;
  else if (v == L"7.1") s.cicp = 12;
  else if (v == L"9.1.6") s.cicp = 300; // 7.1 bed + wides + top front / top rear / top middle
  else {
    int n = _wtoi(v.c_str());
    s.cicp = (n >= 1 && n <= 500) ? n : 2;
  }

  int room = static_cast<int>(GetPrivateProfileIntW(L"MPEG-H", L"HeadphoneRoom", 30, ini.c_str()));
  s.room = std::max(0, std::min(100, room));

  GetPrivateProfileStringW(L"MPEG-H", L"TargetLoudness", L"", buf, 127, ini.c_str());
  v = trimLower(buf);
  if (!v.empty()) {
    double lkfs = _wtof(v.c_str()); // e.g. -16
    int q = static_cast<int>(-lkfs * 4.0 + 0.5);
    s.targetLoudness = std::max(40, std::min(127, q));
  }

  GetPrivateProfileStringW(L"MPEG-H", L"DrcEffect", L"", buf, 127, ini.c_str());
  v = trimLower(buf);
  if (!v.empty()) {
    int n = _wtoi(v.c_str());
    if (n >= -1 && n <= 6) s.drcEffect = n;
  }

  s.readTags = GetPrivateProfileIntW(L"MPEG-H", L"ReadTags", 1, ini.c_str()) != 0;
  return s;
}

// ------------------------------------------------------------------------------------
// IAIMPStream -> mmtisobmff input adapter
// ------------------------------------------------------------------------------------
struct StreamShared {
  IAIMPStream* stream;
  std::mutex lock;
  uint64_t size;

  explicit StreamShared(IAIMPStream* s) : stream(s) {
    stream->AddRef();
    INT64 sz = stream->GetSize();
    size = sz > 0 ? static_cast<uint64_t>(sz) : 0;
  }
  ~StreamShared() { stream->Release(); }

  size_t readAt(uint64_t at, uint8_t* dst, size_t n) {
    std::lock_guard<std::mutex> g(lock);
    if (at >= size || n == 0) return 0;
    n = static_cast<size_t>(std::min<uint64_t>(n, size - at));
    if (FAILED(stream->Seek(static_cast<INT64>(at), AIMP_STREAM_SEEKMODE_FROM_BEGINNING))) return 0;
    size_t got = 0;
    while (got < n) {
      unsigned int want = static_cast<unsigned int>(std::min<size_t>(n - got, 1u << 20));
      int r = stream->Read(dst + got, want);
      if (r <= 0) break;
      got += static_cast<size_t>(r);
    }
    return got;
  }
};

class CAimpInput : public IIsobmffInput {
 public:
  explicit CAimpInput(std::shared_ptr<StreamShared> sh) : m_sh(std::move(sh)) {}

  size_t read(ilo::ByteBuffer::iterator inBegin, ilo::ByteBuffer::iterator inEnd) override {
    size_t n = static_cast<size_t>(inEnd - inBegin);
    if (n == 0) return 0;
    uint8_t* dst = &(*inBegin);
    size_t done = 0;
    while (done < n) {
      if (m_pos >= m_cachePos && m_pos < m_cachePos + m_cache.size()) {
        size_t off = static_cast<size_t>(m_pos - m_cachePos);
        size_t c = std::min(n - done, m_cache.size() - off);
        memcpy(dst + done, m_cache.data() + off, c);
        done += c;
        m_pos += c;
        continue;
      }
      if (n - done >= kCacheSize) {
        size_t r = m_sh->readAt(m_pos, dst + done, n - done);
        done += r;
        m_pos += r;
        break;
      }
      m_cache.resize(kCacheSize);
      size_t r = m_sh->readAt(m_pos, m_cache.data(), kCacheSize);
      m_cache.resize(r);
      m_cachePos = m_pos;
      if (r == 0) break;
    }
    return done;
  }

  void seek(pos_type pos) override { m_pos = pos; }

  void seek(offset_type offset, SeekingOrigin origin) override {
    int64_t base = 0;
    switch (origin) {
      case SeekingOrigin::beg: base = 0; break;
      case SeekingOrigin::end: base = static_cast<int64_t>(m_sh->size); break;
      case SeekingOrigin::cur: base = static_cast<int64_t>(m_pos); break;
    }
    int64_t p = base + offset;
    m_pos = p < 0 ? 0 : static_cast<uint64_t>(p);
  }

  pos_type tell() override { return m_pos; }
  bool isEOI() override { return m_pos >= m_sh->size; }

  std::unique_ptr<IIsobmffInput> clone() override {
    auto c = ilo::make_unique<CAimpInput>(m_sh);
    c->m_pos = m_pos;
    return std::unique_ptr<IIsobmffInput>(c.release());
  }

 private:
  static constexpr size_t kCacheSize = 64 * 1024;
  std::shared_ptr<StreamShared> m_sh;
  uint64_t m_pos = 0;
  std::vector<uint8_t> m_cache;
  uint64_t m_cachePos = 0;
};

// ------------------------------------------------------------------------------------
// Quick MP4 inspection: is there an MPEG-H track? Also collects iTunes-style tags.
// ------------------------------------------------------------------------------------
static inline uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
static inline uint64_t be64(const uint8_t* p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }

struct Tags {
  std::wstring title, artist, album, albumArtist, date, genre, composer, comment;
  std::wstring trackNo, trackTotal, discNo, discTotal;
};

static std::wstring utf8ToWide(const uint8_t* p, size_t n) {
  if (n == 0) return std::wstring();
  int len = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(p), static_cast<int>(n),
                                nullptr, 0);
  if (len <= 0) return std::wstring();
  std::wstring w(static_cast<size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(p), static_cast<int>(n), &w[0], len);
  while (!w.empty() && w.back() == L'\0') w.pop_back();
  return w;
}

// Finds a child box of the given type inside [p, p+n). Returns payload pointer/size.
static bool findBox(const uint8_t* p, size_t n, const char* type, const uint8_t** payload,
                    size_t* payloadSize) {
  size_t off = 0;
  while (off + 8 <= n) {
    uint64_t sz = be32(p + off);
    size_t hdr = 8;
    if (sz == 1) {
      if (off + 16 > n) return false;
      sz = be64(p + off + 8);
      hdr = 16;
    } else if (sz == 0) {
      sz = n - off;
    }
    if (sz < hdr || sz > n - off) return false;
    if (memcmp(p + off + 4, type, 4) == 0) {
      *payload = p + off + hdr;
      *payloadSize = static_cast<size_t>(sz) - hdr;
      return true;
    }
    off += static_cast<size_t>(sz);
  }
  return false;
}

static void parseTags(const uint8_t* moov, size_t n, Tags& t) {
  const uint8_t *udta, *meta, *ilst;
  size_t udtaN, metaN, ilstN;
  if (!findBox(moov, n, "udta", &udta, &udtaN)) return;
  if (!findBox(udta, udtaN, "meta", &meta, &metaN) || metaN < 4) return;
  if (!findBox(meta + 4, metaN - 4, "ilst", &ilst, &ilstN)) return;

  size_t off = 0;
  while (off + 8 <= ilstN) {
    uint32_t sz = be32(ilst + off);
    if (sz < 8 || sz > ilstN - off) break;
    const uint8_t* name = ilst + off + 4;
    const uint8_t* data;
    size_t dataN;
    if (findBox(ilst + off + 8, sz - 8, "data", &data, &dataN) && dataN >= 8) {
      const uint8_t* v = data + 8;
      size_t vn = dataN - 8;
      auto is = [&](const char* id) { return memcmp(name, id, 4) == 0; };
      if (is("\xA9nam")) t.title = utf8ToWide(v, vn);
      else if (is("\xA9" "ART")) t.artist = utf8ToWide(v, vn);
      else if (is("\xA9" "alb")) t.album = utf8ToWide(v, vn);
      else if (is("aART")) t.albumArtist = utf8ToWide(v, vn);
      else if (is("\xA9" "day")) t.date = utf8ToWide(v, vn);
      else if (is("\xA9gen")) t.genre = utf8ToWide(v, vn);
      else if (is("\xA9wrt")) t.composer = utf8ToWide(v, vn);
      else if (is("\xA9" "cmt")) t.comment = utf8ToWide(v, vn);
      else if (is("trkn") && vn >= 6) {
        int a = (v[2] << 8) | v[3], b = (v[4] << 8) | v[5];
        if (a) t.trackNo = std::to_wstring(a);
        if (b) t.trackTotal = std::to_wstring(b);
      } else if (is("disk") && vn >= 6) {
        int a = (v[2] << 8) | v[3], b = (v[4] << 8) | v[5];
        if (a) t.discNo = std::to_wstring(a);
        if (b) t.discTotal = std::to_wstring(b);
      }
    }
    off += sz;
  }
}

static bool containsFourcc(const std::vector<uint8_t>& buf, const char* fcc) {
  if (buf.size() < 4) return false;
  const uint8_t* p = buf.data();
  const uint8_t* end = p + buf.size() - 3;
  while (p < end) {
    p = static_cast<const uint8_t*>(memchr(p, fcc[0], static_cast<size_t>(end - p)));
    if (!p) return false;
    if (memcmp(p, fcc, 4) == 0) return true;
    p++;
  }
  return false;
}

// Returns true if the stream looks like an MP4 with an MPEG-H sample entry.
static bool probeMpegh(StreamShared& sh, Tags& tags) {
  uint8_t hdr[16];
  if (sh.readAt(0, hdr, 12) != 12) return false;
  if (memcmp(hdr + 4, "ftyp", 4) != 0 && memcmp(hdr + 4, "styp", 4) != 0) return false;

  uint64_t off = 0;
  for (int i = 0; i < 4096 && off + 8 <= sh.size; i++) {
    if (sh.readAt(off, hdr, 8) != 8) return false;
    uint64_t sz = be32(hdr);
    uint64_t hsz = 8;
    if (sz == 1) {
      if (sh.readAt(off + 8, hdr + 8, 8) != 8) return false;
      sz = be64(hdr + 8);
      hsz = 16;
    } else if (sz == 0) {
      sz = sh.size - off;
    }
    if (sz < hsz || sz > sh.size - off) return false;

    if (memcmp(hdr + 4, "moov", 4) == 0) {
      uint64_t payload = sz - hsz;
      if (payload > (256u << 20)) return true; // unusually large index: let the real parser decide
      std::vector<uint8_t> moov(static_cast<size_t>(payload));
      if (sh.readAt(off + hsz, moov.data(), moov.size()) != moov.size()) return false;
      bool ok = containsFourcc(moov, "mha1") || containsFourcc(moov, "mhm1") ||
                containsFourcc(moov, "mha2") || containsFourcc(moov, "mhm2");
      if (ok) parseTags(moov.data(), moov.size(), tags);
      return ok;
    }
    off += sz;
  }
  return false;
}

// ------------------------------------------------------------------------------------
// The decoder
// ------------------------------------------------------------------------------------
static const int kMaxChannels = 24;
static const int kMaxFrame = 3072;

class CMpeghDecoder : public IAIMPAudioDecoder {
 public:
  CMpeghDecoder() : m_ref(1) {}

  virtual ~CMpeghDecoder() {
    if (m_dec) mpeghdecoder_destroy(m_dec);
    m_track.reset();
    m_reader.reset();
  }

  // Throws / returns false if the stream cannot be played.
  bool open(IAIMPStream* stream) {
    m_settings = loadSettings();
    m_shared = std::make_shared<StreamShared>(stream);
    if (!probeMpegh(*m_shared, m_tags)) return false;

    m_reader = ilo::make_unique<CIsobmffReader>(
        std::unique_ptr<IIsobmffInput>(new CAimpInput(m_shared)));

    for (const auto& ti : m_reader->trackInfos()) {
      if (ti.codec != Codec::mpegh_mha && ti.codec != Codec::mpegh_mhm) continue;
      m_track = m_reader->trackByIndex<CMpeghTrackReader>(ti.trackIndex);
      if (!m_track) continue;
      m_info = ti;
      break;
    }
    if (!m_track) return false;

    m_dec = mpeghdecoder_init(m_settings.cicp);
    if (!m_dec && m_settings.headphones) {
      m_settings.cicp = 12; // no 7.1.4 renderer: build the 3D sound from 7.1 instead
      m_dec = mpeghdecoder_init(12);
    }
    if (!m_dec && m_settings.cicp != 2) {
      m_settings.headphones = false;
      m_settings.cicp = 2; // unsupported layout requested -> fall back to stereo
      m_dec = mpeghdecoder_init(2);
    }
    if (!m_dec) return false;

    if (m_settings.targetLoudness >= 0)
      mpeghdecoder_setParam(m_dec, MPEGH_DEC_PARAM_TARGET_REFERENCE_LEVEL, m_settings.targetLoudness);
    if (m_settings.drcEffect != -100)
      mpeghdecoder_setParam(m_dec, MPEGH_DEC_PARAM_EFFECT_TYPE, m_settings.drcEffect);

    if (m_info.codec == Codec::mpegh_mha) {
      auto dcr = m_track->mhaDecoderConfigRecord();
      if (!dcr || dcr->mpegh3daConfig().empty()) return false;
      if (mpeghdecoder_setMhaConfig(m_dec, dcr->mpegh3daConfig().data(),
                                    static_cast<uint32_t>(dcr->mpegh3daConfig().size())) != MPEGH_DEC_OK)
        return false;
    }

    m_sample = CSample{m_info.maxSampleSize};
    m_out.resize(kMaxChannels * kMaxFrame);
    if (m_settings.headphones) m_raw.resize(kMaxChannels * kMaxFrame);

    // Decode the first frame so that the real output format is known.
    m_sampleInfo = m_track->nextSample(m_sample);
    m_haveSample = !m_sample.empty();
    if (!decodeFrame() || m_channels <= 0 || m_sampleRate <= 0) return false;

    double seconds = m_info.timescale ? static_cast<double>(m_info.duration) / m_info.timescale : 0.0;
    m_totalFrames = static_cast<int64_t>(seconds * m_sampleRate + 0.5);
    m_duration = seconds;
    return true;
  }

  // IUnknown
  HRESULT WINAPI QueryInterface(REFIID riid, void** obj) override {
    if (!obj) return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IAIMPAudioDecoder)) {
      *obj = static_cast<IAIMPAudioDecoder*>(this);
      AddRef();
      return S_OK;
    }
    *obj = nullptr;
    return E_NOINTERFACE;
  }
  ULONG WINAPI AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&m_ref)); }
  ULONG WINAPI Release() override {
    LONG r = InterlockedDecrement(&m_ref);
    if (r == 0) delete this;
    return static_cast<ULONG>(r);
  }

  // IAIMPAudioDecoder
  BOOL WINAPI GetFileInfo(IAIMPFileInfo* fi) override {
    if (!fi || !m_settings.readTags) return FALSE;
    try {
      fi->BeginUpdate();
      fi->SetValueAsFloat(AIMP_FILEINFO_PROPID_DURATION, m_duration);
      fi->SetValueAsInt32(AIMP_FILEINFO_PROPID_SAMPLERATE, m_sampleRate);
      fi->SetValueAsInt32(AIMP_FILEINFO_PROPID_CHANNELS, m_channels);
      fi->SetValueAsInt32(AIMP_FILEINFO_PROPID_BITDEPTH, 32);
      fi->SetValueAsInt64(AIMP_FILEINFO_PROPID_FILESIZE, static_cast<INT64>(m_shared->size));
      if (m_duration > 0.0)
        fi->SetValueAsInt32(AIMP_FILEINFO_PROPID_BITRATE,
                            static_cast<int>(m_shared->size * 8.0 / m_duration / 1000.0 + 0.5));
      setString(fi, AIMP_FILEINFO_PROPID_CODEC, L"MPEG-H 3D Audio");
      setString(fi, AIMP_FILEINFO_PROPID_TITLE, m_tags.title);
      setString(fi, AIMP_FILEINFO_PROPID_ARTIST, m_tags.artist);
      setString(fi, AIMP_FILEINFO_PROPID_ALBUM, m_tags.album);
      setString(fi, AIMP_FILEINFO_PROPID_ALBUMARTIST, m_tags.albumArtist);
      setString(fi, AIMP_FILEINFO_PROPID_DATE, m_tags.date);
      setString(fi, AIMP_FILEINFO_PROPID_GENRE, m_tags.genre);
      setString(fi, AIMP_FILEINFO_PROPID_COMPOSER, m_tags.composer);
      setString(fi, AIMP_FILEINFO_PROPID_COMMENT, m_tags.comment);
      setString(fi, AIMP_FILEINFO_PROPID_TRACKNUMBER, m_tags.trackNo);
      setString(fi, AIMP_FILEINFO_PROPID_TRACKTOTAL, m_tags.trackTotal);
      setString(fi, AIMP_FILEINFO_PROPID_DISKNUMBER, m_tags.discNo);
      setString(fi, AIMP_FILEINFO_PROPID_DISKTOTAL, m_tags.discTotal);
      fi->EndUpdate();
      return TRUE;
    } catch (...) {
      return FALSE;
    }
  }

  BOOL WINAPI GetStreamInfo(int* sampleRate, int* channels, int* sampleFormat) override {
    if (sampleRate) *sampleRate = m_sampleRate;
    if (channels) *channels = m_channels;
    if (sampleFormat) *sampleFormat = AIMP_DECODER_SAMPLEFORMAT_32BIT;
    return TRUE;
  }

  BOOL WINAPI IsSeekable() override { return TRUE; }
  BOOL WINAPI IsRealTimeStream() override { return FALSE; }

  INT64 WINAPI GetAvailableData() override {
    std::lock_guard<std::mutex> g(m_lock);
    INT64 left = (m_totalFrames - m_posFrames) * frameBytes();
    return left > 0 ? left : 0;
  }
  INT64 WINAPI GetSize() override { return m_totalFrames * frameBytes(); }
  INT64 WINAPI GetPosition() override {
    std::lock_guard<std::mutex> g(m_lock);
    return m_posFrames * frameBytes();
  }

  BOOL WINAPI SetPosition(const INT64 value) override {
    std::lock_guard<std::mutex> g(m_lock);
    try {
      int64_t target = value / frameBytes();
      if (target < 0) target = 0;
      if (m_totalFrames > 0 && target > m_totalFrames) target = m_totalFrames;

      uint64_t ts = static_cast<uint64_t>(static_cast<double>(target) / m_sampleRate * m_info.timescale);
      // Prefer the last sync sample at or before the target, then decode forward to it.
      bool found = false;
      try {
        SSeekConfig cfg(CTimeDuration(m_info.timescale, ts + 1),
                        ESampleSeekMode::lastSyncSampleSmaller);
        m_sampleInfo = m_track->sampleByTimestamp(cfg, m_sample);
        found = !m_sample.empty();
      } catch (...) {
        found = false;
      }
      if (!found) {
        SSeekConfig cfg(CTimeDuration(m_info.timescale, ts), ESampleSeekMode::nearestSyncSample);
        m_sampleInfo = m_track->sampleByTimestamp(cfg, m_sample);
      }
      m_haveSample = !m_sample.empty();
      m_flushed = false;
      m_eof = false;
      m_pending = 0;
      m_pendingOff = 0;
      mpeghdecoder_flush(m_dec);
      if (m_settings.headphones) m_bin.reset();

      m_posFrames = target;
      if (!m_haveSample) {
        m_eof = true;
        return TRUE;
      }
      // Decode forward to the exact position (the sync sample lies at or before the target).
      for (int guard = 0; guard < 4096; guard++) {
        if (!decodeFrame()) break;
        int64_t start = static_cast<int64_t>(m_lastTicks);
        int64_t end = start + m_pending;
        if (start > target || end > target) {
          if (start < target) {
            int skip = static_cast<int>(target - start);
            m_pendingOff = skip;
            m_pending -= skip;
          } else {
            m_posFrames = start; // nearest sync sample lies after the requested point
          }
          break;
        }
        m_pending = 0;
        m_pendingOff = 0;
      }
      return TRUE;
    } catch (...) {
      m_eof = true;
      return FALSE;
    }
  }

  int WINAPI Read(void* buffer, int count) override {
    std::lock_guard<std::mutex> g(m_lock);
    if (!buffer || count <= 0) return 0;
    try {
      const int fb = frameBytes();
      int wantFrames = count / fb;
      int doneFrames = 0;
      uint8_t* dst = static_cast<uint8_t*>(buffer);
      while (doneFrames < wantFrames) {
        if (m_pending == 0 && !decodeFrame()) break;
        int n = std::min(wantFrames - doneFrames, m_pending);
        memcpy(dst + static_cast<size_t>(doneFrames) * fb,
               m_out.data() + static_cast<size_t>(m_pendingOff) * m_channels,
               static_cast<size_t>(n) * fb);
        m_pendingOff += n;
        m_pending -= n;
        doneFrames += n;
      }
      m_posFrames += doneFrames;
      return doneFrames * fb;
    } catch (...) {
      m_eof = true;
      return 0;
    }
  }

 private:
  int frameBytes() const { return m_channels > 0 ? m_channels * 4 : 4; }

  static void setString(IAIMPFileInfo* fi, int prop, const std::wstring& v) {
    if (v.empty() || !g_core) return;
    IAIMPString* s = nullptr;
    if (SUCCEEDED(g_core->CreateObject(IID_IAIMPString, reinterpret_cast<void**>(&s))) && s) {
      s->SetData(const_cast<WCHAR*>(v.c_str()), static_cast<int>(v.size()));
      fi->SetValueAsObject(prop, s);
      s->Release();
    }
  }

  // Produces the next block of PCM in m_out (m_pending frames). Returns false at end of stream.
  bool decodeFrame() {
    m_pending = 0;
    m_pendingOff = 0;
    int errors = 0;
    while (!m_eof) {
      MPEGH_DECODER_OUTPUT_INFO oi;
      MPEGH_DECODER_ERROR st =
          mpeghdecoder_getSamples(m_dec, m_settings.headphones ? m_raw.data() : m_out.data(),
                                  static_cast<uint32_t>(m_out.size()), &oi);
      if (st == MPEGH_DEC_OK) {
        if (oi.numChannels <= 0 || oi.numSamplesPerChannel <= 0) continue;
        if (m_settings.headphones) {
          if (m_srcChannels == 0) {
            const binaural::Speaker* spk = oi.numChannels == 12 ? binaural::kCicp19
                                         : oi.numChannels == 8  ? binaural::kCicp12
                                                                : nullptr;
            if (!spk) { m_eof = true; return false; }
            m_srcChannels = oi.numChannels;
            m_bin.setup(spk, m_srcChannels, oi.sampleRate, m_settings.room / 100.0f);
          } else if (oi.numChannels != m_srcChannels) {
            m_eof = true;
            return false;
          }
        }
        int outCh = m_settings.headphones ? 2 : oi.numChannels;
        if (m_channels == 0) {
          m_channels = outCh;
          m_sampleRate = oi.sampleRate;
        } else if (outCh != m_channels || oi.sampleRate != m_sampleRate) {
          m_eof = true; // output format changed mid-stream: not representable, stop cleanly
          return false;
        }
        m_lastTicks = oi.ticks;
        m_pending = oi.numSamplesPerChannel;
        if (m_settings.headphones) m_bin.process(m_raw.data(), m_out.data(), m_pending);
        else reorder(m_pending);
        return true;
      }
      if (st != MPEGH_DEC_FEED_DATA) {
        m_eof = true;
        return false;
      }
      // decoder wants more input
      if (m_haveSample) {
        MPEGH_DECODER_ERROR e = mpeghdecoder_processTimescale(
            m_dec, m_sample.rawData.data(), static_cast<uint32_t>(m_sample.rawData.size()),
            m_sampleInfo.timestamp.ptsValue(), m_sampleInfo.timestamp.timescale());
        if (e != MPEGH_DEC_OK && ++errors > 16) {
          m_eof = true;
          return false;
        }
        m_sampleInfo = m_track->nextSample(m_sample);
        m_haveSample = !m_sample.empty();
      } else if (!m_flushed) {
        m_flushed = true;
        if (mpeghdecoder_flushAndGet(m_dec) != MPEGH_DEC_OK) {
          m_eof = true;
          return false;
        }
      } else {
        m_eof = true;
        return false;
      }
    }
    return false;
  }

  // MPEG 7.1 (CICP 12) is L R C LFE Ls Rs Lb Rb; Windows expects L R C LFE Lb Rb Ls Rs.
  void reorder(int frames) {
    if (m_settings.cicp != 12 || m_channels != 8) return;
    int32_t* p = m_out.data();
    for (int i = 0; i < frames; i++, p += 8) {
      std::swap(p[4], p[6]);
      std::swap(p[5], p[7]);
    }
  }

  volatile LONG m_ref;
  std::mutex m_lock;
  Settings m_settings;
  Tags m_tags;
  std::shared_ptr<StreamShared> m_shared;
  std::unique_ptr<CIsobmffReader> m_reader;
  std::unique_ptr<CMpeghTrackReader> m_track;
  CTrackInfo m_info;
  HANDLE_MPEGH_DECODER_CONTEXT m_dec = nullptr;

  CSample m_sample;
  SSampleExtraInfo m_sampleInfo;
  bool m_haveSample = false;
  bool m_flushed = false;
  bool m_eof = false;

  std::vector<int32_t> m_out;
  std::vector<int32_t> m_raw; // decoder output before headphone rendering
  binaural::Renderer m_bin;
  int m_srcChannels = 0;
  int m_pending = 0;    // frames available in m_out
  int m_pendingOff = 0; // frames already consumed from m_out
  uint64_t m_lastTicks = 0;

  int m_channels = 0;
  int m_sampleRate = 0;
  int64_t m_totalFrames = 0;
  int64_t m_posFrames = 0;
  double m_duration = 0.0;
};

// ------------------------------------------------------------------------------------
// Decoder extension
// ------------------------------------------------------------------------------------
class CDecoderExtension : public IAIMPExtensionAudioDecoder,
                          public IAIMPExtensionAudioDecoderPriority {
 public:
  CDecoderExtension() : m_ref(1) {}
  virtual ~CDecoderExtension() {}

  HRESULT WINAPI QueryInterface(REFIID riid, void** obj) override {
    if (!obj) return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IAIMPExtensionAudioDecoder)) {
      *obj = static_cast<IAIMPExtensionAudioDecoder*>(this);
    } else if (IsEqualGUID(riid, IID_IAIMPExtensionAudioDecoderPriority)) {
      *obj = static_cast<IAIMPExtensionAudioDecoderPriority*>(this);
    } else {
      *obj = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  ULONG WINAPI AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&m_ref)); }
  ULONG WINAPI Release() override {
    LONG r = InterlockedDecrement(&m_ref);
    if (r == 0) delete this;
    return static_cast<ULONG>(r);
  }

  HRESULT WINAPI CreateDecoder(IAIMPStream* stream, DWORD /*flags*/, IAIMPErrorInfo* /*errorInfo*/,
                               IAIMPAudioDecoder** decoder) override {
    if (!stream || !decoder) return E_INVALIDARG;
    *decoder = nullptr;
    INT64 startPos = stream->GetPosition();
    CMpeghDecoder* d = nullptr;
    bool ok = false;
    try {
      d = new CMpeghDecoder();
      ok = d->open(stream);
    } catch (...) {
      ok = false;
    }
    if (!ok) {
      if (d) d->Release();
      // leave the stream as we found it so the next decoder can try
      stream->Seek(startPos, AIMP_STREAM_SEEKMODE_FROM_BEGINNING);
      return E_FAIL;
    }
    *decoder = d;
    return S_OK;
  }

  int WINAPI GetPriority() override { return 100; }

 private:
  volatile LONG m_ref;
};

// ------------------------------------------------------------------------------------
// Settings dialog (Preferences -> Plugins -> settings button of this plugin)
// ------------------------------------------------------------------------------------
enum {
  IDC_LAYOUT = 101, IDC_LOUDNESS = 102, IDC_DRC = 103, IDC_ROOM = 104, IDC_TAGS = 105,
  IDC_LANG = 106, IDS_LAYOUT = 201, IDS_ROOM = 202, IDS_LOUD = 203, IDS_DRC = 204,
  IDS_LANG = 205, IDS_NOTE = 206
};

// ------------------------------------------------------------------------------------
// Translations
//
// Dialog texts live in Langs\*.ini next to the plugin (or one folder up, for the 64-bit DLL
// in x64\). Each file is UTF-8 or UTF-16 and looks like:
//   [Info]
//   Name=Ð ÑƒÑÑÐºÐ¸Ð¹               ; shown in the Language list
//   AIMP=russian,Ñ€ÑƒÑÑÐºÐ¸Ð¹       ; AIMP language names this file is picked for (Automatic)
//   [Strings]
//   Title=Ð”ÐµÐºÐ¾Ð´ÐµÑ€ MPEG-H 3D Audio
//   ...
// Missing keys, or a missing folder, fall back to the English built into the plugin.
// ------------------------------------------------------------------------------------
struct LangFile {
  std::wstring id;                // file name without .ini, e.g. "russian"
  std::wstring name;              // display name
  std::vector<std::wstring> aimp; // lower-case AIMP language names
  std::map<std::wstring, std::wstring> strings;
};

static std::wstring utf8ToWide(const char* p, size_t n) {
  int len = MultiByteToWideChar(CP_UTF8, 0, p, static_cast<int>(n), nullptr, 0);
  std::wstring w(static_cast<size_t>(len), L'\0');
  if (len > 0) MultiByteToWideChar(CP_UTF8, 0, p, static_cast<int>(n), &w[0], len);
  return w;
}

static std::wstring readTextFile(const std::wstring& path) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return std::wstring();
  DWORD size = GetFileSize(h, nullptr), got = 0;
  std::vector<char> buf(size > 1 << 20 ? 0 : size);
  if (!buf.empty()) ReadFile(h, buf.data(), size, &got, nullptr);
  CloseHandle(h);
  buf.resize(got);
  if (buf.size() >= 2 && static_cast<uint8_t>(buf[0]) == 0xFF && static_cast<uint8_t>(buf[1]) == 0xFE)
    return std::wstring(reinterpret_cast<const wchar_t*>(buf.data() + 2), (buf.size() - 2) / 2);
  size_t off = (buf.size() >= 3 && static_cast<uint8_t>(buf[0]) == 0xEF &&
                static_cast<uint8_t>(buf[1]) == 0xBB && static_cast<uint8_t>(buf[2]) == 0xBF) ? 3 : 0;
  return utf8ToWide(buf.data() + off, buf.size() - off);
}

static std::wstring trimW(const std::wstring& s) {
  size_t a = 0, b = s.size();
  while (a < b && iswspace(s[a])) a++;
  while (b > a && iswspace(s[b - 1])) b--;
  return s.substr(a, b - a);
}

static std::wstring lowerW(std::wstring s) {
  for (auto& ch : s) ch = static_cast<wchar_t>(towlower(ch));
  return s;
}

static bool parseLangFile(const std::wstring& path, LangFile& lf) {
  std::wstring text = readTextFile(path);
  if (text.empty()) return false;
  std::wstring section;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find(L'\n', pos);
    std::wstring line = trimW(text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos));
    pos = nl == std::wstring::npos ? text.size() : nl + 1;
    if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
    if (line[0] == L'[') {
      section = lowerW(trimW(line.substr(1, line.find(L']') - 1)));
      continue;
    }
    size_t eq = line.find(L'=');
    if (eq == std::wstring::npos) continue;
    std::wstring key = trimW(line.substr(0, eq)), val = trimW(line.substr(eq + 1));
    // "\n" in a value means a line break
    for (size_t i = 0; (i = val.find(L"\\n", i)) != std::wstring::npos;) val.replace(i, 2, L"\n");
    if (section == L"info") {
      std::wstring k = lowerW(key);
      if (k == L"name") lf.name = val;
      else if (k == L"aimp") {
        std::wstring v = lowerW(val);
        size_t s = 0;
        while (s <= v.size()) {
          size_t c = v.find(L',', s);
          std::wstring part = trimW(v.substr(s, c == std::wstring::npos ? std::wstring::npos : c - s));
          if (!part.empty()) lf.aimp.push_back(part);
          if (c == std::wstring::npos) break;
          s = c + 1;
        }
      }
    } else if (section == L"strings") {
      lf.strings[key] = val;
    }
  }
  return !lf.strings.empty();
}

static std::wstring langsDir() {
  std::wstring here = dllDir() + L"Langs\\";
  DWORD a = GetFileAttributesW(here.c_str());
  if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return here;
  return dllDir() + L"..\\Langs\\"; // 64-bit DLL lives in x64\ below the shared folder
}

static std::vector<LangFile> loadLangFiles() {
  std::vector<LangFile> out;
  std::wstring dir = langsDir();
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"*.ini").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    LangFile lf;
    std::wstring file = fd.cFileName;
    lf.id = lowerW(file.substr(0, file.size() - 4));
    if (!parseLangFile(dir + file, lf)) continue;
    if (lf.name.empty()) lf.name = file.substr(0, file.size() - 4);
    out.push_back(std::move(lf));
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  std::sort(out.begin(), out.end(), [](const LangFile& a, const LangFile& b) {
    if (a.id == L"english" || b.id == L"english") return a.id == L"english";
    return lowerW(a.name) < lowerW(b.name);
  });
  return out;
}

// AIMP's interface language name (Preferences -> Language), lower-case; empty outside AIMP.
static std::wstring aimpLanguageName() {
  std::wstring name;
  if (!g_core) return name;
  IAIMPServiceMUI* mui = nullptr;
  if (SUCCEEDED(g_core->QueryInterface(IID_IAIMPServiceMUI, reinterpret_cast<void**>(&mui))) && mui) {
    IAIMPString* s = nullptr;
    if (SUCCEEDED(mui->GetName(&s)) && s) {
      name.assign(s->GetData(), static_cast<size_t>(s->GetLength()));
      s->Release();
    }
    mui->Release();
  }
  return lowerW(trimW(name));
}

// Picks the file for a Language setting: a file id, or "auto". Returns nullptr for English
// built-in text.
static const LangFile* pickLanguage(const std::vector<LangFile>& files, const std::wstring& setting) {
  // older settings stored short codes
  std::wstring want = setting == L"en" ? L"english" : setting == L"ru" ? L"russian"
                    : setting == L"es" ? L"spanish" : setting;
  if (want != L"auto" && !want.empty()) {
    for (const auto& f : files) if (f.id == want) return &f;
    return nullptr;
  }
  std::wstring name = aimpLanguageName();
  if (!name.empty()) {
    for (const auto& f : files)
      for (const auto& n : f.aimp)
        if (name == n || name.find(n) != std::wstring::npos) return &f;
    return nullptr; // AIMP language without a file: English
  }
  // outside AIMP: Windows' language
  wchar_t loc[LOCALE_NAME_MAX_LENGTH] = {0};
  if (GetUserDefaultLocaleName(loc, LOCALE_NAME_MAX_LENGTH)) {
    wchar_t eng[64] = {0};
    if (GetLocaleInfoEx(loc, LOCALE_SENGLISHLANGUAGENAME, eng, 63)) {
      std::wstring e = lowerW(eng);
      for (const auto& f : files)
        for (const auto& n : f.aimp)
          if (e == n || n.find(e) != std::wstring::npos) return &f;
    }
  }
  return nullptr;
}

// ------------------------------------------------------------------------------------
// Settings dialog (Preferences -> Plugins -> settings button of this plugin)
// ------------------------------------------------------------------------------------
struct Choice {
  const wchar_t* key;   // translation key
  const wchar_t* label; // built-in English text
  const wchar_t* value; // what is written to the ini
};

static const Choice kLayouts[] = {
    {L"Layout.Headphones", L"Headphones (3D)", L"headphones"},
    {L"Layout.Stereo", L"Stereo (2.0, speakers)", L"stereo"},
    {L"Layout.5.1", L"5.1 surround", L"5.1"},
    {L"Layout.7.1", L"7.1 surround", L"7.1"},
    {L"Layout.9.1.6", L"9.1.6 (16 channels)", L"9.1.6"},
    {L"Layout.Mono", L"Mono", L"mono"}};
static const Choice kRoom[] = {
    {L"Room.Off", L"Off (dry)", L"0"}, {L"Room.Light", L"Light", L"15"},
    {L"Room.Default", L"Default", L"30"}, {L"Room.Medium", L"Medium", L"50"},
    {L"Room.Large", L"Large", L"75"}, {L"Room.Maximum", L"Maximum", L"100"}};
static const Choice kLoudness[] = {
    {L"Loudness.Default", L"Default (-24 LKFS)", L""},
    {L"Loudness.-16", L"-16 LKFS (louder)", L"-16"},
    {L"Loudness.-18", L"-18 LKFS", L"-18"},
    {L"Loudness.-20", L"-20 LKFS", L"-20"},
    {L"Loudness.-23", L"-23 LKFS", L"-23"},
    {L"Loudness.-27", L"-27 LKFS", L"-27"},
    {L"Loudness.-31", L"-31 LKFS (quiet, full dynamics)", L"-31"}};
static const Choice kDrc[] = {
    {L"Drc.Default", L"Default (only to prevent clipping)", L""},
    {L"Drc.Off", L"Off", L"-1"},
    {L"Drc.LateNight", L"Late night", L"1"},
    {L"Drc.Noisy", L"Noisy environment", L"2"},
    {L"Drc.Limited", L"Limited playback range", L"3"},
    {L"Drc.LowLevel", L"Low playback level", L"4"},
    {L"Drc.Dialog", L"Dialog enhancement", L"5"},
    {L"Drc.General", L"General compression", L"6"}};

// Text lookup in the chosen language, English if the key is missing.
static const wchar_t* tr(const LangFile* lf, const wchar_t* key, const wchar_t* english) {
  if (lf) {
    auto it = lf->strings.find(key);
    if (it != lf->strings.end() && !it->second.empty()) return it->second.c_str();
  }
  return english;
}

static std::wstring iniValue(const wchar_t* key, const wchar_t* def) {
  wchar_t buf[128];
  GetPrivateProfileStringW(L"MPEG-H", key, def, buf, 127, iniPath().c_str());
  return trimLower(buf);
}

// Stores the values. Falls back to the profile folder when the current file is read-only.
static bool saveSettings(const std::wstring& layout, const std::wstring& loud, const std::wstring& drc,
                         const std::wstring& room, bool tags, const std::wstring& lang) {
  auto writeAll = [&](const std::wstring& path) {
    return WritePrivateProfileStringW(L"MPEG-H", L"Layout", layout.c_str(), path.c_str()) &&
           WritePrivateProfileStringW(L"MPEG-H", L"HeadphoneRoom", room.c_str(), path.c_str()) &&
           WritePrivateProfileStringW(L"MPEG-H", L"ReadTags", tags ? L"1" : L"0", path.c_str()) &&
           WritePrivateProfileStringW(L"MPEG-H", L"TargetLoudness", loud.c_str(), path.c_str()) &&
           WritePrivateProfileStringW(L"MPEG-H", L"DrcEffect", drc.c_str(), path.c_str()) &&
           WritePrivateProfileStringW(L"MPEG-H", L"Language", lang.c_str(), path.c_str());
  };
  std::wstring cur = iniPath();
  if (writeAll(cur)) {
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, cur.c_str()); // flush
    return true;
  }
  if (g_profileIni.empty() || g_profileIni == cur) return false;
  if (!fileExists(g_profileIni)) {
    if (!CopyFileW(cur.c_str(), g_profileIni.c_str(), TRUE)) writeDefaultIni(g_profileIni);
  }
  if (!writeAll(g_profileIni)) return false;
  WritePrivateProfileStringW(nullptr, nullptr, nullptr, g_profileIni.c_str());
  g_iniPath = g_profileIni;
  return true;
}

struct DlgState {
  std::wstring layout, loud, drc, room, lang;
  std::vector<LangFile> files;
  std::vector<std::wstring> langValues; // Language list entries: "auto", then file ids
};

// Fills a list in the chosen language and selects `current` (an ini value).
template <size_t N>
static void fillCombo(HWND dlg, int id, const Choice (&items)[N], const std::wstring& current,
                      const wchar_t* customFmt, const LangFile* lf) {
  HWND cb = GetDlgItem(dlg, id);
  SendMessageW(cb, CB_RESETCONTENT, 0, 0);
  int sel = -1;
  for (size_t i = 0; i < N; i++) {
    SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(lf, items[i].key, items[i].label)));
    if (current == items[i].value) sel = static_cast<int>(i);
  }
  if (sel < 0) { // value typed by hand into the ini: keep it selectable
    wchar_t label[200];
    _snwprintf(label, 199, customFmt, current.c_str());
    label[199] = 0;
    sel = static_cast<int>(SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label)));
  }
  SendMessageW(cb, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
}

template <size_t N>
static std::wstring comboValue(HWND dlg, int id, const Choice (&items)[N], const std::wstring& current) {
  LRESULT sel = SendMessageW(GetDlgItem(dlg, id), CB_GETCURSEL, 0, 0);
  if (sel >= 0 && static_cast<size_t>(sel) < N) return items[sel].value;
  return current; // the "custom" entry
}

// Room sound only matters for Headphones (3D): grey it out for the other layouts.
static void updateRoomEnabled(HWND dlg) {
  LRESULT sel = SendMessageW(GetDlgItem(dlg, IDC_LAYOUT), CB_GETCURSEL, 0, 0);
  bool hp = sel >= 0 && static_cast<size_t>(sel) < sizeof(kLayouts) / sizeof(kLayouts[0]) &&
            wcscmp(kLayouts[sel].value, L"headphones") == 0;
  EnableWindow(GetDlgItem(dlg, IDC_ROOM), hp);
}

// Puts every text of the dialog into the language of st->lang, keeping the current choices.
static void applyLanguage(HWND dlg, DlgState* st) {
  const LangFile* lf = pickLanguage(st->files, st->lang);
  const wchar_t* custom = tr(lf, L"Custom", L"Custom (%ls)");
  SetWindowTextW(dlg, tr(lf, L"Title", L"MPEG-H 3D Audio decoder"));
  SetDlgItemTextW(dlg, IDS_LAYOUT, tr(lf, L"Label.Layout", L"Output layout:"));
  SetDlgItemTextW(dlg, IDS_ROOM, tr(lf, L"Label.Room", L"Room sound (3D):"));
  SetDlgItemTextW(dlg, IDS_LOUD, tr(lf, L"Label.Loudness", L"Loudness target:"));
  SetDlgItemTextW(dlg, IDS_DRC, tr(lf, L"Label.Drc", L"Dynamic range control:"));
  SetDlgItemTextW(dlg, IDS_LANG, tr(lf, L"Label.Language", L"Language:"));
  SetDlgItemTextW(dlg, IDC_TAGS, tr(lf, L"ReadTags", L"Read title, artist and album tags from the file"));
  SetDlgItemTextW(dlg, IDS_NOTE, tr(lf, L"Note",
      L"Changes apply to the next track you start. Room sound is used only by Headphones (3D). "
      L"Surround layouts need a sound device set up for that many channels."));
  SetDlgItemTextW(dlg, IDOK, tr(lf, L"OK", L"OK"));
  SetDlgItemTextW(dlg, IDCANCEL, tr(lf, L"Cancel", L"Cancel"));
  fillCombo(dlg, IDC_LAYOUT, kLayouts, st->layout, tr(lf, L"CustomLayout", L"Custom layout (CICP %ls)"), lf);
  fillCombo(dlg, IDC_ROOM, kRoom, st->room, custom, lf);
  fillCombo(dlg, IDC_LOUDNESS, kLoudness, st->loud, tr(lf, L"CustomLkfs", L"Custom (%ls LKFS)"), lf);
  fillCombo(dlg, IDC_DRC, kDrc, st->drc, custom, lf);

  // Language list: Automatic, then every file by its own name
  HWND cb = GetDlgItem(dlg, IDC_LANG);
  SendMessageW(cb, CB_RESETCONTENT, 0, 0);
  st->langValues.assign(1, L"auto");
  SendMessageW(cb, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(tr(lf, L"Language.Auto", L"Automatic (as in AIMP)")));
  bool haveEnglish = false;
  for (const auto& f : st->files) {
    st->langValues.push_back(f.id);
    SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(f.name.c_str()));
    if (f.id == L"english") haveEnglish = true;
  }
  if (!haveEnglish) { // built-in English is always available
    st->langValues.push_back(L"english");
    SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English"));
  }
  int sel = 0;
  for (size_t i = 0; i < st->langValues.size(); i++)
    if (st->langValues[i] == st->lang) sel = static_cast<int>(i);
  SendMessageW(cb, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
  updateRoomEnabled(dlg);
}

// Reads the current choices from the controls back into the state.
static void readChoices(HWND dlg, DlgState* st) {
  st->layout = comboValue(dlg, IDC_LAYOUT, kLayouts, st->layout);
  st->room = comboValue(dlg, IDC_ROOM, kRoom, st->room);
  st->loud = comboValue(dlg, IDC_LOUDNESS, kLoudness, st->loud);
  st->drc = comboValue(dlg, IDC_DRC, kDrc, st->drc);
  LRESULT sel = SendMessageW(GetDlgItem(dlg, IDC_LANG), CB_GETCURSEL, 0, 0);
  if (sel >= 0 && static_cast<size_t>(sel) < st->langValues.size()) st->lang = st->langValues[sel];
}

static INT_PTR CALLBACK settingsDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_INITDIALOG: {
      DlgState* st = reinterpret_cast<DlgState*>(lp);
      SetWindowLongPtrW(dlg, DWLP_USER, static_cast<LONG_PTR>(lp));
      st->files = loadLangFiles();
      st->layout = iniValue(L"Layout", L"headphones");
      if (st->layout.empty() || st->layout == L"2.0") st->layout = L"stereo";
      if (st->layout == L"1.0") st->layout = L"mono";
      st->loud = iniValue(L"TargetLoudness", L"");
      st->drc = iniValue(L"DrcEffect", L"");
      st->room = iniValue(L"HeadphoneRoom", L"30");
      if (st->room.empty()) st->room = L"30";
      st->lang = iniValue(L"Language", L"auto");
      if (st->lang.empty()) st->lang = L"auto";
      if (st->lang == L"en") st->lang = L"english";
      if (st->lang == L"ru") st->lang = L"russian";
      if (st->lang == L"es") st->lang = L"spanish";
      CheckDlgButton(dlg, IDC_TAGS, iniValue(L"ReadTags", L"1") == L"0" ? BST_UNCHECKED : BST_CHECKED);
      applyLanguage(dlg, st);
      return TRUE;
    }
    case WM_COMMAND: {
      DlgState* st = reinterpret_cast<DlgState*>(GetWindowLongPtrW(dlg, DWLP_USER));
      if (LOWORD(wp) == IDC_LAYOUT && HIWORD(wp) == CBN_SELCHANGE) {
        updateRoomEnabled(dlg);
        return TRUE;
      }
      if (LOWORD(wp) == IDC_LANG && HIWORD(wp) == CBN_SELCHANGE) { // switch the dialog right away
        readChoices(dlg, st);
        applyLanguage(dlg, st);
        return TRUE;
      }
      if (LOWORD(wp) == IDOK) {
        readChoices(dlg, st);
        bool ok = saveSettings(st->layout, st->loud, st->drc, st->room,
                               IsDlgButtonChecked(dlg, IDC_TAGS) == BST_CHECKED, st->lang);
        if (!ok) {
          const LangFile* lf = pickLanguage(st->files, st->lang);
          MessageBoxW(dlg,
                      tr(lf, L"SaveFailed",
                         L"The settings could not be saved (the settings file is write-protected)."),
                      tr(lf, L"Title", L"MPEG-H 3D Audio decoder"), MB_OK | MB_ICONWARNING);
        }
        EndDialog(dlg, ok ? IDOK : IDCANCEL);
        return TRUE;
      }
      if (LOWORD(wp) == IDCANCEL) {
        EndDialog(dlg, IDCANCEL);
        return TRUE;
      }
      break;
    }
    case WM_CLOSE:
      EndDialog(dlg, IDCANCEL);
      return TRUE;
  }
  return FALSE;
}

// In-memory dialog template (no .rc file needed).
class DlgTemplate {
 public:
  DlgTemplate(const wchar_t* title, short w, short h) {
    put<DWORD>(DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU); // style
    put<DWORD>(0);        // extended style
    put<WORD>(0);         // item count, patched in data()
    put<short>(0); put<short>(0); put<short>(w); put<short>(h);
    put<WORD>(0);         // no menu
    put<WORD>(0);         // default class
    str(title);
    put<WORD>(8);         // font size
    str(L"MS Shell Dlg");
  }
  void item(WORD cls, const wchar_t* text, WORD id, DWORD style, short x, short y, short w, short h) {
    while (m_buf.size() % 4) m_buf.push_back(0);
    put<DWORD>(style | WS_CHILD | WS_VISIBLE);
    put<DWORD>(0);
    put<short>(x); put<short>(y); put<short>(w); put<short>(h);
    put<WORD>(id);
    put<WORD>(0xFFFF); put<WORD>(cls);
    str(text);
    put<WORD>(0); // no creation data
    m_count++;
  }
  const DLGTEMPLATE* data() {
    memcpy(m_buf.data() + 8, &m_count, sizeof(WORD));
    return reinterpret_cast<const DLGTEMPLATE*>(m_buf.data());
  }

 private:
  template <class T> void put(T v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    m_buf.insert(m_buf.end(), p, p + sizeof(T));
  }
  void str(const wchar_t* s) {
    for (;; s++) { put<WORD>(static_cast<WORD>(*s)); if (!*s) break; }
  }
  std::vector<uint8_t> m_buf;
  WORD m_count = 0;
};

// Texts are filled in by applyLanguage(), so the template carries no words of its own.
static void showSettingsDialog(HWND parent) {
  const WORD BUTTON = 0x0080, STATIC = 0x0082, COMBO = 0x0085;
  const DWORD comboStyle = CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP;
  const short W = 290, LW = 108, CX = 120, CW = 160;
  DlgTemplate t(L"MPEG-H 3D Audio decoder", W, 188);
  t.item(STATIC, L"", IDS_LAYOUT, SS_LEFT, 10, 12, LW, 10);
  t.item(COMBO, L"", IDC_LAYOUT, comboStyle, CX, 10, CW, 120);
  t.item(STATIC, L"", IDS_ROOM, SS_LEFT, 10, 32, LW, 10);
  t.item(COMBO, L"", IDC_ROOM, comboStyle, CX, 30, CW, 120);
  t.item(STATIC, L"", IDS_LOUD, SS_LEFT, 10, 52, LW, 10);
  t.item(COMBO, L"", IDC_LOUDNESS, comboStyle, CX, 50, CW, 140);
  t.item(STATIC, L"", IDS_DRC, SS_LEFT, 10, 72, LW, 10);
  t.item(COMBO, L"", IDC_DRC, comboStyle, CX, 70, CW, 140);
  t.item(STATIC, L"", IDS_LANG, SS_LEFT, 10, 92, LW, 10);
  t.item(COMBO, L"", IDC_LANG, comboStyle, CX, 90, CW, 100);
  t.item(BUTTON, L"", IDC_TAGS, BS_AUTOCHECKBOX | WS_TABSTOP, 10, 110, W - 20, 12);
  t.item(STATIC, L"", IDS_NOTE, SS_LEFT, 10, 128, W - 20, 34);
  t.item(BUTTON, L"", IDOK, BS_DEFPUSHBUTTON | WS_TABSTOP, W - 130, 168, 55, 14);
  t.item(BUTTON, L"", IDCANCEL, BS_PUSHBUTTON | WS_TABSTOP, W - 65, 168, 55, 14);
  DlgState st;
  DialogBoxIndirectParamW(g_module, t.data(), parent, settingsDlgProc, reinterpret_cast<LPARAM>(&st));
}

// ------------------------------------------------------------------------------------
// Plugin
// ------------------------------------------------------------------------------------
class CPlugin : public IAIMPPlugin, public IAIMPExternalSettingsDialog {
 public:
  CPlugin() : m_ref(1) {}
  virtual ~CPlugin() {}

  HRESULT WINAPI QueryInterface(REFIID riid, void** obj) override {
    if (!obj) return E_POINTER;
    if (IsEqualGUID(riid, IID_IUnknown)) {
      *obj = static_cast<IAIMPPlugin*>(this);
      AddRef();
      return S_OK;
    }
    if (IsEqualGUID(riid, IID_IAIMPExternalSettingsDialog)) {
      *obj = static_cast<IAIMPExternalSettingsDialog*>(this);
      AddRef();
      return S_OK;
    }
    *obj = nullptr;
    return E_NOINTERFACE;
  }

  // IAIMPExternalSettingsDialog
  void WINAPI Show(HWND parent) override {
    try {
      showSettingsDialog(parent);
    } catch (...) {
    }
  }
  ULONG WINAPI AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&m_ref)); }
  ULONG WINAPI Release() override {
    LONG r = InterlockedDecrement(&m_ref);
    if (r == 0) delete this;
    return static_cast<ULONG>(r);
  }

  PWCHAR WINAPI InfoGet(int index) override {
    switch (index) {
      case AIMP_PLUGIN_INFO_NAME: return const_cast<PWCHAR>(kPluginName);
      case AIMP_PLUGIN_INFO_AUTHOR: return const_cast<PWCHAR>(kPluginAuthor);
      case AIMP_PLUGIN_INFO_SHORT_DESCRIPTION: return const_cast<PWCHAR>(kPluginShort);
      case AIMP_PLUGIN_INFO_FULL_DESCRIPTION: return const_cast<PWCHAR>(kPluginFull);
    }
    return nullptr;
  }

  DWORD WINAPI InfoGetCategories() override { return AIMP_PLUGIN_CATEGORY_DECODERS; }

  HRESULT WINAPI Initialize(IAIMPCore* core) override {
    if (!core) return E_INVALIDARG;
    try {
      disableLogging();
    } catch (...) {
    }
    g_core = core;
    g_core->AddRef();
    resolveIniPath(core);
    CDecoderExtension* ext = new CDecoderExtension();
    HRESULT hr = core->RegisterExtension(IID_IAIMPServiceAudioDecoders,
                                         static_cast<IAIMPExtensionAudioDecoder*>(ext));
    ext->Release();
    if (FAILED(hr)) {
      g_core->Release();
      g_core = nullptr;
    }
    return hr;
  }

  HRESULT WINAPI Finalize() override {
    if (g_core) {
      g_core->Release();
      g_core = nullptr;
    }
    return S_OK;
  }

  void WINAPI SystemNotification(int /*notifyId*/, IUnknown* /*data*/) override {}

 private:
  volatile LONG m_ref;
};

extern "C" __declspec(dllexport) HRESULT WINAPI AIMPPluginGetHeader(IAIMPPlugin** header) {
  if (!header) return E_POINTER;
  try {
    *header = new CPlugin();
    return S_OK;
  } catch (...) {
    *header = nullptr;
    return E_FAIL;
  }
}

extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = inst;
    DisableThreadLibraryCalls(inst);
  }
  return TRUE;
}
