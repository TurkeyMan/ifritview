// VideoPlayer on Windows: Media Foundation's Media Engine plays the file (its
// own clock, audio to the default device, frames already turned upright per
// the rotation tag) in frame-server mode; a thread of ours owns the engine,
// takes each new frame as BGRA through WIC and hands it to the main thread,
// where it replaces the image's pixels.
#include "video.h"

#include "mf.h"
#include "platform.h"

#include <mfmediaengine.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace
{
constexpr UINT32 kBgra = 87;  // DXGI_FORMAT_B8G8R8A8_UNORM
}

struct VideoPlayer::Impl
{
    // Between the main thread, ours and the engine's callbacks; the main
    // thread's posts hold it weakly, so a stopped player's frames go nowhere.
    struct State
    {
        // Main thread only.
        VideoPlayer* owner = nullptr;
        std::shared_ptr<Image> target;
        bool loop = false;

        std::mutex mutex;
        std::condition_variable wake;
        bool quit = false;
        std::deque<std::function<void(IMFMediaEngine*, IMFMediaEngineEx*)>> commands;
        PixelBuffer frame;
        int frame_w = 0, frame_h = 0;
        bool frame_posted = false;

        std::atomic<double> position{0}, duration{0};
    };

    // The engine's event callback (any thread): ends and errors go to the main thread.
    class Notify final : public IMFMediaEngineNotify  // final: Release()'s delete this is of the whole object
    {
    public:
        explicit Notify(std::weak_ptr<State> state) : state_(std::move(state)) {}
        STDMETHODIMP QueryInterface(REFIID iid, void** out) override
        {
            if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFMediaEngineNotify))
            {
                *out = static_cast<IMFMediaEngineNotify*>(this);
                AddRef();
                return S_OK;
            }
            *out = nullptr;
            return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
        STDMETHODIMP_(ULONG) Release() override
        {
            const ULONG n = --refs_;
            if (!n)
                delete this;
            return n;
        }
        STDMETHODIMP EventNotify(DWORD event, DWORD_PTR, DWORD) override
        {
            if (event == MF_MEDIA_ENGINE_EVENT_ENDED || event == MF_MEDIA_ENGINE_EVENT_ERROR)
                platform::post_main(
                    [weak = state_, event]
                    {
                        auto state = weak.lock();
                        if (!state || !state->owner)
                            return;
                        if (event == MF_MEDIA_ENGINE_EVENT_ENDED && state->loop)
                            return;  // the engine loops by itself
                        state->owner->playing_ = false;  // ended: it stays on the last frame
                        if (state->owner->on_frame)
                            state->owner->on_frame();
                    });
            return S_OK;
        }

    private:
        std::weak_ptr<State> state_;
        std::atomic<ULONG> refs_{1};
    };

    std::shared_ptr<State> state = std::make_shared<State>();
    std::thread worker;

    void command(std::function<void(IMFMediaEngine*, IMFMediaEngineEx*)> fn)
    {
        {
            std::lock_guard lock(state->mutex);
            state->commands.push_back(std::move(fn));
        }
        state->wake.notify_one();
    }

    // Main thread: the newest frame becomes the image's pixels.
    static void apply(const std::weak_ptr<State>& weak)
    {
        auto state = weak.lock();
        if (!state || !state->owner)
            return;
        {
            std::lock_guard lock(state->mutex);
            state->frame_posted = false;
            if (!state->target || state->frame.empty())
                return;
            Image& img = *state->target;
            if (img.width != state->frame_w || img.height != state->frame_h)
                img.allocate(state->frame_w, state->frame_h);
            std::memcpy(img.pixels.data(), state->frame.data(), img.pixels.size());
            img.mips.clear();
        }
        if (state->owner->on_frame)
            state->owner->on_frame();
    }

    static void run(std::shared_ptr<State> state, std::wstring path, bool muted, double volume, bool loop)
    {
        mf::Thread thread;
        if (!thread.ok())
            return;
        mf::Ptr<IMFMediaEngineClassFactory> factory;
        mf::Ptr<IMFAttributes> attributes;
        mf::Ptr<IMFMediaEngine> engine;
        mf::Ptr<IMFMediaEngineEx> engine_ex;
        mf::Ptr<IWICImagingFactory> wic;
        auto* notify = new Notify(state);
        mf::create_attributes(attributes.put(), 2);
        if (attributes)
        {
            attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify);
            attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, kBgra);
        }
        notify->Release();  // the attributes hold it
        if (!attributes ||
            FAILED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put()))) ||
            FAILED(factory->CreateInstance(0, attributes.get(), engine.put())) ||
            FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()))))
            return;
        engine->QueryInterface(IID_PPV_ARGS(engine_ex.put()));
        engine->SetMuted(muted);
        engine->SetVolume(volume);
        engine->SetLoop(loop);
        BSTR url = SysAllocString(path.c_str());
        engine->SetSource(url);
        SysFreeString(url);
        engine->Play();

        mf::Ptr<IWICBitmap> bitmap;
        DWORD bitmap_w = 0, bitmap_h = 0;
        std::deque<std::function<void(IMFMediaEngine*, IMFMediaEngineEx*)>> commands;
        for (;;)
        {
            {
                std::unique_lock lock(state->mutex);
                state->wake.wait_for(lock, std::chrono::milliseconds(4), [&] { return state->quit || !state->commands.empty(); });
                if (state->quit)
                    break;
                commands.swap(state->commands);
            }
            for (auto& c : commands)
                c(engine.get(), engine_ex.get());
            commands.clear();

            LONGLONG pts = 0;
            DWORD w = 0, h = 0;
            if (engine->OnVideoStreamTick(&pts) == S_OK && SUCCEEDED(engine->GetNativeVideoSize(&w, &h)) && w && h)
            {
                if (!bitmap || w != bitmap_w || h != bitmap_h)
                {
                    wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppBGRA, WICBitmapCacheOnDemand, bitmap.put());
                    bitmap_w = w, bitmap_h = h;
                }
                const MFVideoNormalizedRect src{0, 0, 1, 1};
                const RECT dst{0, 0, LONG(w), LONG(h)};
                const MFARGB border{0, 0, 0, 255};
                mf::Ptr<IWICBitmapLock> lock;
                const WICRect all{0, 0, INT(w), INT(h)};
                UINT stride = 0, size = 0;
                BYTE* data = nullptr;
                if (bitmap && SUCCEEDED(engine->TransferVideoFrame(bitmap.get(), &src, &dst, &border)) &&
                    SUCCEEDED(bitmap->Lock(&all, WICBitmapLockRead, lock.put())) && SUCCEEDED(lock->GetStride(&stride)) &&
                    SUCCEEDED(lock->GetDataPointer(&size, &data)))
                {
                    std::lock_guard guard(state->mutex);
                    mf::copy_upright(data, int(w), int(h), ptrdiff_t(stride), 0, state->frame, state->frame_w, state->frame_h);
                    if (!state->frame_posted)
                    {
                        state->frame_posted = true;
                        platform::post_main([weak = std::weak_ptr<State>(state)] { apply(weak); });
                    }
                }
            }
            const double d = engine->GetDuration();
            state->duration = std::isfinite(d) && d > 0 ? d : 0.0;
            state->position = engine->GetCurrentTime();
        }
        engine->Shutdown();
    }
};

bool video_available()
{
    return mf::available();
}

bool video_supported()
{
    mf::Thread thread;
    return thread.ok();
}

VideoPlayer::VideoPlayer() = default;

VideoPlayer::~VideoPlayer()
{
    stop();
}

namespace
{

// Our volume is perceptual (as a slider feels); the engine's is amplitude.
double amplitude(double volume)
{
    const double v = std::clamp(volume, 0.0, 1.0);
    return v * v * v;
}

}  // namespace

void VideoPlayer::start(const std::string& path, std::shared_ptr<Image> target, bool muted, bool loop)
{
    stop();
    impl_ = new Impl;
    impl_->state->owner = this;
    impl_->state->target = std::move(target);
    impl_->state->loop = loop;
    const std::wstring wpath = mf::wide(path);
    impl_->worker = std::thread(
        [state = impl_->state, wpath, muted, loop]
        {
            platform::lower_thread_priority();
            Impl::run(state, wpath, muted, amplitude(1.0), loop);
        });
    playing_ = true;
}

void VideoPlayer::stop()
{
    if (!impl_)
        return;
    impl_->state->owner = nullptr;  // posts still on their way find nobody
    {
        std::lock_guard lock(impl_->state->mutex);
        impl_->state->quit = true;
    }
    impl_->state->wake.notify_one();
    if (impl_->worker.joinable())
        impl_->worker.join();
    delete impl_;
    impl_ = nullptr;
    playing_ = false;
}

bool VideoPlayer::active() const
{
    return impl_ != nullptr;
}

void VideoPlayer::toggle()
{
    if (!impl_)
        return;
    // At the end, play starts again from the beginning.
    const bool restart = !playing_ && duration() > 0 && position() >= duration() - 0.05;
    playing_ = !playing_;
    impl_->command(
        [restart, play = playing_](IMFMediaEngine* e, IMFMediaEngineEx*)
        {
            if (restart)
                e->SetCurrentTime(0);
            if (play)
                e->Play();
            else
                e->Pause();
        });
}

void VideoPlayer::swap(VideoPlayer& other)
{
    std::swap(impl_, other.impl_);
    std::swap(playing_, other.playing_);
    if (impl_)
        impl_->state->owner = this;
    if (other.impl_)
        other.impl_->state->owner = &other;
}

void VideoPlayer::set_playing(bool playing)
{
    if (!impl_ || playing == playing_)
        return;
    playing_ = playing;
    impl_->command(
        [playing](IMFMediaEngine* e, IMFMediaEngineEx*)
        {
            if (playing)
                e->Play();
            else
                e->Pause();
        });
}

void VideoPlayer::set_muted(bool muted)
{
    if (impl_)
        impl_->command([muted](IMFMediaEngine* e, IMFMediaEngineEx*) { e->SetMuted(muted); });
}

void VideoPlayer::set_loop(bool loop)
{
    if (!impl_)
        return;
    impl_->state->loop = loop;
    impl_->command([loop](IMFMediaEngine* e, IMFMediaEngineEx*) { e->SetLoop(loop); });
}

void VideoPlayer::set_volume(double volume)
{
    if (impl_)
        impl_->command([v = amplitude(volume)](IMFMediaEngine* e, IMFMediaEngineEx*) { e->SetVolume(v); });
}

void VideoPlayer::seek(double fraction, bool final)
{
    const double d = duration();
    if (!impl_ || d <= 0)
        return;
    const double t = std::clamp(fraction, 0.0, 1.0) * d;
    impl_->state->position = t;  // the scrub line follows at once
    impl_->command(
        [t, final](IMFMediaEngine* e, IMFMediaEngineEx* ex)
        {
            if (ex)
                ex->SetCurrentTimeEx(t, final ? MF_MEDIA_ENGINE_SEEK_MODE_NORMAL : MF_MEDIA_ENGINE_SEEK_MODE_APPROXIMATE);
            else
                e->SetCurrentTime(t);
        });
}

double VideoPlayer::position() const
{
    return impl_ ? impl_->state->position.load() : 0;
}

double VideoPlayer::duration() const
{
    return impl_ ? impl_->state->duration.load() : 0;
}
