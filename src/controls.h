#pragma once

#include <array>
#include <string>

// Mouse gestures and the actions they can be bound to. Keys and labels are
// used by the config file and the settings dialog respectively.

enum class Kind { Click, Drag, Wheel };

enum class Control {
    LeftClick, LeftDouble, LeftDrag,
    MiddleClick, MiddleDouble, MiddleDrag,
    RightClick, RightDouble, RightDrag,
    Wheel, CtrlWheel, ShiftWheel,
    FullscreenLeftClick, FullscreenRightClick,   // override the plain clicks in fullscreen
    Count
};

enum class Action {
    None,
    // click
    ZoomSelection,      // zoom to the selection, or back to fit when zoomed
    ToggleFullscreen,
    ZoomFit,
    ZoomOriginal,
    NextImage,
    PrevImage,
    OpenFile,
    Settings,
    Quit,
    // drag
    Pan,
    Select,
    // wheel
    Navigate,
    Zoom,
    Scroll,
};

struct ControlInfo {
    Control id;
    const char* key;
    const char* label;
    Kind kind;
};

struct ActionInfo {
    Action id;
    const char* key;
    const char* label;
    Kind kind;          // which controls may bind it; None fits every kind
};

constexpr std::array<ControlInfo, size_t(Control::Count)> kControls = {{
    {Control::LeftClick, "left_click", "Left click", Kind::Click},
    {Control::LeftDouble, "left_double_click", "Left double-click", Kind::Click},
    {Control::LeftDrag, "left_drag", "Left drag", Kind::Drag},
    {Control::MiddleClick, "middle_click", "Middle click", Kind::Click},
    {Control::MiddleDouble, "middle_double_click", "Middle double-click", Kind::Click},
    {Control::MiddleDrag, "middle_drag", "Middle drag", Kind::Drag},
    {Control::RightClick, "right_click", "Right click", Kind::Click},
    {Control::RightDouble, "right_double_click", "Right double-click", Kind::Click},
    {Control::RightDrag, "right_drag", "Right drag", Kind::Drag},
    {Control::Wheel, "wheel", "Wheel", Kind::Wheel},
    {Control::CtrlWheel, "ctrl_wheel", "Ctrl + wheel", Kind::Wheel},
    {Control::ShiftWheel, "shift_wheel", "Shift + wheel", Kind::Wheel},
    {Control::FullscreenLeftClick, "fullscreen_left_click", "Fullscreen left click", Kind::Click},
    {Control::FullscreenRightClick, "fullscreen_right_click", "Fullscreen right click", Kind::Click},
}};

constexpr std::array<ActionInfo, 15> kActions = {{
    {Action::None, "none", "Nothing", Kind::Click},
    {Action::ZoomSelection, "zoom-selection", "Zoom to selection / reset zoom", Kind::Click},
    {Action::ToggleFullscreen, "fullscreen", "Toggle fullscreen", Kind::Click},
    {Action::ZoomFit, "zoom-fit", "Fit to window", Kind::Click},
    {Action::ZoomOriginal, "zoom-original", "Actual pixels", Kind::Click},
    {Action::NextImage, "next", "Next image", Kind::Click},
    {Action::PrevImage, "prev", "Previous image", Kind::Click},
    {Action::OpenFile, "open", "Open file…", Kind::Click},
    {Action::Settings, "settings", "Settings…", Kind::Click},
    {Action::Quit, "quit", "Quit", Kind::Click},
    {Action::Pan, "pan", "Pan the image", Kind::Drag},
    {Action::Select, "select", "Select a region", Kind::Drag},
    {Action::Navigate, "navigate", "Previous / next image", Kind::Wheel},
    {Action::Zoom, "zoom", "Zoom in / out", Kind::Wheel},
    {Action::Scroll, "scroll", "Scroll the image", Kind::Wheel},
}};

using Bindings = std::array<Action, size_t(Control::Count)>;

constexpr Bindings default_bindings() {
    Bindings b{};
    b.fill(Action::None);
    b[size_t(Control::LeftClick)] = Action::ZoomSelection;
    b[size_t(Control::LeftDouble)] = Action::ToggleFullscreen;
    b[size_t(Control::LeftDrag)] = Action::Select;
    b[size_t(Control::MiddleDrag)] = Action::Pan;
    b[size_t(Control::RightDrag)] = Action::Pan;
    b[size_t(Control::Wheel)] = Action::Navigate;
    b[size_t(Control::CtrlWheel)] = Action::Zoom;
    b[size_t(Control::ShiftWheel)] = Action::Scroll;
    b[size_t(Control::FullscreenLeftClick)] = Action::NextImage;
    b[size_t(Control::FullscreenRightClick)] = Action::PrevImage;
    return b;
}

inline const ActionInfo& action_info(Action a) {
    for (const ActionInfo& i : kActions)
        if (i.id == a) return i;
    return kActions[0];
}

inline bool action_fits(Action a, Kind k) {
    return a == Action::None || action_info(a).kind == k;
}

inline Action action_from_key(const std::string& key, Action fallback) {
    for (const ActionInfo& i : kActions)
        if (key == i.key) return i.id;
    return fallback;
}
