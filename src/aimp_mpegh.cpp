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
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "apiPlugin.h"
#include "apiDecoders.h"
#include "apiFileManager.h"

#include "ilo/memory.h"
#include "mmtisobmff/types.h"
#include "mmtisobmff/logging.h"
#include "mmtisobmff/reader/input.h"
#include "mmtisobmff/reader/reader.h"
#include "mmtisobmff/reader/trackreader.h"
#include "mpeghdecoder.h"

using namespace mmt::isobmff;

static const wchar_t* kPluginName = L"MPEG-H 3D Audio decoder (mha1/mhm1) v1.1";
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
    ";   stereo  - 2.0 (default; works on headphones and any sound card)\r\n"
    ";   5.1     - L R C LFE Ls Rs\r\n"
    ";   7.1     - L R C LFE + side and back surrounds\r\n"
    ";   9.1.6   - 16 channels: L R C LFE Lb Rb Ls Rs Lw Rw + top front, top rear, top middle pairs\r\n"
    ";             (needs a 16-channel output device; AIMP itself may refuse more than 8 channels)\r\n"
    ";   mono\r\n"
    "; A raw MPEG CICP layout number is also accepted (advanced, e.g. 10 = quad).\r\n"
    "Layout=stereo\r\n"
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

  GetPrivateProfileStringW(L"MPEG-H", L"Layout", L"stereo", buf, 127, ini.c_str());
  std::wstring v = trimLower(buf);
  if (v == L"mono" || v == L"1.0") s.cicp = 1;
  else if (v == L"stereo" || v == L"2.0" || v.empty()) s.cicp = 2;
  else if (v == L"5.1") s.cicp = 6;
  else if (v == L"7.1") s.cicp = 12;
  else if (v == L"9.1.6") s.cicp = 300; // 7.1 bed + wides + top front / top rear / top middle
  else {
    int n = _wtoi(v.c_str());
    s.cicp = (n >= 1 && n <= 500) ? n : 2;
  }

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
    if (!m_dec && m_settings.cicp != 2) {
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
          mpeghdecoder_getSamples(m_dec, m_out.data(), static_cast<uint32_t>(m_out.size()), &oi);
      if (st == MPEGH_DEC_OK) {
        if (oi.numChannels <= 0 || oi.numSamplesPerChannel <= 0) continue;
        if (m_channels == 0) {
          m_channels = oi.numChannels;
          m_sampleRate = oi.sampleRate;
        } else if (oi.numChannels != m_channels || oi.sampleRate != m_sampleRate) {
          m_eof = true; // output format changed mid-stream: not representable, stop cleanly
          return false;
        }
        m_lastTicks = oi.ticks;
        m_pending = oi.numSamplesPerChannel;
        reorder(m_pending);
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
enum { IDC_LAYOUT = 101, IDC_LOUDNESS = 102, IDC_DRC = 103 };

struct Choice {
  const wchar_t* label;
  const wchar_t* value; // what is written to the ini
};

static const Choice kLayouts[] = {
    {L"Stereo (2.0)", L"stereo"}, {L"5.1 surround", L"5.1"}, {L"7.1 surround", L"7.1"},
    {L"9.1.6 (16 channels)", L"9.1.6"}, {L"Mono", L"mono"}};
static const Choice kLoudness[] = {
    {L"Default (-24 LKFS)", L""}, {L"-16 LKFS (louder)", L"-16"}, {L"-18 LKFS", L"-18"},
    {L"-20 LKFS", L"-20"}, {L"-23 LKFS", L"-23"}, {L"-27 LKFS", L"-27"},
    {L"-31 LKFS (quiet, full dynamics)", L"-31"}};
static const Choice kDrc[] = {
    {L"Default (only to prevent clipping)", L""}, {L"Off", L"-1"}, {L"Late night", L"1"},
    {L"Noisy environment", L"2"}, {L"Limited playback range", L"3"},
    {L"Low playback level", L"4"}, {L"Dialog enhancement", L"5"}, {L"General compression", L"6"}};

static std::wstring iniValue(const wchar_t* key, const wchar_t* def) {
  wchar_t buf[128];
  GetPrivateProfileStringW(L"MPEG-H", key, def, buf, 127, iniPath().c_str());
  return trimLower(buf);
}

// Stores the three values. Falls back to the profile folder when the current file is read-only.
static bool saveSettings(const std::wstring& layout, const std::wstring& loud, const std::wstring& drc) {
  auto writeAll = [&](const std::wstring& path) {
    return WritePrivateProfileStringW(L"MPEG-H", L"Layout", layout.c_str(), path.c_str()) &&
           WritePrivateProfileStringW(L"MPEG-H", L"TargetLoudness", loud.c_str(), path.c_str()) &&
           WritePrivateProfileStringW(L"MPEG-H", L"DrcEffect", drc.c_str(), path.c_str());
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

template <size_t N>
static void fillCombo(HWND dlg, int id, const Choice (&items)[N], const std::wstring& current,
                      const wchar_t* customFmt) {
  HWND cb = GetDlgItem(dlg, id);
  int sel = -1;
  for (size_t i = 0; i < N; i++) {
    SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(items[i].label));
    if (current == items[i].value) sel = static_cast<int>(i);
  }
  if (sel < 0) { // value typed by hand into the ini: keep it selectable
    wchar_t label[160];
    _snwprintf(label, 159, customFmt, current.c_str());
    label[159] = 0;
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

struct DlgState {
  std::wstring layout, loud, drc;
};

static INT_PTR CALLBACK settingsDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_INITDIALOG: {
      DlgState* st = reinterpret_cast<DlgState*>(lp);
      SetWindowLongPtrW(dlg, DWLP_USER, static_cast<LONG_PTR>(lp));
      st->layout = iniValue(L"Layout", L"stereo");
      if (st->layout.empty() || st->layout == L"2.0") st->layout = L"stereo";
      if (st->layout == L"1.0") st->layout = L"mono";
      st->loud = iniValue(L"TargetLoudness", L"");
      st->drc = iniValue(L"DrcEffect", L"");
      fillCombo(dlg, IDC_LAYOUT, kLayouts, st->layout, L"Custom layout (CICP %ls)");
      fillCombo(dlg, IDC_LOUDNESS, kLoudness, st->loud, L"Custom (%ls LKFS)");
      fillCombo(dlg, IDC_DRC, kDrc, st->drc, L"Custom (%ls)");
      return TRUE;
    }
    case WM_COMMAND:
      if (LOWORD(wp) == IDOK) {
        DlgState* st = reinterpret_cast<DlgState*>(GetWindowLongPtrW(dlg, DWLP_USER));
        bool ok = saveSettings(comboValue(dlg, IDC_LAYOUT, kLayouts, st->layout),
                               comboValue(dlg, IDC_LOUDNESS, kLoudness, st->loud),
                               comboValue(dlg, IDC_DRC, kDrc, st->drc));
        if (!ok)
          MessageBoxW(dlg, L"The settings could not be saved (the settings file is write-protected).",
                      L"MPEG-H 3D Audio decoder", MB_OK | MB_ICONWARNING);
        EndDialog(dlg, ok ? IDOK : IDCANCEL);
        return TRUE;
      }
      if (LOWORD(wp) == IDCANCEL) {
        EndDialog(dlg, IDCANCEL);
        return TRUE;
      }
      break;
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

static void showSettingsDialog(HWND parent) {
  const WORD BUTTON = 0x0080, STATIC = 0x0082, COMBO = 0x0085;
  const DWORD comboStyle = CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP;
  DlgTemplate t(L"MPEG-H 3D Audio decoder", 250, 122);
  t.item(STATIC, L"Output layout:", 0xFFFF, SS_LEFT, 10, 12, 80, 10);
  t.item(COMBO, L"", IDC_LAYOUT, comboStyle, 92, 10, 148, 120);
  t.item(STATIC, L"Loudness target:", 0xFFFF, SS_LEFT, 10, 32, 80, 10);
  t.item(COMBO, L"", IDC_LOUDNESS, comboStyle, 92, 30, 148, 140);
  t.item(STATIC, L"Dynamic range control:", 0xFFFF, SS_LEFT, 10, 52, 80, 10);
  t.item(COMBO, L"", IDC_DRC, comboStyle, 92, 50, 148, 140);
  t.item(STATIC,
         L"Changes apply to the next track you start. Surround layouts need a sound "
         L"device set up for that many channels.",
         0xFFFF, SS_LEFT, 10, 72, 230, 20);
  t.item(BUTTON, L"OK", IDOK, BS_DEFPUSHBUTTON | WS_TABSTOP, 135, 100, 50, 14);
  t.item(BUTTON, L"Cancel", IDCANCEL, BS_PUSHBUTTON | WS_TABSTOP, 190, 100, 50, 14);
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
