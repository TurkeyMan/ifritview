#pragma once
// GStreamer, loaded at runtime. The viewer compiles against GStreamer's
// headers but doesn't link it: gst_api_load() dlopens the system libraries
// and resolves the handful of functions used. Without GStreamer installed,
// videos are simply not part of the series.
//
// Include this instead of the gst headers. After it, the usual gst_* names
// call through the table, so the code reads like plain GStreamer.
#include <gst/app/gstappsink.h>
#include <gst/audio/streamvolume.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#define IFRIT_GST_FUNCTIONS(X)        \
    X(gst_init_check)                 \
    X(gst_element_factory_make)       \
    X(gst_element_get_bus)            \
    X(gst_element_set_state)          \
    X(gst_element_seek_simple)        \
    X(gst_element_query_position)     \
    X(gst_element_query_duration)     \
    X(gst_util_set_object_arg)        \
    X(gst_filename_to_uri)            \
    X(gst_bus_add_watch)              \
    X(gst_object_unref)               \
    X(gst_mini_object_unref)          \
    X(gst_message_parse_error)        \
    X(gst_caps_from_string)           \
    X(gst_sample_get_buffer)          \
    X(gst_sample_get_caps)            \
    X(gst_buffer_map)                 \
    X(gst_buffer_unmap)               \
    X(gst_tag_list_get_string)        \
    X(gst_app_sink_pull_preroll)      \
    X(gst_app_sink_pull_sample)       \
    X(gst_app_sink_try_pull_preroll)  \
    X(gst_app_sink_set_callbacks)     \
    X(gst_video_info_from_caps)       \
    X(gst_stream_volume_set_volume)

struct GstApi
{
#define IFRIT_GST_POINTER(name) decltype(&::name) name = nullptr;
    IFRIT_GST_FUNCTIONS(IFRIT_GST_POINTER)
#undef IFRIT_GST_POINTER
};

// Filled by gst_api_load(); all null until then, or if loading failed.
extern GstApi g_gst;

// Loads GStreamer once (thread-safe) and returns whether every function
// resolved and gst_init succeeded. IFRIT_NO_VIDEO=1 pretends it's missing.
bool gst_api_load();

#ifndef IFRIT_GST_NO_REDIRECT
// Route the names through the table. The header's inline helpers call the
// real symbols, so those are redirected too; the type-checked cast macros
// would call *_get_type(), so plain casts replace them.
#define gst_init_check g_gst.gst_init_check
#define gst_element_factory_make g_gst.gst_element_factory_make
#define gst_element_get_bus g_gst.gst_element_get_bus
#define gst_element_set_state g_gst.gst_element_set_state
#define gst_element_seek_simple g_gst.gst_element_seek_simple
#define gst_element_query_position g_gst.gst_element_query_position
#define gst_element_query_duration g_gst.gst_element_query_duration
#define gst_util_set_object_arg g_gst.gst_util_set_object_arg
#define gst_filename_to_uri g_gst.gst_filename_to_uri
#define gst_bus_add_watch g_gst.gst_bus_add_watch
#define gst_object_unref g_gst.gst_object_unref
#define gst_message_parse_error g_gst.gst_message_parse_error
#define gst_caps_from_string g_gst.gst_caps_from_string
#define gst_sample_get_buffer g_gst.gst_sample_get_buffer
#define gst_sample_get_caps g_gst.gst_sample_get_caps
#define gst_buffer_map g_gst.gst_buffer_map
#define gst_buffer_unmap g_gst.gst_buffer_unmap
#define gst_tag_list_get_string g_gst.gst_tag_list_get_string
#define gst_app_sink_pull_preroll g_gst.gst_app_sink_pull_preroll
#define gst_app_sink_pull_sample g_gst.gst_app_sink_pull_sample
#define gst_app_sink_try_pull_preroll g_gst.gst_app_sink_try_pull_preroll
#define gst_app_sink_set_callbacks g_gst.gst_app_sink_set_callbacks
#define gst_video_info_from_caps g_gst.gst_video_info_from_caps
#define gst_stream_volume_set_volume g_gst.gst_stream_volume_set_volume
#define gst_caps_unref(x) g_gst.gst_mini_object_unref(GST_MINI_OBJECT_CAST(x))
#define gst_sample_unref(x) g_gst.gst_mini_object_unref(GST_MINI_OBJECT_CAST(x))
#define gst_tag_list_unref(x) g_gst.gst_mini_object_unref(GST_MINI_OBJECT_CAST(x))
#undef GST_APP_SINK
#define GST_APP_SINK(x) reinterpret_cast<GstAppSink*>(x)
#undef GST_STREAM_VOLUME
#define GST_STREAM_VOLUME(x) reinterpret_cast<GstStreamVolume*>(x)
#endif
