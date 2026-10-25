#pragma once
// lensfun's database (lens_database.h), kept in the user's cache rather than
// shipped: fetched whole from lensfun's site the first time a raw wants it
// (the archive lensfun-update-data fetches, so the request says nothing of
// which lenses are in use), checked for a newer one at most every 30 days,
// and read from the cache on a worker thread. Nothing is fetched when
// downloads are off; a copy already cached is still used.
//
// The cache is laid out as lensfun's own updates are, so its tools read it
// too: <cache>/lensfun/version_2/*.xml and timestamp.txt (the database's
// date, as lensfun's site gives it), beside a note of when it was last
// checked. IFRIT_LENSFUN_DB names a folder of the XML files to read instead
// (nothing fetched).

#include <functional>
#include <memory>
#include <string>

namespace lenses
{

class Database;

// Whether lensfun's site may be asked (Config::lens_downloads). Turning
// them off stops a download under way.
void allow_downloads(bool allow);

// The database, once read; null until then. Any thread.
std::shared_ptr<const Database> database();

// A raw wants it: the cached copy read the first time (on a worker thread),
// then fetched or refreshed if it's missing or due and downloads are
// allowed. `arrived` runs on the main loop each time a database comes in.
// Main thread.
void want(std::function<void()> arrived);

// Where it stands, for when a raw goes uncorrected: "not downloaded yet",
// "downloading", "downloads are off", or empty once there's a database.
std::string state();

// --lenses: each raw's camera and lens, and what the cached database
// matches and corrects of them, one line for each different shot setup
// (camera, lens) with how many files had it. Reads, never fetches.
int report(int count, char** files);

}  // namespace lenses
