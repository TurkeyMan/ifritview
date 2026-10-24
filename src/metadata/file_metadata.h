#pragma once
// A file's metadata as it was stored, kept from the decode so a save can
// write it back: EXIF, XMP, IPTC and the ICC colour profile, each as the
// bytes the container held. Saving edits them to fit the new file (the
// orientation is already applied, the size may have changed) and leaves out
// what the user chose to strip; the colour profile is part of the pixels'
// meaning, so it's always written.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct FileMetadata
{
    std::vector<uint8_t> exif;  // a TIFF structure, from its byte-order mark ("II*\0" / "MM\0*")
    std::vector<uint8_t> xmp;   // the XMP packet (XML)
    std::vector<uint8_t> iptc;  // IPTC-IIM datasets (0x1C ...)
    std::vector<uint8_t> icc;   // the ICC profile
    bool empty() const { return exif.empty() && xmp.empty() && iptc.empty() && icc.empty(); }
    // Fills the fields `other` has and this doesn't.
    void merge(const FileMetadata& other);
};

// Finds the metadata in a JPEG, PNG, WebP or JPEG XL container's bytes.
// Other formats' decoders fill it themselves from their libraries.
FileMetadata extract_metadata(const uint8_t* data, size_t size);

// What a save keeps. The groups cut across EXIF, XMP and IPTC alike.
struct MetadataChoice
{
    bool keep = true;           // any metadata at all
    bool location = true;       // GPS position, place names
    bool personal = true;       // camera and lens serial numbers, owner, maker notes
    bool author = true;         // creator, copyright, contact, credit
    bool description = true;    // title, caption, keywords, rating
    bool capture = true;        // camera, lens, settings, dates
    bool history = true;        // software, edit history, develop settings
};

// A choice in words, for the command line and settings: a preset ("all",
// "no-location", "private" = no location or personal details, "copyright" =
// author and contact only, "none"), or "strip:" / "only:" and groups joined
// by '+' ("strip:location+personal"; groups: location, personal, author,
// description, capture, history). False, with `error`, if it isn't one.
bool parse_metadata_choice(const std::string& text, MetadataChoice& choice, std::string& error);
// The preset a choice matches ("all", ...), or "" for any other mix.
std::string metadata_preset(const MetadataChoice& choice);
// The choice in words parse_metadata_choice reads: its preset, else "only:...".
std::string metadata_choice_text(const MetadataChoice& choice);

// Which groups the metadata has anything in (as MetadataChoice's flags;
// `keep` says whether there's any metadata at all, the profile aside).
MetadataChoice metadata_present(const FileMetadata& m);

// The file being written, so the metadata can be made to describe it.
struct SaveTarget
{
    int width = 0, height = 0;
    bool pixels_changed = false;  // cropped, rotated or re-exposed: a stored thumbnail no longer matches
    bool drop_develop = false;    // raw/XMP develop settings were applied to the pixels (or describe a raw)
};

// The metadata to write: orientation 1 (the pixels are upright), the new
// size, no stale thumbnail, and only the chosen groups. The ICC profile is
// passed through whatever the choice.
FileMetadata prepare_metadata(const FileMetadata& source, const MetadataChoice& choice, const SaveTarget& target);

struct Image;
// What's saved with `image`: its file's metadata made to describe the file
// written (`width` × `height`, or the image's own size when 0), keeping what
// `choice` keeps, for a format that holds metadata; for one that doesn't,
// the colour profile alone. Null when there's nothing. `develop_applied`:
// raw or XMP develop settings were rendered into the pixels (a raw always).
std::shared_ptr<const FileMetadata> metadata_for_save(const Image& image, const MetadataChoice& choice, bool format_holds_metadata,
                                                      bool develop_applied, int width = 0, int height = 0);

// The descriptive text fields of an EXIF block's main directory (TIFF tag
// number and text: description, make, model, software, date, artist,
// copyright), for formats that keep them as plain tags.
std::vector<std::pair<uint16_t, std::string>> exif_strings(const std::vector<uint8_t>& tiff);

// The editors behind prepare_metadata, exposed for tests.
std::vector<uint8_t> edit_exif(const std::vector<uint8_t>& tiff, const MetadataChoice& choice, const SaveTarget& target);
std::vector<uint8_t> edit_xmp(const std::vector<uint8_t>& packet, const MetadataChoice& choice, const SaveTarget& target);
std::vector<uint8_t> edit_iptc(const std::vector<uint8_t>& iim, const MetadataChoice& choice);
