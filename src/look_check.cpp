// --check-looks DIR: the look's profile mapping (look.h) and its XMP, both
// ways, on sidecars written into DIR.
#include "decoders/decoder.h"
#include "image.h"
#include "look.h"

#include <glib.h>
#include <glib/gstdio.h>

#include "decoders/texel.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace
{

int failures = 0;

void expect(bool ok, const std::string& what)
{
    if (!ok)
    {
        std::fprintf(stderr, "check-looks: FAILED: %s\n", what.c_str());
        ++failures;
    }
}

std::string sidecar_text(const std::string& raw)
{
    gchar* text = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(xmp_sidecar_path(raw).c_str(), &text, &len, nullptr))
        return {};
    std::string out(text, len);
    g_free(text);
    return out;
}

bool has(const std::string& text, const std::string& what)
{
    return text.find(what) != std::string::npos;
}

// A raw's sidecar of these properties (crs and the rest declared), and the
// raw as the viewer opens it with them.
const char kHead[] = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
                     " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
                     "  <rdf:Description rdf:about=\"\"\n"
                     "    xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"\n"
                     "    xmlns:dc=\"http://purl.org/dc/elements/1.1/\"\n"
                     "    xmlns:crs=\"http://ns.adobe.com/camera-raw-settings/1.0/\"";
const char kKeywords[] = "   <dc:subject>\n    <rdf:Bag>\n     <rdf:li>harbour</rdf:li>\n     <rdf:li>dusk</rdf:li>\n    </rdf:Bag>\n   </dc:subject>\n";
const char kTail[] = "  </rdf:Description>\n </rdf:RDF>\n</x:xmpmeta>\n";

std::string raw_with(const std::string& dir, const char* name, const std::string& attributes, const std::string& elements)
{
    const std::string raw = dir + "/" + name + ".CR2";
    const std::string text = std::string(kHead) + attributes + ">\n" + kKeywords + elements + kTail;
    g_file_set_contents(xmp_sidecar_path(raw).c_str(), text.data(), gssize(text.size()), nullptr);
    return raw;
}

Image opened(const std::string& raw, Look untreated)
{
    Image img;
    img.is_raw = true;
    read_xmp(raw, nullptr, 0, img);
    img.adjust = img.opening_adjust(true, untreated);
    return img;
}

XmpDevelop develop_of(const Image& img)
{
    XmpDevelop d = img.xmp;
    d.adjust = img.adjust;
    return d;
}

bool save(const std::string& raw, const Image& img)
{
    std::string error;
    const bool ok = write_xmp_sidecar(raw, develop_of(img), img.opening_adjust(false), error);
    expect(ok, "writing " + xmp_sidecar_path(raw) + ": " + error);
    return ok;
}

}  // namespace

int check_looks(const std::string& dir)
{
    g_mkdir_with_parents(dir.c_str(), 0755);

    // The table, and each look written as an Adobe profile reads back as itself.
    const struct
    {
        const char* profile;
        Look look;
    } table[] = {
        {"Adobe Neutral", Look::Neutral},     {"Camera Neutral", Look::Neutral},       {"Camera Faithful", Look::Neutral},
        {"Adobe Standard", Look::CameraLike}, {"Adobe Color", Look::CameraLike},       {"Camera Standard", Look::CameraLike},
        {"Adobe Portrait", Look::CameraLike}, {"Camera Portrait", Look::CameraLike},   {"Adobe Vivid", Look::Enhanced},
        {"Adobe Landscape", Look::Enhanced},  {"Camera Landscape", Look::Enhanced},    {"Adobe Monochrome", Look::Monochrome},
        {"Camera Monochrome", Look::Monochrome}, {"Some Vendor Film 7", Look::CameraLike}, {"", Look::CameraLike},
        {"B&W 05", Look::Monochrome},         {"Camera Velvia/Vivid", Look::Enhanced}, {"Camera Flat", Look::Neutral},
    };
    for (const auto& t : table)
        expect(look_of_profile(t.profile) == t.look, std::string("profile '") + t.profile + "' maps to " + look_id(t.look));
    expect(!known_profile("Some Vendor Film 7") && known_profile("Adobe Color"), "unknown profiles are told from Lightroom's");
    expect(std::string(adobe_profile(Look::Enhanced)) == "Adobe Vivid" && std::string(adobe_profile(Look::CameraLike)) == "Adobe Color" &&
               std::string(adobe_profile(Look::Neutral)) == "Adobe Neutral",
           "looks are written as Adobe Vivid, Adobe Color, Adobe Neutral");
    for (int i = 0; i < kLookCount; ++i)
        expect(look_of_profile(adobe_profile(Look(i))) == Look(i), std::string("the Adobe profile of ") + look_id(Look(i)) + " reads back");
    for (const auto& [style, look] : {std::pair{"Landscape", Look::Enhanced}, {"Neutral", Look::Neutral}, {"Faithful", Look::Neutral},
                                      {"Monochrome", Look::Monochrome}, {"Standard", Look::CameraLike}, {"User Def. 1", Look::CameraLike},
                                      {"VIVID", Look::Enhanced}, {"Pro Neg. Std", Look::Neutral}, {"Velvia", Look::Enhanced}})
        expect(look_of_picture_style(style) == look, std::string("picture style '") + style + "' maps to " + look_id(look));
    // The fitted model's shape: the curve pivots near middle grey (darker
    // below, brighter above, white kept); blue skies held back, foliage raised.
    {
        const LookParams n = look_params(Look::Neutral), c = look_params(Look::CameraLike), e = look_params(Look::Enhanced);
        const double dark = srgb_encode(0.05), light = srgb_encode(0.5);
        expect(n.identity() && look_tone(n, dark) == dark, "Neutral's curve is the identity");
        expect(look_tone(c, dark) < dark && look_tone(c, light) > light && std::abs(look_tone(c, 1.0) - 1) < 1e-9, "Camera-like's S-curve");
        expect(look_tone(e, dark) < look_tone(c, dark), "Enhanced's curve is stronger");
        double sky[3] = {0.05, 0.12, 0.25}, leaf[3] = {0.08, 0.14, 0.03};
        expect(look_colour(c, sky) < look_colour(c, leaf), "Camera-like holds blue back against foliage");
    }
    // Neutral is the identity: no tables; the others aren't.
    {
        Adjustments a;
        expect(a.neutral(), "Neutral with every control at rest is neutral");
        for (Look l : {Look::CameraLike, Look::Enhanced, Look::Monochrome})
        {
            a.look = l;
            expect(!a.neutral(), std::string(look_id(l)) + " isn't neutral");
        }
    }

    // Treated: develop settings, not ratings, labels or keywords.
    {
        const Image rated = opened(raw_with(dir, "rated", "\n    xmp:Rating=\"3\" xmp:Label=\"Red\"", ""), Look::Enhanced);
        expect(!rated.xmp.treated, "ratings, a label and keywords aren't develop settings");
        expect(rated.adjust.look == Look::Enhanced && rated.adjust.profile.empty() && !rated.adjust.treated,
               "an untreated raw takes the default look");
        const Image unset = opened(raw_with(dir, "unset", "\n    xmp:Rating=\"1\" crs:HasSettings=\"False\"", ""), Look::Neutral);
        expect(!unset.xmp.treated, "HasSettings=\"False\" alone isn't treated");
        const Image process = opened(raw_with(dir, "process", "\n    crs:ProcessVersion=\"11.0\"", ""), Look::Enhanced);
        expect(process.xmp.treated && process.adjust.treated, "a ProcessVersion is treated");
        expect(process.adjust.look == Look::CameraLike, "a treated raw without a profile is Adobe's default (Camera-like), not the default look");
        const Image one = opened(raw_with(dir, "one", "\n    crs:Vibrance=\"+10\"", ""), Look::Neutral);
        expect(one.xmp.treated, "any develop setting is treated");
        const Image standard = opened(raw_with(dir, "standard", "\n    crs:ProcessVersion=\"11.0\" crs:CameraProfile=\"Camera Landscape\"", ""), Look::Neutral);
        expect(standard.adjust.look == Look::Enhanced && standard.adjust.profile == "Camera Landscape", "a treated raw's profile picks its look");
        // Lightroom's own way with Adobe Color, Vivid...: a crs:Look in the profile group.
        const Image vivid = opened(raw_with(dir, "vivid", "\n    crs:ProcessVersion=\"11.0\" crs:CameraProfile=\"Adobe Standard\"",
                                            "   <crs:Look>\n    <rdf:Description crs:Name=\"Adobe Vivid\" crs:Amount=\"1\">\n"
                                            "     <crs:Group>\n      <rdf:Alt>\n       <rdf:li xml:lang=\"x-default\">Profiles</rdf:li>\n"
                                            "      </rdf:Alt>\n     </crs:Group>\n    </rdf:Description>\n   </crs:Look>\n"),
                                   Look::Neutral);
        expect(vivid.adjust.look == Look::Enhanced && vivid.adjust.profile == "Adobe Vivid", "a profile recorded as Lightroom's crs:Look picks the look");
        Image changed = vivid;
        changed.adjust.look = Look::Neutral;
        changed.adjust.profile.clear();
        if (save(dir + "/vivid.CR2", changed))
        {
            const std::string text = sidecar_text(dir + "/vivid.CR2");
            expect(has(text, "crs:CameraProfile=\"Adobe Neutral\"") && !has(text, "crs:Look>") && !has(text, "Adobe Vivid"),
                   "a new look replaces the profile and the crs:Look that recorded the old one");
            expect(opened(dir + "/vivid.CR2", Look::Enhanced).adjust.look == Look::Neutral, "and reads back as chosen");
        }
    }

    // An unknown profile and a creative look survive a save untouched, as
    // does what isn't Camera Raw's.
    {
        const std::string raw =
            raw_with(dir, "custom",
                     "\n    xmp:Rating=\"4\" crs:ProcessVersion=\"11.0\" crs:CameraProfile=\"Some Vendor Film 7\" "
                     "crs:CameraProfileDigest=\"0123456789ABCDEF\" crs:Contrast2012=\"+10\"",
                     "   <crs:Look>\n    <rdf:Description crs:Name=\"Vintage 07\" crs:Amount=\"0.6\"/>\n   </crs:Look>\n");
        Image img = opened(raw, Look::Enhanced);
        expect(img.adjust.look == Look::CameraLike && img.adjust.profile == "Some Vendor Film 7", "an unknown profile renders Camera-like, named as it is");
        img.adjust.contrast = 20;
        if (save(raw, img))
        {
            const std::string text = sidecar_text(raw);
            expect(has(text, "crs:CameraProfile=\"Some Vendor Film 7\"") && has(text, "crs:CameraProfileDigest=\"0123456789ABCDEF\""),
                   "an unknown profile and its digest are kept on saving");
            expect(has(text, "crs:Name=\"Vintage 07\"") && has(text, "crs:Amount=\"0.6\""), "a creative crs:Look is kept on saving");
            expect(has(text, "xmp:Rating=\"4\"") && has(text, "<rdf:li>harbour</rdf:li>") && has(text, "<rdf:li>dusk</rdf:li>"),
                   "ratings and keywords are kept on saving");
            const Image again = opened(raw, Look::Neutral);
            expect(again.adjust.profile == "Some Vendor Film 7" && again.adjust.contrast == 20, "the unknown profile reads back, with the change");
        }
        img.adjust.look = Look::Enhanced;
        img.adjust.profile.clear();
        if (save(raw, img))
        {
            const std::string text = sidecar_text(raw);
            expect(has(text, "crs:CameraProfile=\"Adobe Vivid\"") && !has(text, "CameraProfileDigest"),
                   "Enhanced is written as Adobe Vivid, the old profile's digest gone");
            expect(has(text, "crs:Name=\"Vintage 07\""), "the creative look stays when the profile changes");
        }
    }

    // A control reset to rest is written where another source of the
    // image's settings set it (darktable's photo.CR2.xmp, read before
    // photo.xmp; the raw's embedded XMP), so it doesn't come back.
    {
        const std::string raw = dir + "/elsewhere.CR2";
        g_remove(xmp_sidecar_path(raw).c_str());
        const std::string other = std::string(kHead) +
                                  "\n    crs:ProcessVersion=\"11.0\" crs:Contrast2012=\"+30\" crs:Exposure2012=\"+0.50\" crs:WhiteBalance=\"Custom\" "
                                  "crs:Temperature=\"4000\" crs:Tint=\"+5\" crs:ToneCurveName2012=\"Medium Contrast\">\n" +
                                  kTail;
        g_file_set_contents((raw + ".xmp").c_str(), other.data(), gssize(other.size()), nullptr);
        const std::string embedded = std::string(kHead) + "\n    crs:Vibrance=\"+40\">\n" + kTail;
        const auto open = [&]
        {
            Image img;
            img.is_raw = true;
            read_xmp(raw, reinterpret_cast<const uint8_t*>(embedded.data()), embedded.size(), img);
            img.adjust = img.opening_adjust(true, Look::Neutral);
            return img;
        };
        Image img = open();
        expect(img.adjust.contrast == 30 && img.adjust.vibrance == 40 && img.xmp.exposure == 0.5 && img.xmp.temperature == 4000,
               "settings are read from photo.CR2.xmp and the embedded XMP");
        img.adjust.contrast = img.adjust.vibrance = 0;
        img.adjust.curve_name = "Linear";
        img.xmp.exposure = img.xmp.temperature = img.xmp.tint = 0;
        if (save(raw, img))
        {
            const Image again = open();
            expect(again.adjust.contrast == 0 && again.adjust.vibrance == 0, "controls reset to rest stay reset over other sources");
            expect(again.xmp.exposure == 0, "an exposure reset to 0 stays reset over another source");
            expect(again.xmp.temperature == 0, "white balance reset to as shot stays reset over another source");
            expect(again.adjust.curve_name == "Linear", "a curve reset to linear stays reset over another source");
        }
    }

    // The review's cases: Contrast2012 +30, Exposure2012 +1.25 and a
    // three-point ToneCurvePV2012 in the embedded XMP, then in
    // photo.CR2.xmp; reset to 0, 0 and linear and saved, they reopen reset.
    for (const bool embedded : {true, false})
    {
        const std::string raw = dir + (embedded ? "/inherit-embedded.CR2" : "/inherit-darktable.CR2");
        g_remove(xmp_sidecar_path(raw).c_str());
        g_remove((raw + ".xmp").c_str());
        const std::string packet = std::string(kHead) + "\n    crs:ProcessVersion=\"11.0\" crs:Contrast2012=\"+30\" crs:Exposure2012=\"+1.25\">\n" +
                                   "   <crs:ToneCurvePV2012>\n    <rdf:Seq>\n     <rdf:li>0, 0</rdf:li>\n     <rdf:li>128, 150</rdf:li>\n"
                                   "     <rdf:li>255, 255</rdf:li>\n    </rdf:Seq>\n   </crs:ToneCurvePV2012>\n" +
                                   kTail;
        if (!embedded)
            g_file_set_contents((raw + ".xmp").c_str(), packet.data(), gssize(packet.size()), nullptr);
        const auto open = [&]
        {
            Image img;
            img.is_raw = true;
            if (embedded)
                read_xmp(raw, reinterpret_cast<const uint8_t*>(packet.data()), packet.size(), img);
            else
                read_xmp(raw, nullptr, 0, img);
            img.adjust = img.opening_adjust(true, Look::Neutral);
            return img;
        };
        const std::string where = embedded ? " (embedded XMP)" : " (photo.CR2.xmp)";
        Image img = open();
        expect(img.adjust.contrast == 30 && img.xmp.exposure == 1.25 && img.adjust.curve.size() == 3, "the inherited settings are read" + where);
        img.adjust.contrast = 0;
        img.xmp.exposure = 0;
        img.adjust.curve.clear();
        if (save(raw, img))
        {
            const Image again = open();
            expect(again.adjust.contrast == 0, "Contrast reset to 0 stays reset" + where);
            expect(again.xmp.exposure == 0, "Exposure reset to 0 stays reset" + where);
            const Adjustments::Curve& c = again.adjust.curve;
            expect(c.empty() || c == Adjustments::Curve{{0, 0}, {255, 255}}, "the point curve reset to linear stays reset" + where);
        }
    }

    // The review's case: an untreated raw, the default Neutral, saved; the
    // default then Enhanced: it stays Adobe Neutral.
    {
        const std::string raw = dir + "/saved-neutral.CR2";
        g_remove(xmp_sidecar_path(raw).c_str());
        Image img;
        img.is_raw = true;
        img.adjust = img.opening_adjust(false, Look::Neutral);
        if (save(raw, img))
        {
            img.develop_saved(develop_of(img));
            expect(img.adjust.treated && img.adjust.profile == "Adobe Neutral", "a raw saved under the Neutral default is treated, Adobe Neutral");
            expect(has(sidecar_text(raw), "crs:CameraProfile=\"Adobe Neutral\""), "and its sidecar says Adobe Neutral");
            expect(opened(raw, Look::Enhanced).adjust.look == Look::Neutral, "and with the default then Enhanced it opens Neutral");
        }
    }

    // Saved, an untreated raw is treated, under the profile written: the
    // settings' default look no longer applies to it.
    {
        const std::string raw = dir + "/now-saved.CR2";
        g_remove(xmp_sidecar_path(raw).c_str());
        Image img;
        img.is_raw = true;
        img.adjust = img.opening_adjust(false, Look::Enhanced);
        img.adjust.contrast = 10;
        if (save(raw, img))
        {
            img.develop_saved(develop_of(img));
            expect(img.adjust.treated && img.xmp.treated && img.adjust.profile == "Adobe Vivid" && img.xmp.adjust.profile == "Adobe Vivid",
                   "a saved raw is treated, its profile the one written");
            expect(img.opening_adjust(true, Look::Neutral).look == Look::Enhanced, "and opens in its saved look, not the default");
        }
    }

    // Save writes the profile, the process and only the controls changed;
    // a control the sidecar has follows even back at rest.
    {
        const std::string raw = dir + "/fresh.CR2";
        g_remove(xmp_sidecar_path(raw).c_str());
        Image img;
        img.is_raw = true;
        img.adjust = img.opening_adjust(false, Look::CameraLike);
        img.adjust.vibrance = 15;
        if (save(raw, img))
        {
            const std::string text = sidecar_text(raw);
            expect(has(text, "crs:Vibrance=\"+15\"") && has(text, "crs:CameraProfile=\"Adobe Color\"") && has(text, "crs:ProcessVersion=\"11.0\"") &&
                       has(text, "crs:HasSettings=\"True\""),
                   "Save writes the changed control, the profile and the process");
            for (const char* rest : {"Contrast2012", "Saturation", "Exposure2012", "Sharpness", "ColorNoiseReduction", "WhiteBalance",
                                     "ToneCurvePV2012", "ToneCurveName2012", "Shadows2012", "HueAdjustmentRed", "Brightness"})
                expect(!has(text, std::string("crs:") + rest + "="), std::string("Save leaves out ") + rest + ", at rest");
        }
        const std::string kept = raw_with(dir, "kept", "\n    crs:ProcessVersion=\"11.0\" crs:Contrast2012=\"+30\"", "");
        Image back = opened(kept, Look::Neutral);
        back.adjust.contrast = 0;
        if (save(kept, back))
        {
            const std::string text = sidecar_text(kept);
            expect(has(text, "crs:Contrast2012=\"0\""), "a control the sidecar had is written back at rest");
            expect(has(text, "crs:CameraProfile=\"Adobe Color\""), "a treated raw without a profile is written as Adobe's default");
        }
    }

    std::printf("check-looks: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
