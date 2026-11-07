// IFRIT_SCRIPT's drag-pane-native: the info pane's divider dragged through
// the splitter's own mouse handling, not set_sizes(), by posting its bar the
// button and moves a drag sends it (the pointer itself left alone).
#include "script_drag_win32.h"

#include <windows.h>  // (WIN32_LEAN_AND_MEAN and NOMINMAX come from the build)
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>

// Rapport's Win32 backend, for trace harnesses: a splitter's bar `index`
// (0, the first), null if it hasn't one or isn't the Win32 backend's. Borrowed
// while the splitter lives; called on the UI thread. (Linked in: IfritView
// builds rapport statically on Windows.)
extern "C" void* rapport_win32_trace_splitter_bar(rap::splitter*, unsigned index);

namespace
{

// The bar's left edge in its parent's client coordinates.
long left_in_parent(HWND bar)
{
    RECT r;
    GetWindowRect(bar, &r);
    POINT p{r.left, r.top};
    ScreenToClient(GetParent(bar), &p);
    return p.x;
}

}  // namespace

std::function<bool(int)> native_pane_drag(rap::splitter& splitter, float by, int steps)
{
    HWND bar = static_cast<HWND>(rapport_win32_trace_splitter_bar(&splitter, 0));
    if (!bar || !IsWindowVisible(bar))
    {
        std::fprintf(stderr, "drag-pane-native: no splitter bar shown\n");
        return {};
    }
    RECT client;
    GetClientRect(bar, &client);
    const long grip = client.right / 2, y = client.bottom / 2;
    const long start = left_in_parent(bar) + grip;  // where the drag holds the bar, in the parent
    const long distance = std::lround(by * GetDpiForWindow(bar) / 96.0);
    // Step 0 presses, 1..steps move (each from where the bar is now: it
    // moves with the drag), steps + 1 releases.
    return [bar, grip, y, start, distance, steps](int step) -> bool
    {
        if (!IsWindow(bar))
            return false;
        if (step == 0)
            return PostMessageW(bar, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(grip, y));
        if (GetCapture() != bar)
        {
            std::fprintf(stderr, "drag-pane-native: interrupted (the bar lost the mouse capture)\n");
            return false;
        }
        if (step > steps)
            return PostMessageW(bar, WM_LBUTTONUP, 0, MAKELPARAM(grip, y));
        const long target = start + distance * step / steps;
        const long x = target - left_in_parent(bar);
        return PostMessageW(bar, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(short(x), y));
    };
}

// -- drag-pane-native-fast ------------------------------------------------
//
// The pointer as a mouse moves it: a thread moves the target along the drag
// every `period` ms whatever the UI thread is doing, and the bar gets a move
// only once it has taken the last one, its position where the target is when
// it takes it (a hook on its message loop fills it in), as Windows coalesces
// a mouse's moves into the latest. So a slow step lets several of the
// target's go by, as a real drag does.

namespace
{

constexpr WPARAM kOurs = 0x4000;  // a bit no mouse message uses: the moves posted here,
                                  // their run in the high word (the splitter reads only lParam)
unsigned g_runs = 0;              // (the UI thread's) runs started

// One run, owned by the poll function returned for it: dropping that (the
// script's next drag replacing it, the app closing) cancels the run, joins its
// thread and takes the hook off, on the UI thread.
struct Fast
{
    HWND bar = nullptr;
    long y = 0, grip = 0;
    WPARAM id = 0;  // kOurs and this run's number, as its moves carry them
    DWORD ui = 0;
    std::atomic<long> target{0};  // in the bar's parent's client coordinates
    std::atomic<bool> pending{false}, cancel{false}, done{false}, interrupted{false};
    bool said = false;  // (the UI thread's) what it did, told once
    std::atomic<int> posted{0}, taken{0}, ticks{0};
    HHOOK hook = nullptr;
    std::thread mover;

    ~Fast();
};
Fast* g_fast = nullptr;  // the UI thread's one run (the hook's way to it)

LRESULT CALLBACK take_move(int code, WPARAM remove, LPARAM lp)
{
    MSG* m = reinterpret_cast<MSG*>(lp);
    if (code == HC_ACTION && remove == PM_REMOVE && m->message == WM_MOUSEMOVE && (m->wParam & kOurs))
    {
        if (!g_fast || m->hwnd != g_fast->bar || (m->wParam & ~WPARAM(0xFFFF)) != (g_fast->id & ~WPARAM(0xFFFF)))
        {
            m->message = WM_NULL;  // an earlier run's, its position never filled in: dropped
            return CallNextHookEx(nullptr, code, remove, lp);
        }
        m->wParam &= 0xFFFF & ~kOurs;
        m->lParam = MAKELPARAM(short(g_fast->target.load() - left_in_parent(g_fast->bar)), g_fast->y);
        g_fast->taken++;
        g_fast->pending = false;
    }
    return CallNextHookEx(nullptr, code, remove, lp);
}

Fast::~Fast()
{
    cancel = true;
    if (mover.joinable())
        mover.join();  // its waits all end on `cancel`: nothing waits on this thread
    if (g_fast == this)
        g_fast = nullptr;
    // A move of this run's still queued would reach the next run's hook, or
    // with no hook the splitter with no position: taken out while this one's
    // hook still drops it (g_fast's gone, so it's not taken as a move).
    MSG m;
    while (bar && PeekMessageW(&m, bar, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_REMOVE))
    {
    }
    if (hook)
        UnhookWindowsHookEx(hook);
}

// Whether the run can go on: not cancelled, the bar there, and (once it has
// taken a move, so its press is in) still holding the capture.
bool going(Fast& f)
{
    if (f.cancel || !IsWindow(f.bar))
        return false;
    if (f.taken > 0)
    {
        GUITHREADINFO gti{sizeof gti};
        if (!GetGUIThreadInfo(f.ui, &gti) || gti.hwndCapture != f.bar)
        {
            f.interrupted = true;
            return false;
        }
    }
    return true;
}

// The next move posted, unless one's still waiting to be taken; false if it
// can't be (the post failed).
bool post_move(Fast& f)
{
    if (f.pending.exchange(true))
        return true;
    if (!PostMessageW(f.bar, WM_MOUSEMOVE, MK_LBUTTON | f.id, 0))
    {
        f.pending = false;
        return false;
    }
    f.posted++;
    return true;
}

// Until the last move's taken; false if the run stops first (or a second
// goes by: the UI thread isn't taking them).
bool wait_taken(Fast& f)
{
    for (int ms = 0; f.pending; ++ms)
    {
        if (!going(f) || ms > 1000)
            return false;
        Sleep(1);
    }
    return true;
}

void move(Fast& f, long start, long distance, int duration_ms, int period_ms)
{
    const int n = std::max(1, duration_ms / period_ms);
    // Sleep() keeps to the system tick (15.6 ms): a high-resolution timer keeps
    // a mouse's pace.
    HANDLE tick = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -LONGLONG(period_ms) * 10000;
    bool ok = true;
    for (int i = 1; i <= n && ok; ++i)
    {
        if (tick && SetWaitableTimer(tick, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(tick, DWORD(period_ms) + 100);
        else
            Sleep(DWORD(period_ms));
        f.target = start + distance * i / n;
        f.ticks++;
        ok = going(f) && post_move(f);
    }
    if (tick)
        CloseHandle(tick);
    // The last position delivered, then the release (a press always gets one,
    // so the splitter lets go of the capture, whatever stopped the run).
    ok = ok && wait_taken(f) && post_move(f) && wait_taken(f);
    if (!ok && !f.cancel)
        f.interrupted = true;
    if (IsWindow(f.bar))
        PostMessageW(f.bar, WM_LBUTTONUP, 0, MAKELPARAM(f.grip, f.y));
    f.done = true;
}

}  // namespace

std::function<bool()> native_pane_drag_fast(rap::splitter& splitter, float by, int duration_ms, int period_ms)
{
    if (g_fast)
    {
        std::fprintf(stderr, "drag-pane-native-fast: one's already going\n");
        return {};
    }
    HWND bar = static_cast<HWND>(rapport_win32_trace_splitter_bar(&splitter, 0));
    if (!bar || !IsWindowVisible(bar))
    {
        std::fprintf(stderr, "drag-pane-native-fast: no splitter bar shown\n");
        return {};
    }
    auto f = std::make_shared<Fast>();
    f->bar = bar;
    f->ui = GetCurrentThreadId();
    f->id = kOurs | (WPARAM(++g_runs & 0x7FFF) << 16);
    RECT client;
    GetClientRect(bar, &client);
    f->grip = client.right / 2;
    f->y = client.bottom / 2;
    const long start = left_in_parent(bar) + f->grip;
    const long distance = std::lround(by * GetDpiForWindow(bar) / 96.0);
    f->target = start;
    f->hook = SetWindowsHookExW(WH_GETMESSAGE, take_move, nullptr, f->ui);
    if (!f->hook || !PostMessageW(bar, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(f->grip, f->y)))
    {
        std::fprintf(stderr, "drag-pane-native-fast: couldn't start (hook or press)\n");
        return {};  // f unhooks as it goes
    }
    g_fast = f.get();
    f->mover = std::thread(move, std::ref(*f), start, distance, duration_ms, std::max(1, period_ms));
    // Polled on the UI thread: true while the drag goes on; once it's done,
    // what it did is said (the run itself ends with the last copy of this).
    return [f]() -> bool
    {
        if (!f->done)
            return true;
        if (!f->said)
        {
            f->said = true;
            std::fprintf(stderr, "drag-pane-native-fast: %d target moves, %d posted, %d taken%s\n", f->ticks.load(), f->posted.load(), f->taken.load(),
                         f->interrupted ? "; interrupted (the bar lost the mouse capture, or stopped taking moves)" : "");
        }
        return false;
    };
}
