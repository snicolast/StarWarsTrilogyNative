#include "image_io.h"

#include <windows.h>
#ifndef __APPLE__
#include <wincodec.h>
#endif

#include <cstdio>
#include <utility>

#ifndef __APPLE__   // src/platform/mac/image_io_mac.mm
namespace {

IWICImagingFactory* wic() {
    static IWICImagingFactory* factory = [] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // fine if COM is already set up
        IWICImagingFactory* f = nullptr;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
        return f;
    }();
    return factory;
}

// Path for the Win32 wide-character file APIs (the project's paths are ASCII)
std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

}  // namespace

bool png_load(const std::string& path, int& w, int& h, std::vector<uint8_t>& rgba) {
    IWICImagingFactory* f = wic();
    if (!f) return false;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapSource* conv = nullptr;
    bool ok = false;
    if (SUCCEEDED(f->CreateDecoderFromFilename(widen(path).c_str(), nullptr, GENERIC_READ,
                                               WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) &&
        SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame, &conv))) {
        UINT uw = 0, uh = 0;
        conv->GetSize(&uw, &uh);
        w = int(uw);
        h = int(uh);
        rgba.resize(size_t(uw) * uh * 4);
        ok = w > 0 && h > 0 && SUCCEEDED(conv->CopyPixels(nullptr, uw * 4, UINT(rgba.size()), rgba.data()));
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    return ok;
}

bool png_save(const std::string& path, int w, int h, const uint8_t* rgba) {
    IWICImagingFactory* f = wic();
    if (!f) return false;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = false;
    // The PNG encoder takes BGRA
    std::vector<uint8_t> bgra(rgba, rgba + size_t(w) * h * 4);
    for (size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(f->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(widen(path).c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->SetSize(UINT(w), UINT(h))) && SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
        IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA) &&
        SUCCEEDED(frame->WritePixels(UINT(h), UINT(w) * 4, UINT(bgra.size()), bgra.data())) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit())) {
        ok = true;
    }
    if (frame) frame->Release();
    if (enc) enc->Release();
    if (stream) stream->Release();
    return ok;
}
#endif

std::vector<std::string> list_files(const std::string& dir, const char* pattern) {
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA((dir + "/" + pattern).c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        out.push_back(fd.cFileName);
    } while (FindNextFileA(find, &fd));
    FindClose(find);
    return out;
}

void list_hashes(const std::string& dir, std::unordered_set<uint64_t>& out) {
    for (const std::string& name : list_files(dir, "*.png")) {
        unsigned long long hash = 0;
        if (sscanf(name.c_str(), "%16llx", &hash) == 1) out.insert(hash);
    }
}
