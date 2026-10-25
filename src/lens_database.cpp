#include "lens_database.h"

#include <glib.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <regex>

namespace lenses
{
namespace
{

float number(const char* s)
{
    return float(g_ascii_strtod(s, nullptr));  // the file's numbers, whatever the locale
}

// As lensfun compares names (_lf_strcmp): case folded, runs of white space
// as one, none at the ends.
std::string folded(const std::string& s)
{
    gchar* f = g_utf8_casefold(s.c_str(), -1);
    std::string out;
    bool space = false;
    for (const char* p = f; *p; ++p)
    {
        if (g_ascii_isspace(*p))
        {
            space = !out.empty();
            continue;
        }
        if (space)
            out += ' ';
        space = false;
        out += *p;
    }
    g_free(f);
    return out;
}

std::string first_word(const std::string& s)
{
    return s.substr(0, s.find(' '));
}

// A lens name's focal lengths and its (widest) aperture, as lensfun's
// lfLens::GuessParameters reads them out of one: 0 where it doesn't say.
struct Numbers
{
    float min_focal = 0, max_focal = 0, aperture = 0;
};

Numbers guess(const std::string& name)
{
    static const std::regex patterns[3] = {
        // [min focal]-[max focal]mm f/[min aperture]-[max aperture]
        std::regex("[^:]*?([0-9]+[0-9.]*)[-]?([0-9]+[0-9.]*)?(mm)[[:space:]]+(f/|f|1/|1:)?([0-9.]+)(-[0-9.]+)?.*"),
        // 1:[min aperture]-[max aperture] [min focal]-[max focal]mm
        std::regex(".*?1:([0-9.]+)[-]?([0-9.]+)?[[:space:]]+([0-9.]+)[-]?([0-9.]+)?(mm)?.*"),
        // [min aperture]-[max aperture]/[min focal]-[max focal]
        std::regex(".*?([0-9.]+)[-]?([0-9.]+)?[\\s]*/[\\s]*([0-9.]+)[-]?([0-9.]+)?.*"),
    };
    static const int groups[3][3] = {{1, 2, 5}, {3, 4, 1}, {3, 4, 1}};
    Numbers n;
    for (int i = 0; i < 3; ++i)
    {
        std::smatch m;
        if (!std::regex_match(name, m, patterns[i]))
            continue;
        if (m[groups[i][0]].matched)
            n.min_focal = number(m[groups[i][0]].str().c_str());
        if (m[groups[i][1]].matched)
            n.max_focal = number(m[groups[i][1]].str().c_str());
        if (m[groups[i][2]].matched)
            n.aperture = number(m[groups[i][2]].str().c_str());
        break;
    }
    return n;
}

bool converter(const std::string& name)
{
    static const std::regex magnification(".*?[0-9](\\.[0.9]+)?x.*");
    for (const char* w : {"adapter", "reducer", "booster", "extender", "converter", "magnifier"})
        if (name.find(w) != std::string::npos)
            return true;
    return std::regex_match(name, magnification);
}

// lfLens::GuessParameters: the focal range and apertures from the name, or
// from what's calibrated.
void guess_parameters(Lens& l)
{
    float minf = FLT_MAX, maxf = FLT_MIN, mina = FLT_MAX, maxa = FLT_MIN;
    if (!l.models.empty() && (!l.min_aperture || !l.min_focal) && !converter(l.models[0]))
    {
        const Numbers n = guess(l.models[0]);
        if (n.min_focal)
            minf = n.min_focal;
        if (n.max_focal)
            maxf = n.max_focal;
        if (n.aperture)
            mina = n.aperture;
    }
    if (!l.min_aperture || !l.min_focal)
    {
        for (const auto& c : l.distortion)
            minf = std::min(minf, c.focal), maxf = std::max(maxf, c.focal);
        for (const auto& c : l.tca)
            minf = std::min(minf, c.focal), maxf = std::max(maxf, c.focal);
        for (const auto& c : l.vignetting)
        {
            minf = std::min(minf, c.focal), maxf = std::max(maxf, c.focal);
            mina = std::min(mina, c.aperture), maxa = std::max(maxa, c.aperture);
        }
    }
    if (minf != FLT_MAX && !l.min_focal)
        l.min_focal = minf;
    if (maxf != FLT_MIN && !l.max_focal)
        l.max_focal = maxf;
    if (mina != FLT_MAX && !l.min_aperture)
        l.min_aperture = mina;
    if (maxa != FLT_MIN && !l.max_aperture)
        l.max_aperture = maxa;
    if (!l.max_focal)
        l.max_focal = l.min_focal;
}

// _lf_interpolate: a cubic Hermite spline through y2 and y3, its tangents
// from the neighbours where there are any (FLT_MAX: none).
float spline(float y1, float y2, float y3, float y4, float t)
{
    const float t2 = t * t, t3 = t2 * t;
    const float tg2 = y1 == FLT_MAX ? y3 - y2 : (y3 - y1) * 0.5f;
    const float tg3 = y4 == FLT_MAX ? y3 - y2 : (y4 - y2) * 0.5f;
    return (2 * t3 - 3 * t2 + 1) * y2 + (t3 - 2 * t2 + t) * tg2 + (-2 * t3 + 3 * t2) * y3 + (t3 - t2) * tg3;
}

// __insert_spline: the two calibrations below the focal length and the two
// above, nearest first.
template <typename T>
void insert(const T* s[4], float d[4], float dist, const T* v)
{
    if (dist < 0)
    {
        if (dist > d[1])
            d[0] = d[1], d[1] = dist, s[0] = s[1], s[1] = v;
        else if (dist > d[0])
            d[0] = dist, s[0] = v;
    }
    else
    {
        if (dist < d[2])
            d[3] = d[2], d[2] = dist, s[3] = s[2], s[2] = v;
        else if (dist < d[3])
            d[3] = dist, s[3] = v;
    }
}

// lfLens::InterpolateDistortion. Its terms follow a 1/f law, so they're
// interpolated times the focal length and divided by it after.
bool interpolate(const Lens& l, float focal, Lens::Distortion& out)
{
    const Lens::Distortion* s[4] = {};
    float d[4] = {-FLT_MAX, -FLT_MAX, FLT_MAX, FLT_MAX};
    int model = 0;
    for (const auto& c : l.distortion)
    {
        if (!model)
            model = c.model;
        else if (model != c.model)
            continue;  // only the first model lensfun meets
        const float df = focal - c.focal;
        if (df == 0)
        {
            out = c;
            return true;
        }
        insert(s, d, df, &c);
    }
    if (!s[1] || !s[2])
    {
        if (!s[1] && !s[2])
            return false;
        out = s[1] ? *s[1] : *s[2];
        return true;
    }
    out.model = model;
    out.focal = focal;
    const float t = (focal - s[1]->focal) / (s[2]->focal - s[1]->focal);
    out.real_focal = spline(s[0] ? s[0]->real_focal : FLT_MAX, s[1]->real_focal, s[2]->real_focal, s[3] ? s[3]->real_focal : FLT_MAX, t);
    for (int i = 0; i < 3; ++i)
        out.terms[i] = spline(s[0] ? s[0]->terms[i] * s[0]->focal : FLT_MAX, s[1]->terms[i] * s[1]->focal, s[2]->terms[i] * s[2]->focal,
                              s[3] ? s[3]->terms[i] * s[3]->focal : FLT_MAX, t) /
                       focal;
    return true;
}

// lfLens::InterpolateTCA: the same, but vr and vb (the terms near 1) as they are.
bool interpolate(const Lens& l, float focal, Lens::Tca& out)
{
    const Lens::Tca* s[4] = {};
    float d[4] = {-FLT_MAX, -FLT_MAX, FLT_MAX, FLT_MAX};
    int model = 0;
    for (const auto& c : l.tca)
    {
        if (!model)
            model = c.model;
        else if (model != c.model)
            continue;
        const float df = focal - c.focal;
        if (df == 0)
        {
            out = c;
            return true;
        }
        insert(s, d, df, &c);
    }
    if (!s[1] || !s[2])
    {
        if (!s[1] && !s[2])
            return false;
        out = s[1] ? *s[1] : *s[2];
        return true;
    }
    out.model = model;
    out.focal = focal;
    const float t = (focal - s[1]->focal) / (s[2]->focal - s[1]->focal);
    for (int i = 0; i < 6; ++i)
    {
        const auto scale = [&](const Lens::Tca* c) { return i < 2 ? 1.0f : c->focal; };
        const float after = i < 2 ? 1.0f : focal;
        out.terms[i] = spline(s[0] ? s[0]->terms[i] * scale(s[0]) : FLT_MAX, s[1]->terms[i] * scale(s[1]), s[2]->terms[i] * scale(s[2]),
                              s[3] ? s[3]->terms[i] * scale(s[3]) : FLT_MAX, t) /
                       after;
    }
    return true;
}

// lfLens::InterpolateVignetting: inverse distance weighting (power 3.5)
// over focal length (across the lens's range), 4 / aperture and 0.1 /
// distance; nothing when the nearest calibration is too far away.
bool interpolate(const Lens& l, float focal, float aperture, float distance, Lens::Vignetting& out)
{
    const auto dist = [&](const Lens::Vignetting& x)
    {
        float f1 = focal - l.min_focal, f2 = x.focal - l.min_focal;
        const float df = l.max_focal - l.min_focal;
        if (df != 0)
            f1 /= df, f2 /= df;
        const float a1 = 4.0f / aperture, a2 = 4.0f / x.aperture;
        const float d1 = 0.1f / distance, d2 = 0.1f / x.distance;
        return std::sqrt((f2 - f1) * (f2 - f1) + (a2 - a1) * (a2 - a1) + (d2 - d1) * (d2 - d1));
    };
    out = {focal, aperture, distance, {0, 0, 0}};
    float total = 0, nearest = FLT_MAX;
    for (const auto& c : l.vignetting)
    {
        const float d = dist(c);
        if (d < 0.0001f)
        {
            out = c;
            return true;
        }
        nearest = std::min(nearest, d);
        const float w = std::fabs(1.0f / std::pow(d, 3.5f));
        for (int i = 0; i < 3; ++i)
            out.terms[i] += w * c.terms[i];
        total += w;
    }
    if (nearest > 1 || !(total > 0) || nearest == FLT_MAX)
        return false;
    for (float& t : out.terms)
        t /= total;
    return true;
}

// lfDatabase::MatchScore's part for the crop factor: how well a lens
// calibrated on a sensor of crop `lens` suits one of `camera`; 0 not at all
// (a calibration on a smaller sensor doesn't reach the corners).
int crop_score(float camera, float lens)
{
    if (camera > 0.01f && camera < lens * 0.96f)
        return 0;
    if (camera >= lens * 1.41f)
        return 2;
    if (camera >= lens * 1.31f)
        return 4;
    if (camera >= lens * 1.21f)
        return 6;
    if (camera >= lens * 1.11f)
        return 8;
    if (camera >= lens * 1.01f)
        return 10;
    if (camera >= lens)
        return 5;
    return 3;
}

bool near(float a, float b)
{
    const float r = a / b;
    return r > 0.99f && r < 1.01f;
}

}  // namespace

std::vector<std::string> words(const std::string& name)
{
    // lfFuzzyStrCmp::Split
    std::vector<std::string> out;
    const char* s = name.c_str();
    while (*s)
    {
        while (*s && std::isspace((unsigned char)*s))
            ++s;
        if (!*s)
            break;
        const char* word = s++;
        int strip = 0;
        if (std::isdigit((unsigned char)*word))
        {
            while (*s && (std::isdigit((unsigned char)*s) || *s == '.'))
                ++s;
            if (s - word >= 2 && s[-2] == '.' && s[-1] == '0')
                strip = 2;
        }
        else if (std::ispunct((unsigned char)*word))
            while (*s && std::ispunct((unsigned char)*s))
                ++s;
        else
            while (*s && !std::isspace((unsigned char)*s) && !std::isdigit((unsigned char)*s) && !std::ispunct((unsigned char)*s))
                ++s;
        if (s - word == 1 && (std::ispunct((unsigned char)*word) || std::tolower((unsigned char)*word) == 'f') && *word != '*' && *word != '+')
            continue;
        gchar* f = g_utf8_casefold(word, s - word - strip);
        out.emplace_back(f);
        g_free(f);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The XML, as lensfun's database.cpp reads it (what it rejects, rejected
// here; elements it doesn't know, passed over).
struct Parser
{
    explicit Parser(Database& d) : db(d) {}
    Database& db;
    std::vector<std::string> stack;
    std::string lang;
    bool in_camera = false, in_lens = false, in_mount = false;
    Camera camera;
    Lens lens;
    Mount mount;

    static const char* attr(const gchar** names, const gchar** values, const char* name)
    {
        for (int i = 0; names[i]; ++i)
            if (!std::strcmp(names[i], name))
                return values[i];
        return nullptr;
    }

    static void start(GMarkupParseContext*, const gchar* element, const gchar** names, const gchar** values, gpointer data, GError** error)
    {
        auto& p = *static_cast<Parser*>(data);
        const std::string ctx = p.stack.empty() ? "" : p.stack.back();
        p.stack.push_back(element);
        p.lang.clear();
        const std::string e = element;
        if (e == "lensdatabase")
        {
            const char* v = attr(names, values, "version");
            const int version = v ? std::atoi(v) : 0;
            if (!ctx.empty() || version < 1 || version > 2)
                g_set_error(error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT, "database version %d", version);
        }
        else if (e == "mount" && ctx == "lensdatabase")
            p.in_mount = true, p.mount = {};
        else if (e == "camera" && ctx == "lensdatabase")
            p.in_camera = true, p.camera = {};
        else if (e == "lens" && ctx == "lensdatabase")
            p.in_lens = true, p.lens = {};
        else if (p.in_lens && ctx == "lens" && (e == "focal" || e == "aperture"))
        {
            float& lo = e == "focal" ? p.lens.min_focal : p.lens.min_aperture;
            float& hi = e == "focal" ? p.lens.max_focal : p.lens.max_aperture;
            if (const char* v = attr(names, values, "min"))
                lo = number(v);
            if (const char* v = attr(names, values, "max"))
                hi = number(v);
            if (const char* v = attr(names, values, "value"))
                lo = hi = number(v);
        }
        else if (p.in_lens && ctx == "lens" && e == "center")
        {
            if (const char* v = attr(names, values, "x"))
                p.lens.centre_x = number(v);
            if (const char* v = attr(names, values, "y"))
                p.lens.centre_y = number(v);
        }
        else if (p.in_lens && ctx == "calibration" && e == "distortion")
        {
            Lens::Distortion d{};
            const char* model = attr(names, values, "model");
            d.model = !model ? 0 : !std::strcmp(model, "poly3") ? 1 : !std::strcmp(model, "poly5") ? 2 : !std::strcmp(model, "ptlens") ? 3 : 0;
            for (int i = 0; names[i]; ++i)
            {
                const std::string n = names[i];
                const float v = number(values[i]);
                if (n == "focal")
                    d.focal = v;
                else if (n == "real-focal")
                    d.real_focal = v;
                else if (n == "a" || n == "k1")
                    d.terms[0] = v;
                else if (n == "b" || n == "k2")
                    d.terms[1] = v;
                else if (n == "c" || n == "k3")
                    d.terms[2] = v;
            }
            if (!(d.real_focal > 0))
                d.real_focal = d.model == 3   ? d.focal * (1 - d.terms[0] - d.terms[1] - d.terms[2])
                               : d.model == 1 ? d.focal * (1 - d.terms[0])
                                              : d.focal;
            if (d.model && d.focal > 0)
                p.lens.distortion.push_back(d);
        }
        else if (p.in_lens && ctx == "calibration" && e == "tca")
        {
            Lens::Tca t{};
            t.terms[0] = t.terms[1] = 1;
            const char* model = attr(names, values, "model");
            t.model = !model ? 0 : !std::strcmp(model, "linear") ? 1 : !std::strcmp(model, "poly3") ? 2 : 0;
            static const char* keys[6][2] = {{"kr", "vr"}, {"kb", "vb"}, {"cr", ""}, {"cb", ""}, {"br", ""}, {"bb", ""}};
            for (int i = 0; names[i]; ++i)
            {
                if (!std::strcmp(names[i], "focal"))
                    t.focal = number(values[i]);
                for (int k = 0; k < 6; ++k)
                    if (!std::strcmp(names[i], keys[k][0]) || !std::strcmp(names[i], keys[k][1]))
                        t.terms[k] = number(values[i]);
            }
            if (t.model && t.focal > 0)
                p.lens.tca.push_back(t);
        }
        else if (p.in_lens && ctx == "calibration" && e == "vignetting")
        {
            Lens::Vignetting v{};
            const char* model = attr(names, values, "model");
            for (int i = 0; names[i]; ++i)
            {
                const std::string n = names[i];
                const float x = number(values[i]);
                if (n == "focal")
                    v.focal = x;
                else if (n == "aperture")
                    v.aperture = x;
                else if (n == "distance")
                    v.distance = x;
                else if (n == "k1")
                    v.terms[0] = x;
                else if (n == "k2")
                    v.terms[1] = x;
                else if (n == "k3")
                    v.terms[2] = x;
            }
            if (model && !std::strcmp(model, "pa") && v.focal > 0 && v.aperture > 0 && v.distance > 0)
                p.lens.vignetting.push_back(v);
        }
        else if (e == "maker" || e == "model" || e == "name")
        {
            if (const char* l = attr(names, values, "lang"))
                p.lang = l;
        }
    }

    static void end(GMarkupParseContext*, const gchar* element, gpointer data, GError**)
    {
        auto& p = *static_cast<Parser*>(data);
        if (!p.stack.empty())
            p.stack.pop_back();
        const std::string e = element;
        const std::string ctx = p.stack.empty() ? "" : p.stack.back();
        if (ctx != "lensdatabase")
            return;
        if (e == "mount" && p.in_mount)
        {
            if (!p.mount.name.empty())
                p.db.mounts_.push_back(std::move(p.mount));
            p.in_mount = false;
        }
        else if (e == "camera" && p.in_camera)
        {
            settle(p.camera.makers), settle(p.camera.models);
            if (!p.camera.makers.empty() && !p.camera.models.empty() && !p.camera.mount.empty() && p.camera.crop > 0)
                p.db.cameras_.push_back(std::move(p.camera));
            p.in_camera = false;
        }
        else if (e == "lens" && p.in_lens)
        {
            // lfLens::Check
            Lens& l = p.lens;
            settle(l.makers), settle(l.models);
            guess_parameters(l);
            if (!l.models.empty() && !l.mounts.empty() && l.min_focal <= l.max_focal && (!l.max_aperture || l.min_aperture <= l.max_aperture) &&
                l.crop > 0 && l.aspect >= 1)
            {
                l.words = words(l.models[0]);
                p.db.lenses_.push_back(std::move(l));
            }
            p.in_lens = false;
        }
    }

    // Strings without a default (a language's only) take their first.
    static void settle(std::vector<std::string>& s)
    {
        if (!s.empty() && s[0].empty())
            s.erase(s.begin());
    }

    // A multi-language string: the one without a language first.
    static void add(std::vector<std::string>& to, std::string text, const std::string& lang)
    {
        if (lang.empty())
        {
            if (to.empty())
                to.push_back(std::move(text));
            else
                to[0] = std::move(text);
        }
        else
        {
            if (to.empty())
                to.emplace_back();  // the default, still to come
            to.push_back(std::move(text));
        }
    }

    static void text(GMarkupParseContext*, const gchar* text, gsize len, gpointer data, GError**)
    {
        auto& p = *static_cast<Parser*>(data);
        if (p.stack.empty())
            return;
        gchar* stripped = g_strstrip(g_strndup(text, len));
        const std::string t = stripped;
        g_free(stripped);
        if (t.empty())
            return;
        const std::string& e = p.stack.back();
        if (e == "name" && p.in_mount && p.lang.empty())
            p.mount.name = t;
        else if (e == "compat" && p.in_mount)
            p.mount.compat.push_back(t);
        else if (e == "maker" && (p.in_camera || p.in_lens))
            add(p.in_camera ? p.camera.makers : p.lens.makers, t, p.lang);
        else if (e == "model" && (p.in_camera || p.in_lens))
            add(p.in_camera ? p.camera.models : p.lens.models, t, p.lang);
        else if (e == "mount" && p.in_camera)
            p.camera.mount = t;
        else if (e == "mount" && p.in_lens)
            p.lens.mounts.push_back(t);
        else if (e == "cropfactor" && p.in_camera)
            p.camera.crop = number(t.c_str());
        else if (e == "cropfactor" && p.in_lens)
            p.lens.crop = number(t.c_str());
        else if (e == "aspect-ratio" && p.in_lens)
        {
            const size_t colon = t.find(':');
            p.lens.aspect = colon == std::string::npos ? number(t.c_str()) : number(t.c_str()) / number(t.c_str() + colon + 1);
        }
    }
};

bool Database::add_xml(const char* data, size_t size, std::string& error)
{
    static const GMarkupParser callbacks = {Parser::start, Parser::end, Parser::text, nullptr, nullptr};
    Parser p(*this);
    GMarkupParseContext* ctx = g_markup_parse_context_new(&callbacks, GMarkupParseFlags(0), &p, nullptr);
    GError* e = nullptr;
    const bool ok = g_markup_parse_context_parse(ctx, data, gssize(size), &e) && g_markup_parse_context_end_parse(ctx, &e);
    if (!ok)
    {
        error = e ? e->message : "malformed";
        g_clear_error(&e);
    }
    g_markup_parse_context_free(ctx);
    return ok;
}

const Camera* Database::camera(const Shot& shot) const
{
    if (shot.model.empty())
        return nullptr;
    const std::string make = folded(shot.make), model = folded(shot.model), both = folded(shot.make + " " + shot.model);
    // By maker and model; or, where the maker's name has changed (Olympus,
    // now OM Digital Solutions), by a model name no other camera has.
    const Camera* found[2] = {};
    bool unsure[2] = {};  // two that differ: which is it?
    for (const Camera& c : cameras_)
    {
        const bool named = std::any_of(c.models.begin(), c.models.end(),
                                       [&](const std::string& m)
                                       {
                                           const std::string f = folded(m);
                                           return f == model || f == both;
                                       });
        if (!named)
            continue;
        const bool maker = std::any_of(c.makers.begin(), c.makers.end(), [&](const std::string& m) { return first_word(folded(m)) == first_word(make); });
        for (int i = maker ? 0 : 1; i < 2; ++i)
            if (!found[i])
                found[i] = &c;
            else if (found[i]->mount != c.mount || !near(found[i]->crop, c.crop))
                unsure[i] = true;
    }
    for (int i = 0; i < 2; ++i)
        if (found[i])
            return unsure[i] ? nullptr : found[i];
    return nullptr;
}

const Lens* Database::lens(const Camera& camera, const Shot& shot, std::string& why) const
{
    // A camera with a lens of its own: lensfun's mount for it is named in
    // lower case, and has that one lens (whatever the raw calls it).
    if (!camera.mount.empty() && g_ascii_islower(camera.mount[0]))
    {
        const Lens* own = nullptr;
        for (const Lens& l : lenses_)
            if (std::find(l.mounts.begin(), l.mounts.end(), camera.mount) != l.mounts.end() && crop_score(camera.crop, l.crop))
            {
                if (own && folded(own->models[0]) != folded(l.models[0]))
                {
                    why = "the " + camera.mount + " mount has more than one lens";
                    return nullptr;
                }
                if (!own || crop_score(camera.crop, l.crop) > crop_score(camera.crop, own->crop))
                    own = &l;
            }
        if (!own)
            why = "its built-in lens isn't in the database";
        return own;
    }
    if (shot.lens.empty())
    {
        why = "the raw doesn't name its lens";
        return nullptr;
    }
    // The numbers read from names as lensfun reads them, but whatever the
    // case ("F2.8" as "f/2.8").
    const std::vector<std::string> pattern = words(shot.lens);
    const Numbers numbers = guess(folded(shot.lens));
    if (pattern.size() < 2 || !numbers.min_focal)
    {
        why = "the lens's name says too little";
        return nullptr;
    }
    // The camera's mount, and those it takes.
    std::vector<std::string> mounts = {folded(camera.mount)};
    for (const Mount& m : mounts_)
        if (folded(m.name) == mounts[0])
            for (const std::string& c : m.compat)
                mounts.push_back(folded(c));
    const Lens* best = nullptr;
    int best_score = 0;
    std::vector<std::string> names;  // the different lenses that fit
    for (const Lens& l : lenses_)
    {
        if (!std::any_of(l.mounts.begin(), l.mounts.end(),
                         [&](const std::string& m) { return std::find(mounts.begin(), mounts.end(), folded(m)) != mounts.end(); }))
            continue;
        const int score = crop_score(camera.crop, l.crop);
        if (!score)
            continue;
        // Every word of the recorded name, and nothing else but the maker's.
        std::vector<std::string> maker;
        for (const std::string& m : l.makers)
            for (std::string& w : words(m))
                maker.push_back(std::move(w));
        std::sort(maker.begin(), maker.end());
        bool named = false;
        for (const std::string& model : l.models)
        {
            const std::vector<std::string> w = &model == &l.models[0] ? l.words : words(model);
            if (!std::includes(w.begin(), w.end(), pattern.begin(), pattern.end()))
                continue;
            std::vector<std::string> extra;
            std::set_difference(w.begin(), w.end(), pattern.begin(), pattern.end(), std::back_inserter(extra));
            std::sort(extra.begin(), extra.end());
            extra.erase(std::unique(extra.begin(), extra.end()), extra.end());
            if (!std::all_of(extra.begin(), extra.end(), [&](const std::string& x) { return std::binary_search(maker.begin(), maker.end(), x); }))
                continue;
            // Its focal lengths and aperture exactly the name's.
            const Numbers n = guess(folded(model));
            const auto same = [](float a, float b) { return !a || !b || near(a, b); };
            if (!n.min_focal || !same(n.min_focal, numbers.min_focal) || !same(n.max_focal ? n.max_focal : n.min_focal, numbers.max_focal ? numbers.max_focal : numbers.min_focal) ||
                !same(n.aperture, numbers.aperture))
                continue;
            named = true;
            break;
        }
        if (!named)
            continue;
        // And the range the maker notes give, if they do.
        if (shot.lens_min_focal > 0 && shot.lens_max_focal > 0 &&
            (!near(shot.lens_min_focal, l.min_focal) || !near(shot.lens_max_focal, l.max_focal)))
            continue;
        const std::string name = folded(l.models[0]);
        if (std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(name);
        if (score > best_score)
            best = &l, best_score = score;
    }
    if (names.size() > 1)
    {
        why = "it could be any of " + std::to_string(names.size()) + " lenses";
        for (const std::string& n : names)
            why += (n == names[0] ? ": " : ", ") + n;
        return nullptr;
    }
    if (!best)
        why = "no lens in the database by that name for the " + camera.mount + " mount";
    return best;
}

dng::Corrections corrections(const Lens& lens, const Camera& camera, const Shot& shot)
{
    dng::Corrections out;
    const int x = shot.frame[0], y = shot.frame[1], width = shot.frame[2], height = shot.frame[3];
    if (width < 2 || height < 2 || !(shot.focal > 0))
        return out;
    const float crop = camera.crop, focal = shot.focal;
    // lfModifier's coordinates: pixel centres 0 .. width - 1, in units of
    // the real focal length, about the lens's centre.
    const double W = width - 1, H = height - 1;
    Lens::Distortion dist{};
    const bool distorts = interpolate(lens, focal, dist);
    const double real_focal = distorts ? dist.real_focal : focal;
    const double norm = std::hypot(36.0, 24.0) / crop / std::hypot(W + 1, H + 1) / real_focal;
    const double size = std::min(W, H);
    const double cx = W / 2 + size / 2 * lens.centre_x, cy = H / 2 + size / 2 * lens.centre_y;
    // The calibration's coordinates (Hugin's: r = 1 at half the shorter
    // side of its sensor) against these.
    const float hugin_mm = float(std::hypot(36.0, 24.0) / lens.crop / std::hypot(double(lens.aspect), 1.0) / 2.0);
    const float hugin = float(real_focal) / hugin_mm;

    // Distortion as rd = ru (1 + c1 ru + c2 ru^2 + c3 ru^3 + c4 ru^4).
    float c[4] = {};
    if (distorts)
    {
        const float* k = dist.terms;
        if (dist.model == 1)
        {
            const float d = 1 - k[0];
            c[1] = k[0] * std::pow(hugin, 2.0f) / std::pow(d, 3.0f);
        }
        else if (dist.model == 2)
            c[1] = k[0] * std::pow(hugin, 2.0f), c[3] = k[1] * std::pow(hugin, 4.0f);
        else if (dist.model == 3)
        {
            // a, b, c: rd = ru (a ru^3 + b ru^2 + c ru + 1), with d = 1 - a - b - c taken out
            const float d = 1 - k[0] - k[1] - k[2];
            c[0] = k[2] * hugin / std::pow(d, 2.0f);
            c[1] = k[1] * std::pow(hugin, 2.0f) / std::pow(d, 3.0f);
            c[2] = k[0] * std::pow(hugin, 3.0f) / std::pow(d, 4.0f);
        }
    }
    const bool warps_shape = c[0] || c[1] || c[2] || c[3];
    const auto distort = [&](float x, float y, float& ox, float& oy)
    {
        const float r2 = x * x + y * y, r = std::sqrt(r2);
        const float p = 1 + r * (c[0] + r * (c[1] + r * (c[2] + r * c[3])));
        ox = x * p, oy = y * p;
    };

    // TCA: red and blue's radius r (v + c r + b r^2) after the distortion.
    Lens::Tca tca{};
    float t[6] = {1, 1, 0, 0, 0, 0};  // vr, vb, cr, cb, br, bb
    bool chromatic = interpolate(lens, focal, tca);
    if (chromatic)
    {
        for (int i = 0; i < 6; ++i)
            t[i] = tca.terms[i];
        if (tca.model == 1)
            t[2] = t[3] = t[4] = t[5] = 0;
        else
        {
            t[2] *= hugin, t[3] *= hugin;
            t[4] *= hugin * hugin, t[5] *= hugin * hugin;
        }
        chromatic = t[0] != 1 || t[1] != 1 || t[2] || t[3] || t[4] || t[5];
    }

    // Vignetting: the gain 1 / (1 + k1 r^2 + k2 r^4 + k3 r^6), r = 1 at the
    // calibration sensor's corner.
    Lens::Vignetting vig{};
    const bool vignettes = shot.aperture > 0 && interpolate(lens, focal, shot.aperture, shot.distance > 0 ? shot.distance : 1000, vig) &&
                           (vig.terms[0] || vig.terms[1] || vig.terms[2]);

    // lfModifier::GetAutoScale: the least enlargement that leaves no edge
    // of the frame reading from outside the image (Newton's method along
    // the corners' and edges' directions), and a little more for the TCA.
    float scale = 1;
    if (warps_shape)
    {
        const float max_x = float(W / 2 * norm), max_y = float(H / 2 * norm);
        const auto residual = [&](float x, float y)
        {
            return std::max({x - max_x, -max_x - x, y - max_y, -max_y - y});
        };
        const auto transformed = [&](float dist, float angle) -> float
        {
            const double sa = std::sin(angle), ca = std::cos(angle);
            float ru = dist, dx = 0.0001f;
            for (int countdown = 50;; --countdown)
            {
                float x, y;
                distort(float(ca * ru), float(sa * ru), x, y);
                const double rd = residual(x, y);
                if (rd > -0.00001 * 100 && rd < 0.00001 * 100)
                    break;
                if (!countdown)
                    return -1;
                distort(float(ca * (ru + dx)), float(sa * (ru + dx)), x, y);
                const double rd1 = residual(x, y);
                if (std::fabs(rd1 - rd) < 0.00001)
                {
                    dx *= 2;
                    continue;
                }
                ru -= float(rd / ((rd1 - rd) / dx));
            }
            return ru;
        };
        const float corner = float(std::atan2(H, W));
        const float diagonal = float(std::sqrt(W * W + H * H) * 0.5 * norm), across = float(W * 0.5 * norm), down = float(H * 0.5 * norm);
        const float points[8][2] = {{across, 0},
                                    {diagonal, corner},
                                    {down, float(G_PI / 2)},
                                    {diagonal, float(G_PI) - corner},
                                    {across, float(G_PI)},
                                    {diagonal, float(G_PI) + corner},
                                    {down, float(G_PI * 3 / 2)},
                                    {diagonal, float(2 * G_PI) - corner}};
        float s = 0.01f;
        for (const auto& p : points)
            s = std::max(s, p[0] / transformed(p[0], p[1]));
        scale = s * 1.001f * (chromatic ? 1.001f : 1.0f);
    }
    else if (chromatic)
        scale = 1.001f;  // lensfun's for the subpixel shift alone

    if (!warps_shape && !chromatic && !vignettes)
        return out;
    std::vector<uint32_t>& w = out.words;
    w.assign(48, 0);
    if (vignettes)
    {
        w[2] = 2;
        const float vh = float(real_focal / (std::hypot(36.0, 24.0) / lens.crop / 2.0));
        w[4] = dng::bits(vig.terms[0] * vh * vh), w[5] = dng::bits(vig.terms[1] * std::pow(vh, 4.0f)), w[6] = dng::bits(vig.terms[2] * std::pow(vh, 6.0f));
        // The develop measures from photosite centres (x + 0.5).
        w[9] = dng::bits(float(x + cx + 0.5)), w[10] = dng::bits(float(y + cy + 0.5)), w[11] = dng::bits(float(norm * norm));
    }
    if (warps_shape || chromatic)
    {
        w[12] = 1;
        w[16] = chromatic ? 3 : 1;
        w[17] = dng::bits(float(x + cx)), w[18] = dng::bits(float(y + cy)), w[19] = dng::bits(float(norm));
        w[20] = dng::bits(1 / scale);
        for (int i = 0; i < 4; ++i)
            w[21 + i] = dng::bits(c[i]);
        w[25] = dng::bits(t[0]), w[26] = dng::bits(t[2]), w[27] = dng::bits(t[4]);
        w[28] = dng::bits(t[1]), w[29] = dng::bits(t[3]), w[30] = dng::bits(t[5]);
    }
    std::string& s = out.summary;
    for (const auto& [on, what] : {std::pair{warps_shape, "distortion"}, std::pair{chromatic, "TCA"}, std::pair{vignettes, "vignetting"}})
        if (on)
            s += (s.empty() ? "" : ", ") + std::string(what);
    return out;
}

}  // namespace lenses
