// Minimal stand-in for AIMP: loads the plugin DLL through the real SDK interfaces,
// opens a file through a fake IAIMPStream and writes the decoded PCM to a WAV file.
// usage: test_host.exe plugin.dll input.mp4 [out.wav]
#include <windows.h>
#include <unknwn.h>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "apiPlugin.h"
#include "apiDecoders.h"
#include "apiFileManager.h"

template <class T>
struct Ref : T {
  LONG rc = 1;
  virtual ~Ref() {}
  ULONG WINAPI AddRef() override { return ++rc; }
  ULONG WINAPI Release() override {
    LONG r = --rc;
    if (!r) delete this;
    return r;
  }
};

struct Str : Ref<IAIMPString> {
  std::wstring s;
  HRESULT WINAPI QueryInterface(REFIID, void** o) override { *o = this; AddRef(); return S_OK; }
  HRESULT WINAPI GetChar(int, WCHAR*) override { return E_NOTIMPL; }
  WCHAR* WINAPI GetData() override { return &s[0]; }
  int WINAPI GetLength() override { return (int)s.size(); }
  int WINAPI GetHashCode() override { return 0; }
  HRESULT WINAPI SetChar(int, WCHAR) override { return E_NOTIMPL; }
  HRESULT WINAPI SetData(WCHAR* c, int n) override { s.assign(c, n); return S_OK; }
  HRESULT WINAPI Add(IAIMPString*) override { return E_NOTIMPL; }
  HRESULT WINAPI Add2(WCHAR*, int) override { return E_NOTIMPL; }
  HRESULT WINAPI ChangeCase(int) override { return E_NOTIMPL; }
  HRESULT WINAPI Clone(IAIMPString**) override { return E_NOTIMPL; }
  HRESULT WINAPI Compare(IAIMPString*, int*, BOOL) override { return E_NOTIMPL; }
  HRESULT WINAPI Compare2(WCHAR*, int, int*, BOOL) override { return E_NOTIMPL; }
  HRESULT WINAPI Delete(int, int) override { return E_NOTIMPL; }
  HRESULT WINAPI Find(IAIMPString*, int*, int, int) override { return E_NOTIMPL; }
  HRESULT WINAPI Find2(WCHAR*, int, int*, int, int) override { return E_NOTIMPL; }
  HRESULT WINAPI Insert(int, IAIMPString*) override { return E_NOTIMPL; }
  HRESULT WINAPI Insert2(int, WCHAR*, int) override { return E_NOTIMPL; }
  HRESULT WINAPI Replace(IAIMPString*, IAIMPString*, int) override { return E_NOTIMPL; }
  HRESULT WINAPI Replace2(WCHAR*, int, WCHAR*, int, int) override { return E_NOTIMPL; }
  HRESULT WINAPI SubString(int, int, IAIMPString**) override { return E_NOTIMPL; }
};

struct FileStream : Ref<IAIMPStream> {
  FILE* f;
  long reads = 0;
  explicit FileStream(const char* p) { f = fopen(p, "rb"); }
  ~FileStream() { if (f) fclose(f); }
  HRESULT WINAPI QueryInterface(REFIID, void** o) override { *o = this; AddRef(); return S_OK; }
  INT64 WINAPI GetSize() override {
    INT64 c = _ftelli64(f); _fseeki64(f, 0, SEEK_END); INT64 s = _ftelli64(f); _fseeki64(f, c, SEEK_SET); return s;
  }
  HRESULT WINAPI SetSize(const INT64) override { return E_NOTIMPL; }
  INT64 WINAPI GetPosition() override { return _ftelli64(f); }
  HRESULT WINAPI Seek(const INT64 o, int m) override {
    return _fseeki64(f, o, m == 0 ? SEEK_SET : m == 1 ? SEEK_CUR : SEEK_END) ? E_FAIL : S_OK;
  }
  int WINAPI Read(unsigned char* b, unsigned int n) override { reads++; return (int)fread(b, 1, n, f); }
  HRESULT WINAPI Write(unsigned char*, unsigned int, unsigned int*) override { return E_NOTIMPL; }
};

struct FileInfo : Ref<IAIMPFileInfo> {
  std::map<int, std::wstring> v;
  HRESULT WINAPI QueryInterface(REFIID, void** o) override { *o = this; AddRef(); return S_OK; }
  HRESULT WINAPI SetValueAsFloat(int id, const double x) override { v[id] = std::to_wstring(x); return S_OK; }
  HRESULT WINAPI SetValueAsInt32(int id, int x) override { v[id] = std::to_wstring(x); return S_OK; }
  HRESULT WINAPI SetValueAsInt64(int id, const INT64 x) override { v[id] = std::to_wstring(x); return S_OK; }
  HRESULT WINAPI SetValueAsObject(int id, IUnknown* x) override {
    v[id] = static_cast<IAIMPString*>(x)->GetData(); return S_OK;
  }
  HRESULT WINAPI Assign(IAIMPFileInfo*) override { return E_NOTIMPL; }
  HRESULT WINAPI Clone(IAIMPFileInfo**) override { return E_NOTIMPL; }
};

struct Core : Ref<IAIMPCore> {
  IUnknown* ext = nullptr;
  HRESULT WINAPI QueryInterface(REFIID, void** o) override { *o = this; AddRef(); return S_OK; }
  HRESULT WINAPI CreateObject(REFIID iid, void** o) override {
    if (IsEqualGUID(iid, IID_IAIMPString)) { *o = new Str(); return S_OK; }
    return E_NOTIMPL;
  }
  HRESULT WINAPI GetPath(int, IAIMPString** o) override {
    const char* p = getenv("TEST_PROFILE");
    if (!p) return E_NOTIMPL;
    Str* s = new Str(); for (const char* c = p; *c; c++) s->s.push_back((wchar_t)*c);
    *o = s; return S_OK;
  }
  HRESULT WINAPI RegisterExtension(REFIID sid, IUnknown* e) override {
    if (!IsEqualGUID(sid, IID_IAIMPServiceAudioDecoders)) return E_FAIL;
    ext = e; e->AddRef(); return S_OK;
  }
  HRESULT WINAPI RegisterService(IUnknown*) override { return E_NOTIMPL; }
  HRESULT WINAPI UnregisterExtension(IUnknown*) override { return S_OK; }
};

static void writeWav(const char* path, const std::vector<uint8_t>& pcm, int sr, int ch) {
  FILE* f = fopen(path, "wb");
  uint32_t n = (uint32_t)pcm.size(), u;
  uint16_t s;
  fwrite("RIFF", 1, 4, f); u = 36 + n; fwrite(&u, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
  u = 16; fwrite(&u, 4, 1, f); s = 1; fwrite(&s, 2, 1, f); s = (uint16_t)ch; fwrite(&s, 2, 1, f);
  u = sr; fwrite(&u, 4, 1, f); u = sr * ch * 4; fwrite(&u, 4, 1, f);
  s = (uint16_t)(ch * 4); fwrite(&s, 2, 1, f); s = 32; fwrite(&s, 2, 1, f);
  fwrite("data", 1, 4, f); fwrite(&n, 4, 1, f); fwrite(pcm.data(), 1, n, f); fclose(f);
}

int main(int argc, char** argv) {
  if (argc < 3) { printf("usage: test_host plugin.dll input [out.wav]\n"); return 2; }
  HMODULE h = LoadLibraryA(argv[1]);
  if (!h) { printf("FAIL LoadLibrary %lu\n", GetLastError()); return 1; }
  auto getHeader = (TAIMPPluginGetHeaderProc)GetProcAddress(h, "AIMPPluginGetHeader");
  if (!getHeader) { printf("FAIL no AIMPPluginGetHeader export\n"); return 1; }
  IAIMPPlugin* plugin = nullptr;
  if (FAILED(getHeader(&plugin)) || !plugin) { printf("FAIL header\n"); return 1; }
  printf("plugin: %ls | categories=%lu\n", plugin->InfoGet(AIMP_PLUGIN_INFO_NAME), plugin->InfoGetCategories());
  Core* core = new Core();
  if (FAILED(plugin->Initialize(core)) || !core->ext) { printf("FAIL initialize\n"); return 1; }

  if (argc > 3 && !strcmp(argv[2], "--dialog")) {
    // usage: test_host plugin.dll --dialog <layoutIndex> <loudIndex> <drcIndex> | cancel
    IAIMPExternalSettingsDialog* dlg = nullptr;
    if (FAILED(plugin->QueryInterface(IID_IAIMPExternalSettingsDialog, (void**)&dlg))) { printf("FAIL QI settings dialog\n"); return 1; }
    static int idx[3]; static bool cancel; cancel = !strcmp(argv[3], "cancel");
    if (!cancel) for (int i = 0; i < 3; i++) idx[i] = atoi(argv[3 + i]);
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
      HWND w = nullptr;
      for (int i = 0; i < 200 && !w; i++) { Sleep(50); w = FindWindowW(nullptr, L"MPEG-H 3D Audio decoder"); }
      if (!w) { printf("FAIL dialog window not found\n"); ExitProcess(1); }
      for (int i = 0; i < 3; i++) {
        HWND cb = GetDlgItem(w, 101 + i);
        wchar_t txt[200] = {0}; int cur = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
        SendMessageW(cb, CB_GETLBTEXT, cur, (LPARAM)txt);
        printf("combo %d: %d items, selected %d '%ls'\n", 101 + i, (int)SendMessageW(cb, CB_GETCOUNT, 0, 0), cur, txt);
        if (!cancel && idx[i] >= 0) SendMessageW(cb, CB_SETCURSEL, idx[i], 0);
      }
      if (getenv("TEST_HOLD")) Sleep(atoi(getenv("TEST_HOLD")));
      PostMessageW(w, WM_COMMAND, cancel ? IDCANCEL : IDOK, 0);
      return 0;
    }, nullptr, 0, nullptr);
    dlg->Show(nullptr);
    printf("dialog closed\n");
    dlg->Release();
    return 0;
  }

  IAIMPExtensionAudioDecoder* ext = nullptr;
  IAIMPExtensionAudioDecoderPriority* prio = nullptr;
  if (FAILED(core->ext->QueryInterface(IID_IAIMPExtensionAudioDecoder, (void**)&ext))) { printf("FAIL QI ext\n"); return 1; }
  if (SUCCEEDED(core->ext->QueryInterface(IID_IAIMPExtensionAudioDecoderPriority, (void**)&prio))) {
    printf("priority=%d\n", prio->GetPriority()); prio->Release();
  }

  FileStream* fs = new FileStream(argv[2]);
  if (!fs->f) { printf("FAIL open input\n"); return 1; }
  fs->Seek(0, 0);
  IAIMPAudioDecoder* dec = nullptr;
  HRESULT hr = ext->CreateDecoder(fs, 0, nullptr, &dec);
  if (FAILED(hr) || !dec) {
    printf("REJECTED hr=%08lx streampos=%lld reads=%ld\n", (unsigned long)hr, (long long)fs->GetPosition(), fs->reads);
    return 3;
  }
  int sr = 0, ch = 0, fmt = 0;
  dec->GetStreamInfo(&sr, &ch, &fmt);
  printf("stream: %d Hz, %d ch, fmt=%d, size=%lld bytes (%.3f s), seekable=%d, open reads=%ld\n", sr, ch, fmt,
         (long long)dec->GetSize(), dec->GetSize() / (double)(sr * ch * 4), dec->IsSeekable(), fs->reads);
  FileInfo* fi = new FileInfo();
  BOOL got = dec->GetFileInfo(fi);
  printf("fileinfo=%d:", got);
  for (auto& kv : fi->v) printf(" [%d]=%ls", kv.first, kv.second.c_str());
  printf("\n");

  // full decode with an awkward buffer size
  std::vector<uint8_t> pcm, buf(10000 * ch * 4 / (ch * 4) * (ch * 4) + 0);
  buf.resize(4099 * ch * 4);
  for (;;) {
    int r = dec->Read(buf.data(), (int)buf.size());
    if (r <= 0) break;
    pcm.insert(pcm.end(), buf.begin(), buf.begin() + r);
  }
  printf("decoded %zu bytes = %zu frames, final pos=%lld, total reads=%ld\n", pcm.size(), pcm.size() / (ch * 4),
         (long long)dec->GetPosition(), fs->reads);
  if (argc > 3) writeWav(argv[3], pcm, sr, ch);

  // seek tests: read 2000 frames at several positions and compare with the linear decode
  size_t frames = pcm.size() / (ch * 4);
  size_t points[] = {frames / 2, frames / 3 + 517, 0, frames - 1500 > 0 ? frames - 1500 : 0, 1};
  int bad = 0;
  for (size_t p : points) {
    if (p >= frames) continue;
    if (!dec->SetPosition((INT64)p * ch * 4)) { printf("FAIL SetPosition %zu\n", p); bad++; continue; }
    INT64 pos = dec->GetPosition() / (ch * 4);
    std::vector<uint8_t> b(2000 * ch * 4);
    int got2 = 0;
    while (got2 < (int)b.size()) { int r = dec->Read(b.data() + got2, (int)b.size() - got2); if (r <= 0) break; got2 += r; }
    size_t cmpN = std::min<size_t>(got2, pcm.size() - (size_t)pos * ch * 4);
    long long maxDiff = 0;
    const int32_t* a = (const int32_t*)(pcm.data() + (size_t)pos * ch * 4);
    const int32_t* c = (const int32_t*)b.data();
    for (size_t i = 0; i < cmpN / 4; i++) { long long d = llabs((long long)a[i] - c[i]); if (d > maxDiff) maxDiff = d; }
    printf("seek to %zu -> pos %lld, read %d frames, max abs diff vs linear = %lld (%.1f dBFS)\n", p, (long long)pos,
           got2 / (ch * 4), maxDiff, maxDiff ? 20 * log10(maxDiff / 2147483648.0) : -999.0);
    if (pos != (INT64)p) bad++;
  }
  dec->Release();
  ext->Release();
  plugin->Finalize();
  plugin->Release();
  printf(bad ? "SEEK POSITION MISMATCHES: %d\n" : "OK\n", bad);
  return bad ? 1 : 0;
}
