#pragma once
// The few things that differ between Linux and Windows. Everything else goes
// through GLib/GTK, which already hide the platform.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

namespace platform
{

// fopen for a UTF-8 path (Windows' fopen takes the ANSI code page).
FILE* fopen(const std::string& path, const char* mode);

// A read-only FILE* over bytes in memory, or null where the C library has no
// such thing (Windows); callers then open the file instead.
FILE* fmemopen(const uint8_t* data, size_t size);

// Size of a file in bytes, or -1.
int64_t file_size(const std::string& path);

// Let the calling thread yield to the UI and the foreground loader.
void lower_thread_priority();

// Memory the system could give us without swapping, or 0 if unknown.
size_t available_memory();

// Absolute path of the running executable.
std::string executable_path();

// Runtime libraries (GStreamer, poppler): their file names here, whether one
// is installed (a few stats, no load), loading, and symbol lookup.
#ifdef _WIN32
#define IFRIT_LIBRARY_NAME(linux_name, windows_name) windows_name
#else
#define IFRIT_LIBRARY_NAME(linux_name, windows_name) linux_name
#endif
bool library_installed(const char* name);
void* open_library(const char* name, std::string* error = nullptr);
// A library by its path (a plugin): its own folder searched for what it
// needs, and no system message box if that's missing, just `error`.
void* open_module(const std::string& path, std::string& error);
void* library_symbol(void* library, const char* symbol);
void close_library(void* library);

#ifdef _WIN32
// UTF-8 to Windows' UTF-16, and back.
std::wstring widen(const std::string& utf8);
std::string narrow(const wchar_t* utf16);
// GetLastError() as text: "The specified module could not be found (error 126)".
std::string last_error_text();
#endif

// Work for the main (UI) thread, from any thread. The front end installs its
// toolkit's way of doing that at startup; until then (and in the GTK front
// end) it goes through GLib's main loop, which the Win32 one doesn't run.
using MainDispatch = std::function<void(std::function<void()>)>;
void set_main_dispatch(MainDispatch dispatch);
void post_main(std::function<void()> fn);

// With IFRIT_TRACE set, a line on stderr stamped with the milliseconds since
// the process was created (on Windows including the loader's time before
// main), for following startup and loading. printf-style.
bool tracing();
void trace(const char* format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

// Large pixel buffers: aligned for huge pages where the system offers them.
void* allocate_pixels(size_t bytes);
void free_pixels(void* p);

}  // namespace platform
