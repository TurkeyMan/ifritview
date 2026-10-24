#pragma once
// Image format plugins: shared libraries exporting the C interface in
// include/ifritview_plugin.h. They're found in a few folders (search_paths),
// loaded once on a background thread, and their formats join the decoder and
// encoder lists after the built-in ones (the gdk-pixbuf catch-all stays last).

#include <functional>
#include <string>
#include <vector>

struct Encoder;

namespace plugins
{

// One format a loaded plugin provides, for listing.
struct FormatSummary
{
    std::string name;
    std::string description;
    std::vector<std::string> extensions;
    bool decode = false, encode = false, metadata = false;
};

struct PluginSummary
{
    std::string path;
    std::string name;
    std::string version;
    unsigned api_version = 0;
    bool sniff = false, preview = false;  // the optional exports present
    std::vector<FormatSummary> formats;
};

// A library in a plugins folder that wasn't loaded, and why.
struct Failure
{
    std::string path;
    std::string reason;
};

// Folders searched, earliest first (whether or not they exist).
std::vector<std::string> search_paths();

// Starts loading on a low-priority thread; `done` is called on that thread
// once the plugins' formats are registered (at once if they already are).
// Later calls only add their `done`.
void load_async(std::function<void()> done = {});

// Blocks until the plugins are loaded, loading them on this thread if
// nobody has started. Cheap once they are.
void wait();

// Whether loading has finished (no wait).
bool ready();

// What was loaded and what wasn't; empty until ready().
std::vector<PluginSummary> loaded();
std::vector<Failure> failures();

// Every extension the plugins' decoders claim, in load order.
std::vector<std::string> extensions();

// The plugins' encoders, which ::encoders() lists after the built-in ones.
// Waits for loading.
const std::vector<const Encoder*>& encoders();

// `ifritview --plugins`: the folders, the plugins and their formats, and
// the failures, on stdout.
int print_report();

}  // namespace plugins
