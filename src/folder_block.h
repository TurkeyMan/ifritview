#pragma once
// What's known of the files in one directory, packed small: a photo costs
// about ten bytes, because neighbours are so alike. Names are stored by what
// they share with the one before; extensions, cameras and sizes as short
// lists with runs of who uses which; times and positions as differences.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct FileRecord
{
    std::string name;    // the file's own name, in its directory
    int64_t size = 0;    // bytes, and the modified time in milliseconds: if
    int64_t mtime = 0;   // either differs from the file's, the record's out of date
    uint32_t width = 0, height = 0;  // as stored, 0 when not known
    uint8_t orientation = 1;         // EXIF 1..8
    int64_t taken = 0;               // capture time in seconds, local to the camera; 0 when not known
    std::string camera;
    bool has_location = false;
    double latitude = 0, longitude = 0;
};

// `records` sorted by name as a side effect.
std::vector<uint8_t> encode_folder_block(std::vector<FileRecord>& records);
// False if it's not a block this version wrote, or it's damaged.
bool decode_folder_block(const uint8_t* data, size_t size, std::vector<FileRecord>& records);
