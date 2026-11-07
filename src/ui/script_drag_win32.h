#pragma once
#include <functional>

namespace rap
{
class splitter;
}

// `splitter`'s first divider dragged `by` logical px through its native mouse
// handling, in `steps` moves: a function to call with 0 (press), 1..steps
// (moves) and steps + 1 (release), one per turn of the message loop, false
// once the drag can't go on (the bar gone, or its capture lost: the run is
// interrupted). Empty if there's no bar shown to drag. Windows only.
std::function<bool(int)> native_pane_drag(rap::splitter& splitter, float by, int steps);

// The same as a mouse drags it: the pointer's target moved every `period_ms`
// over `duration_ms` on a thread of its own, the bar given the latest one each
// time it has taken the last (Windows' coalescing of a mouse's moves). Poll
// the function on the UI thread: true while the drag goes on, false once it
// has ended (and what it did is said). It owns the run: dropping its last copy
// cancels a drag still going (its thread joined, its hook off), on the UI
// thread. Empty if there's no bar to drag, one's already going, or it can't
// start.
std::function<bool()> native_pane_drag_fast(rap::splitter& splitter, float by, int duration_ms, int period_ms);
