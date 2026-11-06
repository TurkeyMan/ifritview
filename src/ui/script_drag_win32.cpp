// IFRIT_SCRIPT's drag-pane-native: the info pane's divider dragged through
// the splitter's own mouse handling, not set_sizes(), by posting its bar the
// button and moves a drag sends it (the pointer itself left alone).
#include "script_drag_win32.h"

#include <windows.h>  // (WIN32_LEAN_AND_MEAN and NOMINMAX come from the build)
#include <windowsx.h>

#include <cmath>
#include <cstdio>

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
