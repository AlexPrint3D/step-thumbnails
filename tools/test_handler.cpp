// Test harness: loads stepthumb.dll like Explorer does and saves the thumbnail as TGA.
#include <windows.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <shobjidl.h>
#include <cstdio>
static const CLSID CLSID_StepThumb = {0xae41d446, 0xce3b, 0x42c7, {0x95, 0x39, 0x75, 0x13, 0xcf, 0x65, 0xe8, 0x03}};
// IID_IThumbnailProvider is missing from MinGW's libuuid, so define it here.
static const IID IID_IThumbnailProvider_ = {0xe357fccd, 0xa995, 0x4576, {0xb0, 0x1f, 0x23, 0x46, 0x30, 0x15, 0x4e, 0x96}};
typedef HRESULT(WINAPI* GetCO)(REFCLSID, REFIID, void**);
static void SaveBitmap(HBITMAP bmp, const wchar_t* path) {
  BITMAP bm; GetObject(bmp, sizeof(bm), &bm);
  int w = bm.bmWidth, h = bm.bmHeight;
  BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
  unsigned char* px = (unsigned char*)malloc((size_t)w * h * 4);
  HDC dc = GetDC(nullptr); GetDIBits(dc, bmp, 0, h, px, &bi, DIB_RGB_COLORS); ReleaseDC(nullptr, dc);
  FILE* f = _wfopen(path, L"wb");
  unsigned char hd[18] = {0}; hd[2] = 2; hd[12] = w & 255; hd[13] = w >> 8; hd[14] = h & 255; hd[15] = h >> 8; hd[16] = 32; hd[17] = 0x28;
  fwrite(hd, 1, 18, f); fwrite(px, 1, (size_t)w * h * 4, f); fclose(f); free(px);
  wprintf(L"saved %dx%d\n", w, h);
}

// Ask the Windows Shell itself (registry + thumbnail surrogate), like Explorer does.
static int ShellMode(const wchar_t* file, const wchar_t* out, int size) {
  IShellItemImageFactory* fac = nullptr;
  HRESULT hr = SHCreateItemFromParsingName(file, nullptr, IID_PPV_ARGS(&fac));
  if (FAILED(hr)) { wprintf(L"SHCreateItemFromParsingName %08lx\n", hr); return 10; }
  SIZE sz = {size, size}; HBITMAP bmp = nullptr;
  DWORD t0 = GetTickCount();
  hr = fac->GetImage(sz, SIIGBF_THUMBNAILONLY, &bmp);
  wprintf(L"shell GetImage hr=%08lx time=%lums\n", hr, GetTickCount() - t0);
  if (FAILED(hr)) return 11;
  SaveBitmap(bmp, out);
  fac->Release();
  return 0;
}

int wmain(int argc, wchar_t** argv) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (argc >= 4 && lstrcmpW(argv[1], L"--shell") == 0) return ShellMode(argv[2], argv[3], argc > 4 ? _wtoi(argv[4]) : 256);
  if (argc < 4) { wprintf(L"usage: test_handler <dll> <in.step> <out.tga> [size]\n       test_handler --shell <in.step> <out.tga> [size]\n"); return 2; }
  HMODULE m = LoadLibraryW(argv[1]);
  if (!m) { wprintf(L"LoadLibrary failed %lu\n", GetLastError()); return 3; }
  GetCO gco = (GetCO)GetProcAddress(m, "DllGetClassObject");
  IClassFactory* cf = nullptr;
  HRESULT hr = gco(CLSID_StepThumb, IID_IClassFactory, (void**)&cf);
  if (FAILED(hr)) { wprintf(L"DllGetClassObject %08lx\n", hr); return 4; }
  IInitializeWithStream* init = nullptr;
  hr = cf->CreateInstance(nullptr, IID_IInitializeWithStream, (void**)&init);
  if (FAILED(hr)) { wprintf(L"CreateInstance %08lx\n", hr); return 5; }
  IStream* stm = nullptr;
  hr = SHCreateStreamOnFileEx(argv[2], STGM_READ, 0, FALSE, nullptr, &stm);
  if (FAILED(hr)) { wprintf(L"stream %08lx\n", hr); return 6; }
  init->Initialize(stm, STGM_READ);
  IThumbnailProvider* tp = nullptr;
  init->QueryInterface(IID_IThumbnailProvider_, (void**)&tp);
  UINT size = argc > 4 ? _wtoi(argv[4]) : 256;
  HBITMAP bmp = nullptr; WTS_ALPHATYPE at;
  DWORD t0 = GetTickCount();
  hr = tp->GetThumbnail(size, &bmp, &at);
  wprintf(L"GetThumbnail hr=%08lx alpha=%d time=%lums\n", hr, (int)at, GetTickCount() - t0);
  if (FAILED(hr)) return 7;
  SaveBitmap(bmp, argv[3]);
  tp->Release(); init->Release(); stm->Release();
  return 0;
}
