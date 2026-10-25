// OpenGL ES 3 through EGL, with no window: a surfaceless context (Mesa's,
// which also runs without a GPU as llvmpipe), else a 1×1 pbuffer on the
// default display. One context, made current on whichever thread is using
// it and released after, under a lock.
//
// The shaders reproduce Image::retone_buffer()'s arithmetic step for step
// (its tables become the same expressions, evaluated per pixel), and write
// unpremultiplied 8-bit values; the premultiply is done on the CPU as
// image.cpp's pack() does it, so the results match it exactly or within a
// step where float and double rounding part.
#include "gpu.h"

#include "image.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <glib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

namespace gpu
{
namespace
{

struct Device
{
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;  // a pbuffer, where surfaceless isn't offered
    std::string renderer;
    GLint max_size = 0;
    GLuint vao = 0;
    std::map<std::string, GLuint> programs;  // by the defines they were built with
    std::mutex mutex;
};

bool has_extension(const char* list, const char* name)
{
    const size_t n = std::strlen(name);
    for (const char* p = list; p && (p = std::strstr(p, name)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0'))
            return true;
    return false;
}

bool open(Device& d)
{
    const char* client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (get_platform_display && has_extension(client, "EGL_MESA_platform_surfaceless"))
        d.display = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (d.display == EGL_NO_DISPLAY)
        d.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (d.display == EGL_NO_DISPLAY || !eglInitialize(d.display, nullptr, nullptr) || !eglBindAPI(EGL_OPENGL_ES_API))
        return false;
    const char* extensions = eglQueryString(d.display, EGL_EXTENSIONS);
    const bool surfaceless = has_extension(extensions, "EGL_KHR_surfaceless_context");
    const EGLint config_attribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint configs = 0;
    if (!eglChooseConfig(d.display, config_attribs, &config, 1, &configs) || configs < 1)
        config = nullptr;
    if (!config && !(surfaceless && has_extension(extensions, "EGL_KHR_no_config_context")))
        return false;
    const EGLint context_attribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
    d.context = eglCreateContext(d.display, config, EGL_NO_CONTEXT, context_attribs);
    if (d.context == EGL_NO_CONTEXT)
        return false;
    if (!surfaceless)
    {
        const EGLint pbuffer_attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        d.surface = eglCreatePbufferSurface(d.display, config, pbuffer_attribs);
        if (d.surface == EGL_NO_SURFACE)
            return false;
    }
    if (!eglMakeCurrent(d.display, d.surface, d.surface, d.context))
        return false;
    d.renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &d.max_size);
    GLint rb = 0;
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &rb);
    d.max_size = std::min(d.max_size, rb);
    glGenVertexArrays(1, &d.vao);
    eglMakeCurrent(d.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return true;
}

Device* device()
{
    static Device* d = []() -> Device*
    {
        if (g_getenv("IFRIT_NO_GPU"))
            return nullptr;
        auto* dev = new Device;
        if (!open(*dev))
        {
            if (g_getenv("IFRIT_TRACE"))
                std::fprintf(stderr, "gpu: no OpenGL ES 3 context (EGL error 0x%x)\n", unsigned(eglGetError()));
            delete dev;
            return nullptr;
        }
        if (g_getenv("IFRIT_TRACE"))
            std::fprintf(stderr, "gpu: %s\n", dev->renderer.c_str());
        return dev;
    }();
    return d;
}

// The device current on this thread for as long as it lives.
class Current
{
public:
    explicit Current(Device& d) : d_(d), lock_(d.mutex) { eglMakeCurrent(d.display, d.surface, d.surface, d.context); }
    ~Current() { eglMakeCurrent(d_.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); }

private:
    Device& d_;
    std::lock_guard<std::mutex> lock_;
};

constexpr const char* kVertex = R"(#version 300 es
void main()
{
    // One triangle over the whole target.
    vec2 p = vec2(float((gl_VertexID & 1) << 2), float((gl_VertexID & 2) << 1)) - 1.0;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

// Each kind of input is its own program (the sampler's type differs);
// the defines say which.
constexpr const char* kFragment = R"(
precision highp float;
precision highp int;
uniform int channels;      // of the source: 1 grey, 2 grey + alpha, 3 RGB, 4 RGBA
uniform float exposure;    // 2^ev
uniform float gain;        // raw without a tone curve: auto gain × exposure
#if defined(SOURCE_UINT)
uniform highp usampler2D source;
#elif defined(SOURCE_INT)
uniform highp isampler2D source;
#elif defined(RAW)
uniform highp usampler2D source;
uniform highp sampler2D tone;  // 4096 values, 64 × 64
#else
uniform highp sampler2D source;
#endif
out vec4 colour;

float srgb_encode(float v)
{
    v = clamp(v, 0.0, 1.0);
    return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}
float byte_of(float v) { return floor(v * 255.0 + 0.5); }
// To the nearest half, as image.cpp looks floats up by their nearest half.
float as_half(float v) { return unpackHalf2x16(packHalf2x16(vec2(v, 0.0))).x; }

void main()
{
    ivec2 at = ivec2(gl_FragCoord.xy);
#if defined(RAW)
    vec3 s = vec3(texelFetch(source, at, 0).rgb);
#  if defined(TONE)
    vec3 c = s * (exposure / 65535.0);
    float lum = 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
    int i = min(4095, int(lum * 4095.0 + 0.5));
    float o = texelFetch(tone, ivec2(i % 64, i / 64), 0).r;
    float ratio = lum > 1e-6 ? o / lum : 0.0;
    ivec3 k = clamp(ivec3(c * ratio * 4095.0 + 0.5), ivec3(0), ivec3(4095));
    vec3 rgb = vec3(byte_of(srgb_encode(float(k.r) / 4095.0)), byte_of(srgb_encode(float(k.g) / 4095.0)),
                    byte_of(srgb_encode(float(k.b) / 4095.0)));
#  else
    vec3 rgb = vec3(byte_of(srgb_encode(s.r / 65535.0 * gain)), byte_of(srgb_encode(s.g / 65535.0 * gain)),
                    byte_of(srgb_encode(s.b / 65535.0 * gain)));
#  endif
    colour = vec4(rgb, 255.0) / 255.0;
#else
#  if defined(SOURCE_UINT)
    vec4 v = vec4(texelFetch(source, at, 0)) / 65535.0;
#  elif defined(SOURCE_INT)
    vec4 v = max(vec4(-1.0), vec4(texelFetch(source, at, 0)) / 32767.0);
#  else
    vec4 v = texelFetch(source, at, 0);
#  endif
    vec4 px = channels >= 3 ? v : vec4(v.r, v.r, v.r, v.g);
    float a = channels == 2 || channels == 4 ? (channels == 2 ? v.g : v.a) : 1.0;
#  if defined(LINEAR)
    // Exposed, to the nearest half, then encoded; NaN shows as nothing.
    vec3 rgb;
    for (int c = 0; c < 3; ++c)
    {
        float h = as_half(px[c] * exposure);
        rgb[c] = isnan(h) ? 0.0 : byte_of(srgb_encode(h));
    }
    float ha = as_half(a);
    float alpha = channels == 2 || channels == 4 ? (isnan(ha) ? 0.0 : byte_of(clamp(ha, 0.0, 1.0))) : 255.0;
#  else
#    if defined(SOURCE_INT)
    px.rgb = (px.rgb + 1.0) / 2.0;
#    endif
    vec3 rgb = vec3(byte_of(clamp(px.r, 0.0, 1.0)), byte_of(clamp(px.g, 0.0, 1.0)), byte_of(clamp(px.b, 0.0, 1.0)));
    float alpha = channels == 2 || channels == 4 ? byte_of(clamp(a, 0.0, 1.0)) : 255.0;
#  endif
    colour = vec4(rgb, alpha) / 255.0;
#endif
}
)";

GLuint compile(GLenum type, const std::string& text, std::string& error)
{
    GLuint shader = glCreateShader(type);
    const char* src = text.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[2048] = {};
        glGetShaderInfoLog(shader, sizeof log, nullptr, log);
        error = std::string("shader: ") + log;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint program(Device& d, const std::string& defines, std::string& error)
{
    if (auto it = d.programs.find(defines); it != d.programs.end())
        return it->second;
    GLuint vs = compile(GL_VERTEX_SHADER, kVertex, error);
    GLuint fs = vs ? compile(GL_FRAGMENT_SHADER, "#version 300 es\n" + defines + kFragment, error) : 0;
    if (!fs)
    {
        if (vs)
            glDeleteShader(vs);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[2048] = {};
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        error = std::string("shader: ") + log;
        glDeleteProgram(p);
        return 0;
    }
    d.programs[defines] = p;
    return p;
}

// A texture of the samples as they are, for texelFetch: no filtering, no mips.
GLuint texture(GLenum internal, GLenum format, GLenum type, int w, int h, const void* data)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GLint(internal), w, h, 0, format, type, data);
    return t;
}

// Unpremultiplied RGBA bytes to image.cpp's premultiplied ARGB, as its pack() does.
void pack_rows(const std::vector<uint8_t>& rgba, int w, int h, PixelBuffer& out)
{
    out.assign(size_t(w) * h * 4, 0);
    for (size_t i = 0, n = size_t(w) * h; i < n; ++i)
    {
        uint32_t r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
        if (a != 255)
        {
            r = (r * a + 127) / 255;
            g = (g * a + 127) / 255;
            b = (b * a + 127) / 255;
        }
        const uint32_t v = (a << 24) | (r << 16) | (g << 8) | b;
        std::memcpy(out.data() + i * 4, &v, 4);
    }
}

}  // namespace

bool available()
{
    return device() != nullptr;
}

std::string renderer()
{
    Device* d = device();
    return d ? d->renderer : std::string();
}

bool render(const Image& image, double ev, PixelBuffer& out, std::string& error)
{
    Device* d = device();
    if (!d)
    {
        error = "no GPU";
        return false;
    }
    const LinearData* raw = image.linear.get();
    const SourceData* s = raw ? nullptr : image.source.get();
    if (!raw && !s)
    {
        error = "nothing beyond its 8-bit pixels";
        return false;
    }
    const int w = image.width, h = image.height;
    if (w <= 0 || h <= 0 || w > d->max_size || h > d->max_size || (s && (s->width != w || s->height != h)) ||
        (raw && raw->rgb.size() != size_t(w) * h * 3))
    {
        error = "too big for one texture, or its samples don't fit it";
        return false;
    }
    Current current(*d);

    // The samples as they are: integer textures for integers, float for halves and floats.
    std::string defines;
    GLuint source = 0, tone = 0;
    if (raw)
    {
        defines = raw->tone.empty() ? "#define RAW\n" : "#define RAW\n#define TONE\n";
        source = texture(GL_RGB16UI, GL_RGB_INTEGER, GL_UNSIGNED_SHORT, w, h, raw->rgb.data());
        if (!raw->tone.empty())
        {
            std::vector<float> table(4096, raw->tone.back());
            std::copy_n(raw->tone.begin(), std::min<size_t>(4096, raw->tone.size()), table.begin());
            tone = texture(GL_R32F, GL_RED, GL_FLOAT, 64, 64, table.data());
        }
    }
    else
    {
        static const GLenum formats[4][3] = {
            {GL_RED_INTEGER, GL_RG_INTEGER, GL_RGB_INTEGER},  // integer: 1, 2, 3 channels (4 below)
            {GL_RED, GL_RG, GL_RGB},
        };
        const int c = s->channels;
        const bool integer = s->sample == SourceData::Sample::U16 || s->sample == SourceData::Sample::I16;
        const GLenum format = c == 4 ? (integer ? GL_RGBA_INTEGER : GL_RGBA) : formats[integer ? 0 : 1][c - 1];
        GLenum internal = 0, type = 0;
        switch (s->sample)
        {
            case SourceData::Sample::U16:
                internal = c == 1 ? GL_R16UI : c == 2 ? GL_RG16UI : c == 3 ? GL_RGB16UI : GL_RGBA16UI;
                type = GL_UNSIGNED_SHORT;
                defines = "#define SOURCE_UINT\n";
                break;
            case SourceData::Sample::I16:
                internal = c == 1 ? GL_R16I : c == 2 ? GL_RG16I : c == 3 ? GL_RGB16I : GL_RGBA16I;
                type = GL_SHORT;
                defines = "#define SOURCE_INT\n";
                break;
            case SourceData::Sample::F16:
                internal = c == 1 ? GL_R16F : c == 2 ? GL_RG16F : c == 3 ? GL_RGB16F : GL_RGBA16F;
                type = GL_HALF_FLOAT;
                break;
            case SourceData::Sample::F32:
                internal = c == 1 ? GL_R32F : c == 2 ? GL_RG32F : c == 3 ? GL_RGB32F : GL_RGBA32F;
                type = GL_FLOAT;
                break;
        }
        if (s->linear)
            defines += "#define LINEAR\n";
        source = texture(internal, format, type, w, h, s->data.data());
    }
    const GLuint p = program(*d, defines, error);

    // Drawn into an 8-bit target the size of the image, and read back.
    GLuint target = 0, fbo = 0;
    glGenRenderbuffers(1, &target);
    glBindRenderbuffer(GL_RENDERBUFFER, target);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, target);
    bool ok = p && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok)
    {
        glUseProgram(p);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, source);
        glUniform1i(glGetUniformLocation(p, "source"), 0);
        if (tone)
        {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, tone);
            glUniform1i(glGetUniformLocation(p, "tone"), 1);
        }
        const double exposure = std::pow(2.0, ev);
        glUniform1i(glGetUniformLocation(p, "channels"), s ? s->channels : 3);
        glUniform1f(glGetUniformLocation(p, "exposure"), float(exposure));
        glUniform1f(glGetUniformLocation(p, "gain"), float(raw ? raw->auto_gain * exposure : exposure));
        glViewport(0, 0, w, h);
        glBindVertexArray(d->vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        std::vector<uint8_t> rgba(size_t(w) * h * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        if (const GLenum e = glGetError(); e != GL_NO_ERROR)
        {
            error = "OpenGL error " + std::to_string(e);
            ok = false;
        }
        else
            pack_rows(rgba, w, h, out);
    }
    else if (error.empty())
        error = "can't draw into an image this size";
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteRenderbuffers(1, &target);
    glDeleteTextures(1, &source);
    if (tone)
        glDeleteTextures(1, &tone);
    return ok;
}

}  // namespace gpu
