#pragma once

#include "i18n.h"

#include <array>
#include <string>

// Mouse gestures and the actions they can be bound to. Keys and labels are
// used by the config file and the settings dialog respectively.

enum class Kind
{
    Click,
    Drag,
    Wheel
};

enum class Control
{
    LeftClick,
    LeftDouble,
    LeftDrag,
    MiddleClick,
    MiddleDouble,
    MiddleDrag,
    RightClick,
    RightDouble,
    RightDrag,
    Wheel,
    CtrlWheel,
    ShiftWheel,
    Count
};

enum class Action
{
    None,
    // click
    ZoomSelection,  // zoom to the selection, or back to fit when zoomed
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
    Exposure,
};

struct ControlInfo
{
    Control id;
    const char* key;
    const char* label;
    Kind kind;
};

struct ActionInfo
{
    Action id;
    const char* key;
    const char* label;
    Kind kind;  // which controls may bind it; None fits every kind
};

constexpr std::array<ControlInfo, size_t(Control::Count)> kControls = {{
    {Control::LeftClick, "left_click", N_("Left click"), Kind::Click},
    {Control::LeftDouble, "left_double_click", N_("Left double-click"), Kind::Click},
    {Control::LeftDrag, "left_drag", N_("Left drag"), Kind::Drag},
    {Control::MiddleClick, "middle_click", N_("Middle click"), Kind::Click},
    {Control::MiddleDouble, "middle_double_click", N_("Middle double-click"), Kind::Click},
    {Control::MiddleDrag, "middle_drag", N_("Middle drag"), Kind::Drag},
    {Control::RightClick, "right_click", N_("Right click"), Kind::Click},
    {Control::RightDouble, "right_double_click", N_("Right double-click"), Kind::Click},
    {Control::RightDrag, "right_drag", N_("Right drag"), Kind::Drag},
    {Control::Wheel, "wheel", N_("Wheel"), Kind::Wheel},
    {Control::CtrlWheel, "ctrl_wheel", N_("Ctrl + wheel"), Kind::Wheel},
    {Control::ShiftWheel, "shift_wheel", N_("Shift + wheel"), Kind::Wheel},
}};

constexpr std::array<ActionInfo, 16> kActions = {{
    {Action::None, "none", N_("Nothing"), Kind::Click},
    {Action::ZoomSelection, "zoom-selection", N_("Zoom to selection / reset zoom"), Kind::Click},
    {Action::ToggleFullscreen, "fullscreen", N_("Toggle fullscreen"), Kind::Click},
    {Action::ZoomFit, "zoom-fit", N_("Fit to window"), Kind::Click},
    {Action::ZoomOriginal, "zoom-original", N_("Actual pixels"), Kind::Click},
    {Action::NextImage, "next", N_("Next image"), Kind::Click},
    {Action::PrevImage, "prev", N_("Previous image"), Kind::Click},
    {Action::OpenFile, "open", N_("Open file…"), Kind::Click},
    {Action::Settings, "settings", N_("Settings…"), Kind::Click},
    {Action::Quit, "quit", N_("Quit"), Kind::Click},
    {Action::Pan, "pan", N_("Pan the image"), Kind::Drag},
    {Action::Select, "select", N_("Select a region"), Kind::Drag},
    {Action::Navigate, "navigate", N_("Previous / next image"), Kind::Wheel},
    {Action::Zoom, "zoom", N_("Zoom in / out"), Kind::Wheel},
    {Action::Scroll, "scroll", N_("Scroll the image"), Kind::Wheel},
    {Action::Exposure, "exposure", N_("Exposure up / down"), Kind::Wheel},
}};

using Bindings = std::array<Action, size_t(Control::Count)>;

constexpr Bindings default_bindings()
{
    Bindings b{};
    b.fill(Action::None);
    b[size_t(Control::LeftClick)] = Action::ZoomSelection;
    b[size_t(Control::LeftDouble)] = Action::ToggleFullscreen;
    b[size_t(Control::LeftDrag)] = Action::Select;
    b[size_t(Control::MiddleDrag)] = Action::Pan;
    b[size_t(Control::RightDrag)] = Action::Pan;
    b[size_t(Control::Wheel)] = Action::Navigate;
    b[size_t(Control::CtrlWheel)] = Action::Zoom;
    b[size_t(Control::ShiftWheel)] = Action::Exposure;
    return b;
}

// Fullscreen is for flicking through: clicks step, drags don't select.
constexpr Bindings default_fullscreen_bindings()
{
    Bindings b = default_bindings();
    b[size_t(Control::LeftClick)] = Action::NextImage;
    b[size_t(Control::RightClick)] = Action::PrevImage;
    b[size_t(Control::LeftDrag)] = Action::None;
    return b;
}

inline const ActionInfo& action_info(Action a)
{
    for (const ActionInfo& i : kActions)
        if (i.id == a)
            return i;
    return kActions[0];
}

inline bool action_fits(Action a, Kind k)
{
    return a == Action::None || action_info(a).kind == k;
}

inline Action action_from_key(const std::string& key, Action fallback)
{
    for (const ActionInfo& i : kActions)
        if (key == i.key)
            return i.id;
    return fallback;
}
