#pragma once
// Media Foundation, Windows' own video decoders: what the player and the
// poster decoder share. Its DLLs are loaded at runtime, not linked, because
// the N editions of Windows ship without them.

#include "pixel_buffer.h"

#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <cstdint>
#include <string>

namespace mf
{

// Whether Media Foundation is installed (cheap: a module lookup, no startup).
bool available();

// Readies the calling thread: COM in this thread, Media Foundation started
// (once per process). `ok` is false if either failed.
class Thread
{
public:
    Thread();
    ~Thread();
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    bool ok() const { return ok_; }

private:
    bool ok_ = false;
    bool uninitialize_ = false;
};

// The few entry points used, from mfplat.dll and mfreadwrite.dll.
HRESULT create_attributes(IMFAttributes** out, UINT32 size);
HRESULT create_media_type(IMFMediaType** out);
HRESULT create_source_reader(const std::wstring& url, IMFAttributes* attributes, IMFSourceReader** out);

std::wstring wide(const std::string& utf8);

// COM pointer: released when it goes.
template <class T>
class Ptr
{
public:
    Ptr() = default;
    Ptr(const Ptr&) = delete;
    Ptr& operator=(const Ptr&) = delete;
    ~Ptr() { reset(); }
    void reset()
    {
        if (p_)
            p_->Release();
        p_ = nullptr;
    }
    T** put()
    {
        reset();
        return &p_;
    }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

// The clockwise turn (0, 90, 180, 270) that stands a video's frames upright,
// from its rotation tag (phones record sideways and say so).
int upright_turn(IMFMediaType* native_type);

// Copies a 32-bit BGR(X/A) frame, turned clockwise by `turn` degrees, into
// `out` as opaque BGRA; `w`/`h` become the turned size.
void copy_upright(const uint8_t* src, int src_w, int src_h, ptrdiff_t stride, int turn, PixelBuffer& out, int& w, int& h);

}  // namespace mf
