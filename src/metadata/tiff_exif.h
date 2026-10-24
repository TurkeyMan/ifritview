#pragma once
// EXIF's own directories (Exif, GPS, Interoperability) added to a TIFF
// already written, from `exif` (a TIFF structure, as FileMetadata holds it).
// Every tag goes across as it is, but the maker note, whose offsets only mean
// something in the original. Nothing to add is success.

#include <cstdint>
#include <string>
#include <vector>

bool attach_exif_directories(const std::string& path, const std::vector<uint8_t>& exif, std::string& error);
