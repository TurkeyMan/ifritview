#include "pipeline.h"

#include "decoders/decoder.h"
#include "encoders/encoder.h"
#include "jpeg_lossless.h"
#include "metadata/file_metadata.h"
#include "plugins.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace
{

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::vector<std::string> split(const std::string& s, char by)
{
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t at; (at = s.find(by, start)) != std::string::npos; start = at + 1)
        parts.push_back(s.substr(start, at - start));
    parts.push_back(s.substr(start));
    return parts;
}

bool integer(const std::string& s, int& v)
{
    if (s.empty())
        return false;
    char* end = nullptr;
    const long n = std::strtol(s.c_str(), &end, 10);
    if (*end || n < -1000000000L || n > 1000000000L)
        return false;
    v = int(n);
    return true;
}

const Encoder* encoder_for(const std::string& path)
{
    std::string ext = lower(path.substr(path.find_last_of('.') == std::string::npos ? path.size() : path.find_last_of('.') + 1));
    if (ext == "jpeg")
        ext = "jpg";
    if (ext == "tiff")
        ext = "tif";
    for (const Encoder* e : encoders())
        if (ext == e->extension)
            return e;
    return nullptr;
}

// A setting's value as words or a number: yes/no for toggles, a choice's
// number (0-based) for choices.
bool setting_value(const EncodeSetting& s, const std::string& text, int& v, std::string& error)
{
    const std::string t = lower(text);
    if (s.kind == EncodeSetting::Kind::Toggle && (t == "yes" || t == "on" || t == "true"))
        v = 1;
    else if (s.kind == EncodeSetting::Kind::Toggle && (t == "no" || t == "off" || t == "false"))
        v = 0;
    else if (!integer(t, v))
    {
        error = std::string("setting ") + s.key + " wants a number, not \"" + text + "\"";
        return false;
    }
    if (v < s.min || v > s.max)
    {
        error = std::string("setting ") + s.key + " goes from " + std::to_string(s.min) + " to " + std::to_string(s.max);
        return false;
    }
    return true;
}

// Lossless JPEG: how much it's wanted.
enum class Lossless
{
    no,     // re-encode (the default)
    yes,    // transform the original's data; fail if that can't be done
    exact,  // as yes, and fail unless the result is exactly the image as shown
    automatic  // transform if it can be done exactly, else re-encode
};

struct Save
{
    std::string path;
    const Encoder* encoder = nullptr;
    EncodeOptions options;
    MetadataChoice metadata;
    Lossless lossless = Lossless::no;
};

bool parse_save(const std::string& arg, Save& s, std::string& error)
{
    const std::vector<std::string> parts = split(arg, ',');
    s.path = parts[0];
    if (s.path.empty())
    {
        error = "save: needs a file name (save:out.jpg)";
        return false;
    }
    s.encoder = encoder_for(s.path);
    if (!s.encoder)
    {
        error = "no format saves \"" + s.path + "\" (by its extension); --formats lists them";
        return false;
    }
    for (size_t i = 1; i < parts.size(); ++i)
    {
        const size_t eq = parts[i].find('=');
        const std::string key = parts[i].substr(0, eq), value = eq == std::string::npos ? "yes" : parts[i].substr(eq + 1);
        if (key == "metadata")
        {
            if (!parse_metadata_choice(value, s.metadata, error))
                return false;
        }
        else if (key == "lossless" && std::string(s.encoder->extension) == "jpg")
        {
            const std::string v = lower(value);
            s.lossless = v == "yes" ? Lossless::yes : v == "exact" ? Lossless::exact : v == "auto" ? Lossless::automatic : v == "no" ? Lossless::no : Lossless(-1);
            if (int(s.lossless) < 0)
            {
                error = "lossless= takes yes, exact, auto or no";
                return false;
            }
        }
        else if (const EncodeSetting* set = s.encoder->setting(key.c_str()))
        {
            int v = 0;
            if (!setting_value(*set, value, v, error))
                return false;
            s.options.values[key] = v;
        }
        else
        {
            error = std::string(s.encoder->name) + " has no setting \"" + key + "\"; --formats lists each format's settings";
            return false;
        }
    }
    return true;
}

bool do_save(const Image& img, const std::string& source_path, const Save& s, std::string& error)
{
    std::shared_ptr<const FileMetadata> metadata;
    if (img.stored && s.encoder->metadata)
    {
        SaveTarget target;
        target.width = img.width;
        target.height = img.height;
        target.pixels_changed = img.edited || img.exposure_edit != 0;
        target.drop_develop = img.is_raw;  // develop settings describe the raw, not this rendering
        metadata = std::make_shared<const FileMetadata>(prepare_metadata(*img.stored, s.metadata, target));
    }
    else if (img.stored && !img.stored->icc.empty())
    {
        FileMetadata icc_only;
        icc_only.icc = img.stored->icc;
        metadata = std::make_shared<const FileMetadata>(std::move(icc_only));
    }
    if (s.lossless != Lossless::no)
    {
        const LosslessJpeg plan = plan_lossless_jpeg(img, source_path);
        const bool usable = plan.why_not.empty() && (s.lossless != Lossless::exact || plan.differs.empty()) &&
                            (s.lossless != Lossless::automatic || plan.differs.empty());
        if (usable)
        {
            std::shared_ptr<const FileMetadata> m = metadata;
            if (m && (plan.width != img.width || plan.height != img.height) && img.stored)
            {
                // The metadata describes the file written, which is the plan's size.
                SaveTarget target;
                target.width = plan.width;
                target.height = plan.height;
                target.pixels_changed = img.edited;
                m = std::make_shared<const FileMetadata>(prepare_metadata(*img.stored, s.metadata, target));
            }
            if (!plan.differs.empty())
                std::fprintf(stderr, "%s: lossless, with %s than the image as shown (JPEG's blocks)\n", s.path.c_str(), plan.differs.c_str());
            return write_lossless_jpeg(plan, m.get(), s.path, error);
        }
        if (s.lossless != Lossless::automatic)
        {
            error = !plan.why_not.empty() ? "can't save losslessly: " + plan.why_not
                                          : "can't save losslessly and exactly: it would have " + plan.differs + " (JPEG's blocks); lossless=yes accepts that";
            return false;
        }
    }
    EncodeOptions options = s.options;
    options.metadata = metadata;
    return s.encoder->encode(img, s.path, options, error);
}

}  // namespace

int process(int argc, char** argv)
{
    if (argc < 1 || std::strcmp(argv[0], "--help") == 0)
    {
        std::fputs(help_text(), stdout);
        return argc < 1 ? 2 : 0;
    }
    const std::string in = argv[0];
    // Every step is checked before anything is done, so a typo late in the
    // line doesn't leave half the work done.
    struct Step
    {
        std::string name, arg;
        Save save;
    };
    std::vector<Step> steps;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        const size_t colon = a.find(':');
        Step st{lower(a.substr(0, colon)), colon == std::string::npos ? "" : a.substr(colon + 1), {}};
        std::string error;
        int v = 0;
        bool ok = true;
        if (st.name == "rotate")
            ok = integer(st.arg, v) && (v % 90 == 0), error = "rotate: takes 90, 180, 270 or -90 (degrees clockwise)";
        else if (st.name == "flip")
            ok = st.arg == "h" || st.arg == "v" || st.arg == "horizontal" || st.arg == "vertical", error = "flip: takes h or v";
        else if (st.name == "crop")
        {
            const std::vector<std::string> n = split(st.arg, ',');
            int x, y, w, h;
            ok = n.size() == 4 && integer(n[0], x) && integer(n[1], y) && integer(n[2], w) && integer(n[3], h) && x >= 0 && y >= 0 && w > 0 && h > 0;
            error = "crop: takes X,Y,WIDTH,HEIGHT in pixels of the image as it is at that step";
        }
        else if (st.name == "exposure")
        {
            char* end = nullptr;
            std::strtod(st.arg.c_str(), &end);
            ok = !st.arg.empty() && !*end, error = "exposure: takes a number of stops (EV), e.g. exposure:-1.5";
        }
        else if (st.name == "save")
            ok = parse_save(st.arg, st.save, error);
        else if (st.name == "info")
            ;
        else
            ok = false, error = "unknown step \"" + a + "\" (rotate, flip, crop, exposure, info, save); --help explains them";
        if (!ok)
        {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 2;
        }
        steps.push_back(std::move(st));
    }
    if (steps.empty())
    {
        std::fprintf(stderr, "nothing to do: give steps after the file, ending with save:OUT\n");
        return 2;
    }

    Image img;
    std::string error;
    if (!decode_image(in, img, error))
    {
        std::fprintf(stderr, "%s: %s\n", in.c_str(), error.c_str());
        return 1;
    }
    for (const Step& st : steps)
    {
        if (st.name == "rotate")
        {
            const int turns = ((std::atoi(st.arg.c_str()) / 90) % 4 + 4) % 4;
            if (turns)
                img.apply_orientation(turns == 1 ? 6 : turns == 2 ? 3 : 8);
        }
        else if (st.name == "flip")
            img.apply_orientation(st.arg[0] == 'h' ? 2 : 4);
        else if (st.name == "crop")
        {
            const std::vector<std::string> n = split(st.arg, ',');
            const int x = std::atoi(n[0].c_str()), y = std::atoi(n[1].c_str()), w = std::atoi(n[2].c_str()), h = std::atoi(n[3].c_str());
            if (x + w > img.width || y + h > img.height)
            {
                std::fprintf(stderr, "crop:%s goes outside the image, which is %d x %d at that step\n", st.arg.c_str(), img.width, img.height);
                return 2;
            }
            img = img.crop(x, y, w, h);
        }
        else if (st.name == "exposure")
        {
            if (!img.tonable())
            {
                std::fprintf(stderr, "exposure: only raw and HDR images take an exposure\n");
                return 2;
            }
            img.exposure_edit = std::strtod(st.arg.c_str(), nullptr);
            img.retone(img.exposure_edit);
        }
        else if (st.name == "info")
            std::printf("%d x %d, %s%s\n", img.width, img.height, img.format.c_str(), img.has_alpha ? ", alpha" : "");
        else if (st.name == "save")
        {
            if (!do_save(img, in, st.save, error))
            {
                std::fprintf(stderr, "%s: %s\n", st.save.path.c_str(), error.c_str());
                return 1;
            }
        }
    }
    return 0;
}

int formats_report()
{
    plugins::wait();  // their formats too
    std::printf("Opens (by extension, then by the file's first bytes):\n");
    for (const Decoder* d : decoders())
    {
        std::string exts;
        for (const std::string& e : d->extensions)
            exts += (exts.empty() ? "" : " ") + e;
        std::printf("  %-16s %s\n", d->name, exts.c_str());
    }
    std::printf("\nSaves (by the output's extension), with settings as key=value:\n");
    for (const Encoder* e : encoders())
    {
        std::printf("  %s (.%s)%s\n", e->name, e->extension, e->metadata ? "" : "  [can't hold metadata]");
        for (const EncodeSetting& s : e->settings)
        {
            std::string range;
            switch (s.kind)
            {
                case EncodeSetting::Kind::Range: range = std::to_string(s.min) + ".." + std::to_string(s.max); break;
                case EncodeSetting::Kind::Toggle: range = "yes|no"; break;
                case EncodeSetting::Kind::Choice:
                    for (size_t i = 0; i < s.choices.size(); ++i)
                        range += (i ? ", " : "") + std::to_string(i) + "=" + s.choices[i];
                    break;
            }
            std::string initial = s.kind == EncodeSetting::Kind::Toggle ? (s.initial ? "yes" : "no") : std::to_string(s.initial);
            std::printf("      %-12s %s: %s (default %s)\n", s.key, s.label, range.c_str(), initial.c_str());
        }
        if (std::string(e->extension) == "jpg")
            std::printf("      %-12s Lossless: yes|exact|auto|no (default no); see --help\n", "lossless");
    }
    return 0;
}

const char* help_text()
{
    return R"(IfritView: an image viewer, and image work from the command line.

USAGE
  ifritview [FILE | FOLDER]          open the viewer on a file (and its folder) or a folder
  ifritview --diff A B               compare two images in the viewer
  ifritview --process IN STEP...     read IN, do the steps in order, no window
  ifritview --convert IN OUT [KEY=VALUE...]
                                     shorthand for --process IN save:OUT,KEY=VALUE,...
  ifritview --probe FILE...          decode files and report what they hold, no window
  ifritview --formats                list what opens, what saves, and every save setting
  ifritview --plugins                list format plugins, the folders searched, and any not loaded
  ifritview --version | --help

PROCESS STEPS (applied left to right; positions are in pixels of the image as
it is at that step, after earlier turns and crops)
  rotate:DEG         turn clockwise: 90, 180, 270 (or -90)
  flip:h | flip:v    mirror left-right (h) or top-bottom (v)
  crop:X,Y,W,H       keep the W x H rectangle whose top-left corner is X,Y
  exposure:EV        raw and HDR images only: brighter (+) or darker (-), in stops
  info               print the image's size and format at this point
  save:OUT[,KEY=VALUE...]
                     write the image as it is at this point; the format comes from
                     OUT's extension. Several saves may appear, e.g. one per size.

  The image is read upright: a JPEG's EXIF orientation is applied on reading.

SAVE SETTINGS (KEY=VALUE after save:OUT, comma separated)
  Each format's own settings: --formats lists them with their ranges and defaults
  (e.g. quality=85, progressive=yes, compression=1). Toggles take yes/no; choices
  take their number. Also, for every format:
  metadata=CHOICE    what of the file's metadata (EXIF, XMP, IPTC) to keep:
                       all           everything (the default)
                       no-location   all but the GPS position and place names
                       private       all but location and personal details (camera and
                                     lens serial numbers, owner, maker notes, face tags)
                       copyright     only author, copyright and contact
                       none          nothing
                       strip:GROUPS  all but these groups, joined by +
                       only:GROUPS   only these groups, joined by +
                     GROUPS: location, personal, author, description (title, caption,
                     keywords, rating), capture (camera, lens, exposure, dates),
                     history (software, edit history, develop settings).
                     The colour profile is always kept. Orientation is written as
                     upright and the size as saved, whatever the choice.
  lossless=MODE      JPEG to JPEG only: redo the turns, flips and crops on the original's
                     compressed data instead of re-encoding, so no quality is lost.
                       no      re-encode (the default)
                       yes     lossless; JPEG's 8 or 16 px blocks may make the result
                               differ slightly: a crop's top-left moves out to a block
                               corner, and a turn can drop a partial edge block. Any
                               difference is reported on stderr.
                       exact   lossless, and fail unless the result is exactly as asked
                       auto    lossless if exact, otherwise re-encode
                     Only turns, flips and crops can be done losslessly; exposure can't.

EXAMPLES
  ifritview --process photo.jpg rotate:90 save:photo-upright.jpg,lossless=yes
  ifritview --process photo.jpg crop:100,50,1600,900 save:crop.jpg,quality=92,metadata=no-location
  ifritview --process scan.tif rotate:-90 flip:h info save:scan.png save:scan.webp,quality=80
  ifritview --process IMG_0001.CR3 exposure:+0.7 save:IMG_0001.jpg,quality=90,metadata=private
  ifritview --convert texture.dds texture.png

EXIT STATUS
  0  done
  1  a file couldn't be read or written (the reason is on stderr)
  2  the command line was wrong (the reason is on stderr); nothing was written
)";
}
