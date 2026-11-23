#ifndef EXPOSURE_HLSLI
#define EXPOSURE_HLSLI

// A raw's automatic exposure (peak.hlsl), in HLSL that C++ compiles too
// (ifritview --check-exposure; inline for that, so it's defined once): it's
// included there after something that gives clamp, min and max on floats,
// and log2, exp2 and log10.
//
// The scene's light, absolute, at its nominal ISO: the luminance at the
// raw's stated white from its sensitivity S, shutter t and f-number N by
// ISO 12232's saturation-based relation, S_sat = 78 / H_sat, with the
// exposure at the focal plane H = q L t / N^2 and q = 0.65 (the standard's
// lens: transmittance, vignetting and angle), so L_white = 78 N^2 / (q S t)
// in cd/m^2. S is the nominal ISO, whatever rating the EXIF says it is (an
// ISO 12232 speed, noise- or saturation-based; a standard output
// sensitivity; a recommended exposure index: CIPA DC-008), each only an
// approximation of S_sat, a fraction of a stop off; exposure.h's table of
// cited per-camera corrections is the only calibration of it. The bias,
// alike across a camera's pictures, is absorbed in calibrating `kNightDepth`. A
// pixel's luminance is its own on the stated white's scale (1 = the stated
// white; the develop's values times the white it scaled to) times L_white,
// and the scene's adapting luminance L_A is their log average.
//
// The key the display shows it at follows L_A through the eye's mesopic
// range, CIE 191:2010's limits: middle grey (0.18) for a photopic scene,
// L_A of 5 cd/m^2 or more, falling in proportion to log L_A to
// `kNightDepth` EV under it at 0.005 cd/m^2, the scotopic limit, and no
// further: within the range, key = 0.18 (L_A / 5)^1.30. The gain is only
// ever lifted toward the key, so it's graded, not a switch: a scene the
// camera exposed dark is lifted less the dimmer it was, from a stop to
// none over about 3.3 stops of scene light, and a night scene whose key is
// under what the camera made of it keeps the camera's exposure, shown dark
// because it was, its few lights not lifting its dark to grey.

// L_white for an ISO speed, shutter time (s) and f-number; 0 when any is unknown.
static const float kSaturationQ = 0.65f;
inline float saturation_luminance(float iso, float shutter, float f_number)
{
    if (!(iso > 0 && shutter > 0 && f_number > 0))
        return 0;
    return 78 * f_number * f_number / (kSaturationQ * iso * shutter);
}

static const float kMiddleGrey = 0.18f;
static const float kPhotopic = 5.0f;     // cd/m^2: CIE 191's mesopic range, top
static const float kScotopic = 0.005f;   // and bottom
// Style, `night_depth`: how far under middle grey a scotopic scene's key
// is, in EV; 4.3 EV a decade of L_A. A calibrated law, not a measured one. Calibrated on night raws the camera's
// own exposure already shows right, with daylight and room light as they
// were: a dark scene at 0.5 cd/m^2 keeps the camera's exposure (key 4 EV
// under grey), and a room at 3 cd/m^2 can still be lifted (1 EV under).
static const float kNightDepth = 13.0f;

// How far through the mesopic range L_A is: 0 photopic, 1 scotopic.
inline float mesopic_depth(float adapting)
{
    return clamp(log10(kPhotopic / max(adapting, 1e-30f)) / log10(kPhotopic / kScotopic), 0.0f, 1.0f);
}

// The adapting luminance from the log average at the develop's scale, the
// white it scaled to (1 = the stated white) and L_white; 0 when L_white is.
inline float adapting_luminance(float average, float develop_white, float white_luminance)
{
    return average * develop_white * white_luminance;
}

// The display key for an adapting luminance; middle grey when it's unknown (0).
inline float display_key(float adapting)
{
    if (!(adapting > 0))
        return kMiddleGrey;
    return kMiddleGrey * exp2(-kNightDepth * mesopic_depth(adapting));
}

// The gain, from the camera's (`anchor`: its middle grey where its JPEG
// has it), the image's log average and 99th percentile luminance at the
// raw's scale, and its key. The camera's metering is the photographer's
// intent, so the gain moves from it a stop at most (`kDeparture`): up as
// far as puts the log average at the key, down only as far as puts the
// 99th percentile at white (the roll-off takes the 1% above).
static const float kDeparture = 1.0f;
inline float exposure_gain(float anchor, float average, float p99, float key)
{
    const float bright = p99 > 0 ? 1 / p99 : 1;
    const float lift = average > 0 ? key / average : anchor;
    return anchor * exp2(clamp(log2(min(bright, max(lift, anchor)) / anchor), -kDeparture, kDeparture));
}

// The gain a raw is shown at. A treated raw's settings (a Lightroom
// develop, Adjustments::treated) are Adobe's, made on the camera's exposure
// plus BaselineExposure with no automatic departure from it: the camera's
// (`anchor`), no key lift and no fitting. Else the automatic gain.
inline float shown_gain(bool treated, float anchor, float automatic)
{
    return treated && anchor > 0 ? anchor : automatic;
}

#endif
