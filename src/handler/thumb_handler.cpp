// A3D STEP Thumbnails — Windows Shell thumbnail handler (IThumbnailProvider).
//
// The DLL itself is tiny and has no CAD code. For each requested thumbnail it
// starts "stepthumb-render.exe" (located next to this DLL), pipes the STEP data
// to its stdin and reads a 32-bit TGA back from its stdout. Heavy work and any
// crash stay in that child process; it is killed after a timeout.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <propsys.h>
#include <new>
#include <string>
#include <vector>
#include <algorithm>

// {AE41D446-CE3B-42C7-9539-7513CF65E803}
static const CLSID CLSID_StepThumb = {0xae41d446, 0xce3b, 0x42c7, {0x95, 0x39, 0x75, 0x13, 0xcf, 0x65, 0xe8, 0x03}};

// IID_IThumbnailProvider is missing from MinGW's libuuid, so define it here.
static const IID IID_IThumbnailProvider_ = {0xe357fccd, 0xa995, 0x4576, {0xb0, 0x1f, 0x23, 0x46, 0x30, 0x15, 0x4e, 0x96}};
static HINSTANCE g_hInst = nullptr;
static LONG g_cRef = 0;

static const DWORD kTimeoutMs = 45000;                 // per thumbnail
static const ULONGLONG kMaxInput = 512ull * 1024 * 1024;  // 512 MB

static std::wstring ModuleDir() {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(g_hInst, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
  std::wstring p(buf, n);
  size_t s = p.find_last_of(L"\\/");
  return s == std::wstring::npos ? L"." : p.substr(0, s);
}

static HRESULT ReadAllStream(IStream* stm, std::vector<char>& out) {
  STATSTG st = {};
  if (SUCCEEDED(stm->Stat(&st, STATFLAG_NONAME)) && st.cbSize.QuadPart > kMaxInput) return E_OUTOFMEMORY;
  LARGE_INTEGER zero = {};
  stm->Seek(zero, STREAM_SEEK_SET, nullptr);
  out.clear();
  if (st.cbSize.QuadPart) out.reserve((size_t)st.cbSize.QuadPart);
  char buf[1 << 16];
  for (;;) {
    ULONG got = 0;
    HRESULT hr = stm->Read(buf, sizeof(buf), &got);
    if (FAILED(hr)) return hr;
    if (got == 0) break;
    out.insert(out.end(), buf, buf + got);
    if (out.size() > kMaxInput) return E_OUTOFMEMORY;
    if (hr == S_FALSE) break;
  }
  return out.empty() ? E_FAIL : S_OK;
}

struct PipeIO {
  HANDLE h;
  const std::vector<char>* in;
  std::vector<char>* out;
};

static DWORD WINAPI WriterThread(LPVOID p) {
  PipeIO* io = (PipeIO*)p;
  size_t off = 0;
  while (off < io->in->size()) {
    DWORD chunk = (DWORD)std::min<size_t>(io->in->size() - off, 1 << 20), wr = 0;
    if (!WriteFile(io->h, io->in->data() + off, chunk, &wr, nullptr) || wr == 0) break;
    off += wr;
  }
  CloseHandle(io->h);  // EOF for the child
  io->h = nullptr;
  return 0;
}

static DWORD WINAPI ReaderThread(LPVOID p) {
  PipeIO* io = (PipeIO*)p;
  char buf[1 << 16];
  DWORD rd = 0;
  while (ReadFile(io->h, buf, sizeof(buf), &rd, nullptr) && rd) io->out->insert(io->out->end(), buf, buf + rd);
  return 0;
}

static HRESULT RunRenderer(const std::vector<char>& step, UINT size, std::vector<char>& tga) {
  std::wstring exe = ModuleDir() + L"\\stepthumb-render.exe";
  wchar_t cmd[MAX_PATH * 3];
  wsprintfW(cmd, L"\"%s\" - - %u", exe.c_str(), size);

  SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
  HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
  if (!CreatePipe(&inR, &inW, &sa, 1 << 20)) return HRESULT_FROM_WIN32(GetLastError());
  if (!CreatePipe(&outR, &outW, &sa, 1 << 20)) {
    CloseHandle(inR); CloseHandle(inW);
    return HRESULT_FROM_WIN32(GetLastError());
  }
  SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
  HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

  STARTUPINFOW si = {sizeof(si)};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = inR;
  si.hStdOutput = outW;
  si.hStdError = nul != INVALID_HANDLE_VALUE ? nul : outW;
  PROCESS_INFORMATION pi = {};

  // Job object: the renderer dies together with the thumbnail host.
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
  }

  BOOL ok = CreateProcessW(exe.c_str(), cmd, nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED,
                           nullptr, nullptr, &si, &pi);
  DWORD err = GetLastError();
  CloseHandle(inR);
  CloseHandle(outW);
  if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  if (!ok) {
    CloseHandle(inW); CloseHandle(outR);
    if (job) CloseHandle(job);
    return HRESULT_FROM_WIN32(err);
  }
  if (job) AssignProcessToJobObject(job, pi.hProcess);
  ResumeThread(pi.hThread);

  PipeIO wio = {inW, &step, nullptr};
  PipeIO rio = {outR, nullptr, &tga};
  HANDLE tw = CreateThread(nullptr, 0, WriterThread, &wio, 0, nullptr);
  HANDLE tr = CreateThread(nullptr, 0, ReaderThread, &rio, 0, nullptr);

  DWORD wait = WaitForSingleObject(pi.hProcess, kTimeoutMs);
  DWORD code = 1;
  if (wait != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
  WaitForSingleObject(pi.hProcess, 5000);
  GetExitCodeProcess(pi.hProcess, &code);

  if (tw) { WaitForSingleObject(tw, 5000); CloseHandle(tw); }
  if (tr) { WaitForSingleObject(tr, 5000); CloseHandle(tr); }
  if (wio.h) CloseHandle(wio.h);
  CloseHandle(outR);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (job) CloseHandle(job);

  if (wait != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  if (code != 0 || tga.size() < 18) return E_FAIL;
  return S_OK;
}

static HRESULT TgaToBitmap(const std::vector<char>& tga, HBITMAP* phbmp) {
  const unsigned char* h = (const unsigned char*)tga.data();
  if (h[2] != 2 || h[16] != 32) return E_FAIL;
  int w = h[12] | (h[13] << 8), ht = h[14] | (h[15] << 8);
  size_t need = 18 + (size_t)h[0] + (size_t)w * ht * 4;
  if (w <= 0 || ht <= 0 || tga.size() < need) return E_FAIL;
  bool topDown = (h[17] & 0x20) != 0;
  const unsigned char* px = h + 18 + h[0];

  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -ht;  // top-down DIB
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bmp) return E_OUTOFMEMORY;
  for (int y = 0; y < ht; ++y) {
    int sy = topDown ? y : (ht - 1 - y);
    memcpy((unsigned char*)bits + (size_t)y * w * 4, px + (size_t)sy * w * 4, (size_t)w * 4);
  }
  *phbmp = bmp;
  return S_OK;
}

// ------------------------------------------------------------------ provider
class StepThumbProvider final : public IInitializeWithStream, public IThumbnailProvider {
 public:
  StepThumbProvider() : ref_(1), stream_(nullptr) { InterlockedIncrement(&g_cRef); }

  IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IInitializeWithStream) *ppv = static_cast<IInitializeWithStream*>(this);
    else if (riid == IID_IThumbnailProvider_) *ppv = static_cast<IThumbnailProvider*>(this);
    else { *ppv = nullptr; return E_NOINTERFACE; }
    AddRef();
    return S_OK;
  }
  IFACEMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&ref_); }
  IFACEMETHODIMP_(ULONG) Release() {
    LONG r = InterlockedDecrement(&ref_);
    if (!r) delete this;
    return r;
  }

  IFACEMETHODIMP Initialize(IStream* stm, DWORD) {
    if (stream_) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    if (!stm) return E_INVALIDARG;
    stream_ = stm;
    stream_->AddRef();
    return S_OK;
  }

  IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) {
    if (!phbmp || !pdwAlpha) return E_POINTER;
    *phbmp = nullptr;
    if (!stream_) return E_UNEXPECTED;
    std::vector<char> step, tga;
    HRESULT hr = ReadAllStream(stream_, step);
    if (FAILED(hr)) return hr;
    UINT size = cx < 16 ? 16 : (cx > 1024 ? 1024 : cx);
    hr = RunRenderer(step, size, tga);
    if (FAILED(hr)) return hr;
    hr = TgaToBitmap(tga, phbmp);
    if (SUCCEEDED(hr)) *pdwAlpha = WTSAT_ARGB;
    return hr;
  }

 private:
  ~StepThumbProvider() {
    if (stream_) stream_->Release();
    InterlockedDecrement(&g_cRef);
  }
  LONG ref_;
  IStream* stream_;
};

// ------------------------------------------------------------------ factory
class ClassFactory : public IClassFactory {
 public:
  IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IClassFactory) { *ppv = this; AddRef(); return S_OK; }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  IFACEMETHODIMP_(ULONG) AddRef() { return 2; }
  IFACEMETHODIMP_(ULONG) Release() { return 1; }
  IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) {
    if (outer) return CLASS_E_NOAGGREGATION;
    StepThumbProvider* p = new (std::nothrow) StepThumbProvider();
    if (!p) return E_OUTOFMEMORY;
    HRESULT hr = p->QueryInterface(riid, ppv);
    p->Release();
    return hr;
  }
  IFACEMETHODIMP LockServer(BOOL lock) {
    if (lock) InterlockedIncrement(&g_cRef); else InterlockedDecrement(&g_cRef);
    return S_OK;
  }
};
static ClassFactory g_factory;

// ------------------------------------------------------------------ exports
extern "C" {

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) { g_hInst = h; DisableThreadLibraryCalls(h); }
  return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
  if (!ppv) return E_POINTER;
  *ppv = nullptr;
  if (clsid != CLSID_StepThumb) return CLASS_E_CLASSNOTAVAILABLE;
  return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() { return g_cRef > 0 ? S_FALSE : S_OK; }

}  // extern "C"
