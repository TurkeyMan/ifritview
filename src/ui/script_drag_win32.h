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
