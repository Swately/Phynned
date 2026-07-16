// framework/render/opengl3/src/FSR1Pass.cpp
// FSR1Pass — AMD FidelityFX Super Resolution 1.0, OpenGL 3.3 implementation.
//
#include <phyriad/render/opengl3/FSR1Pass.hpp>
#include <phyriad/schema/Error.hpp>

// GLFW for glfwGetProcAddress — must come before any GL type definitions.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstring>
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
// Minimal GL 3.3 loader
// Defines only the types and function pointers required by FSR1Pass.
// All functions are loaded once via glfwGetProcAddress (GLFW context must be
// current before FSR1Pass::init() is called).
// Types follow the Khronos glcorearb.h convention (MIT licence).
// ─────────────────────────────────────────────────────────────────────────────
#ifndef APIENTRY
#  ifdef _WIN32
#    define APIENTRY __stdcall
#  else
#    define APIENTRY
#  endif
#endif
#define APIENTRYP APIENTRY *

// Basic scalar types (subset of glcorearb.h).
typedef unsigned int  GLenum;
typedef unsigned int  GLuint;
typedef int           GLint;
typedef int           GLsizei;
typedef unsigned char GLboolean;
typedef float         GLfloat;
typedef unsigned int  GLbitfield;
typedef char          GLchar;
typedef void          GLvoid;
#ifdef _WIN64
typedef signed long long GLsizeiptr;
#elif defined(_WIN32)
typedef signed long      GLsizeiptr;
#else
typedef long             GLsizeiptr;
#endif

// Constants.
#ifndef GL_FALSE
#define GL_FALSE                        0
#define GL_TRUE                         1
#endif
#define GL_TRIANGLES                    0x0004u
#define GL_FLOAT                        0x1406u
#define GL_UNSIGNED_BYTE                0x1401u
#define GL_UNSIGNED_INT                 0x1405u
#define GL_RGBA                         0x1908u
#define GL_RGBA8                        0x8058u
#define GL_TEXTURE_2D                   0x0DE1u
#define GL_TEXTURE_MAG_FILTER           0x2800u
#define GL_TEXTURE_MIN_FILTER           0x2801u
#define GL_TEXTURE_WRAP_S               0x2802u
#define GL_TEXTURE_WRAP_T               0x2803u
#define GL_LINEAR                       0x2601u
#define GL_CLAMP_TO_EDGE                0x812Fu
#define GL_FRAMEBUFFER                  0x8D40u
#define GL_DRAW_FRAMEBUFFER             0x8CA9u
#define GL_COLOR_ATTACHMENT0            0x8CE0u
#define GL_FRAMEBUFFER_COMPLETE         0x8CD5u
#define GL_ARRAY_BUFFER                 0x8892u
#define GL_STATIC_DRAW                  0x88B4u
#define GL_VERTEX_SHADER                0x8B31u
#define GL_FRAGMENT_SHADER              0x8B30u
#define GL_COMPILE_STATUS               0x8B81u
#define GL_LINK_STATUS                  0x8B82u
#define GL_INFO_LOG_LENGTH              0x8B84u
#define GL_TEXTURE0                     0x84C0u

// Function pointer typedefs.
typedef void    (APIENTRYP PFNGLGENFRAMEBUFFERSPROC)         (GLsizei n, GLuint* ids);
typedef void    (APIENTRYP PFNGLBINDFRAMEBUFFERPROC)         (GLenum target, GLuint fb);
typedef void    (APIENTRYP PFNGLFRAMEBUFFERTEXTURE2DPROC)    (GLenum target, GLenum attach, GLenum texTarget, GLuint tex, GLint level);
typedef GLenum  (APIENTRYP PFNGLCHECKFRAMEBUFFERSTATUSPROC)  (GLenum target);
typedef void    (APIENTRYP PFNGLDELETEFRAMEBUFFERSPROC)      (GLsizei n, const GLuint* ids);
typedef void    (APIENTRYP PFNGLGENTEXTURESPROC)             (GLsizei n, GLuint* textures);
typedef void    (APIENTRYP PFNGLBINDTEXTUREPROC)             (GLenum target, GLuint texture);
typedef void    (APIENTRYP PFNGLTEXIMAGE2DPROC)              (GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type, const void* data);
typedef void    (APIENTRYP PFNGLTEXPARAMETERIPROC)           (GLenum target, GLenum pname, GLint param);
typedef void    (APIENTRYP PFNGLDELETETEXTURESPROC)          (GLsizei n, const GLuint* textures);
typedef void    (APIENTRYP PFNGLACTIVETEXTUREPROC)           (GLenum texture);
typedef GLuint  (APIENTRYP PFNGLCREATESHADERPROC)            (GLenum type);
typedef void    (APIENTRYP PFNGLSHADERSOURCEPROC)            (GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length);
typedef void    (APIENTRYP PFNGLCOMPILESHADERPROC)           (GLuint shader);
typedef void    (APIENTRYP PFNGLGETSHADERIVPROC)             (GLuint shader, GLenum pname, GLint* params);
typedef void    (APIENTRYP PFNGLGETSHADERINFOLOGPROC)        (GLuint shader, GLsizei maxLen, GLsizei* length, GLchar* infoLog);
typedef void    (APIENTRYP PFNGLDELETESHADERPROC)            (GLuint shader);
typedef GLuint  (APIENTRYP PFNGLCREATEPROGRAMPROC)           (void);
typedef void    (APIENTRYP PFNGLATTACHSHADERPROC)            (GLuint program, GLuint shader);
typedef void    (APIENTRYP PFNGLLINKPROGRAMPROC)             (GLuint program);
typedef void    (APIENTRYP PFNGLGETPROGRAMIVPROC)            (GLuint program, GLenum pname, GLint* params);
typedef void    (APIENTRYP PFNGLGETPROGRAMINFOLOGPROC)       (GLuint program, GLsizei maxLen, GLsizei* length, GLchar* infoLog);
typedef void    (APIENTRYP PFNGLDELETEPROGRAMPROC)           (GLuint program);
typedef void    (APIENTRYP PFNGLUSEPROGRAMPROC)              (GLuint program);
typedef GLint   (APIENTRYP PFNGLGETUNIFORMLOCATIONPROC)      (GLuint program, const GLchar* name);
typedef void    (APIENTRYP PFNGLUNIFORM1IPROC)               (GLint location, GLint v0);
typedef void    (APIENTRYP PFNGLUNIFORM4UIVPROC)             (GLint location, GLsizei count, const GLuint* value);
typedef void    (APIENTRYP PFNGLGENVERTEXARRAYSPROC)         (GLsizei n, GLuint* arrays);
typedef void    (APIENTRYP PFNGLBINDVERTEXARRAYPROC)         (GLuint array);
typedef void    (APIENTRYP PFNGLDELETEVERTEXARRAYSPROC)      (GLsizei n, const GLuint* arrays);
typedef void    (APIENTRYP PFNGLGENBUFFERSPROC)              (GLsizei n, GLuint* buffers);
typedef void    (APIENTRYP PFNGLBINDBUFFERPROC)              (GLenum target, GLuint buffer);
typedef void    (APIENTRYP PFNGLBUFFERDATAPROC)              (GLenum target, GLsizeiptr size, const void* data, GLenum usage);
typedef void    (APIENTRYP PFNGLDELETEBUFFERSPROC)           (GLsizei n, const GLuint* buffers);
typedef void    (APIENTRYP PFNGLVERTEXATTRIBPOINTERPROC)     (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer);
typedef void    (APIENTRYP PFNGLENABLEVERTEXATTRIBARRAYPROC) (GLuint index);
typedef void    (APIENTRYP PFNGLDRAWARRAYSPROC)              (GLenum mode, GLint first, GLsizei count);
typedef void    (APIENTRYP PFNGLVIEWPORTPROC)                (GLint x, GLint y, GLsizei w, GLsizei h);

// Function pointer storage (file scope, initialized once).
static PFNGLGENFRAMEBUFFERSPROC         gl_GenFramebuffers        {nullptr};
static PFNGLBINDFRAMEBUFFERPROC         gl_BindFramebuffer        {nullptr};
static PFNGLFRAMEBUFFERTEXTURE2DPROC    gl_FramebufferTexture2D   {nullptr};
static PFNGLCHECKFRAMEBUFFERSTATUSPROC  gl_CheckFramebufferStatus {nullptr};
static PFNGLDELETEFRAMEBUFFERSPROC      gl_DeleteFramebuffers     {nullptr};
static PFNGLGENTEXTURESPROC             gl_GenTextures            {nullptr};
static PFNGLBINDTEXTUREPROC             gl_BindTexture            {nullptr};
static PFNGLTEXIMAGE2DPROC              gl_TexImage2D             {nullptr};
static PFNGLTEXPARAMETERIPROC           gl_TexParameteri          {nullptr};
static PFNGLDELETETEXTURESPROC          gl_DeleteTextures         {nullptr};
static PFNGLACTIVETEXTUREPROC           gl_ActiveTexture          {nullptr};
static PFNGLCREATESHADERPROC            gl_CreateShader           {nullptr};
static PFNGLSHADERSOURCEPROC            gl_ShaderSource           {nullptr};
static PFNGLCOMPILESHADERPROC           gl_CompileShader          {nullptr};
static PFNGLGETSHADERIVPROC             gl_GetShaderiv            {nullptr};
static PFNGLGETSHADERINFOLOGPROC        gl_GetShaderInfoLog       {nullptr};
static PFNGLDELETESHADERPROC            gl_DeleteShader           {nullptr};
static PFNGLCREATEPROGRAMPROC           gl_CreateProgram          {nullptr};
static PFNGLATTACHSHADERPROC            gl_AttachShader           {nullptr};
static PFNGLLINKPROGRAMPROC             gl_LinkProgram            {nullptr};
static PFNGLGETPROGRAMIVPROC            gl_GetProgramiv           {nullptr};
static PFNGLGETPROGRAMINFOLOGPROC       gl_GetProgramInfoLog      {nullptr};
static PFNGLDELETEPROGRAMPROC           gl_DeleteProgram          {nullptr};
static PFNGLUSEPROGRAMPROC              gl_UseProgram             {nullptr};
static PFNGLGETUNIFORMLOCATIONPROC      gl_GetUniformLocation     {nullptr};
static PFNGLUNIFORM1IPROC               gl_Uniform1i              {nullptr};
static PFNGLUNIFORM4UIVPROC             gl_Uniform4uiv            {nullptr};
static PFNGLGENVERTEXARRAYSPROC         gl_GenVertexArrays        {nullptr};
static PFNGLBINDVERTEXARRAYPROC         gl_BindVertexArray        {nullptr};
static PFNGLDELETEVERTEXARRAYSPROC      gl_DeleteVertexArrays     {nullptr};
static PFNGLGENBUFFERSPROC              gl_GenBuffers             {nullptr};
static PFNGLBINDBUFFERPROC              gl_BindBuffer             {nullptr};
static PFNGLBUFFERDATAPROC              gl_BufferData             {nullptr};
static PFNGLDELETEBUFFERSPROC           gl_DeleteBuffers          {nullptr};
static PFNGLVERTEXATTRIBPOINTERPROC     gl_VertexAttribPointer    {nullptr};
static PFNGLENABLEVERTEXATTRIBARRAYPROC gl_EnableVertexAttribArray{nullptr};
static PFNGLDRAWARRAYSPROC              gl_DrawArrays             {nullptr};
static PFNGLVIEWPORTPROC                gl_Viewport_              {nullptr};

static bool s_gl_loaded = false;

// ─────────────────────────────────────────────────────────────────────────────
// Shader source — embedded as raw string literals
// ─────────────────────────────────────────────────────────────────────────────

// Shared vertex shader: full-screen quad, position + UV.
static constexpr char kVertSrc[] = R"GLSL(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
out vec2 vUV;
void main() {
    vUV         = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)GLSL";

// EASU fragment shader — Edge-Adaptive Spatial Upsampling.
//
// Constants layout (all floats packed into uint via floatBitsToUint):
//   con0.xy  = (inW/outW, inH/outH)            — scale from output pixel → input texel
//   con0.zw  = (0.5*inW/outW - 0.5,            — offsets (correct half-pixel alignment)
//               0.5*inH/outH - 0.5)
//   con1.xy  = (1/inW, 1/inH)                  — one-texel UV step in input texture
//
// Algorithm:
//   1. Map output pixel gl_FragCoord → sub-texel position pp in input space.
//   2. Sample 12-tap neighborhood (4×4 minus 4 corners) around pp.
//   3. Compute gradient direction from luma differences.
//   4. Apply anisotropic filter: wider along edges, narrower across them.
static constexpr char kEasuFragSrc[] = R"GLSL(
#version 330 core
precision highp float;

uniform sampler2D texSrc;
uniform uvec4 con0;   // [f(inW/outW), f(inH/outH), f(0.5*inW/outW-0.5), f(0.5*inH/outH-0.5)]
uniform uvec4 con1;   // [f(1/inW),    f(1/inH),    unused,               unused              ]

in  vec2 vUV;
out vec4 fragColor;

// Fast luma (2× linear luma, avoids normalization — only used for comparisons).
float luma(vec3 c) { return 0.5 * c.r + c.g + 0.5 * c.b; }

// Sample input at texel offset (ox, oy) from base UV.
vec3 samp(vec2 base, vec2 d, vec2 off) {
    return texture(texSrc, base + d * off).rgb;
}

// Anisotropic filter weight.
// d_par   = distance along edge direction        (wide: soft cutoff)
// d_perp  = distance perpendicular to edge       (narrow: hard cutoff)
float fw(float d_par, float d_perp) {
    float wp = max(0.0, 1.0 - abs(d_par)  * 0.5);   // along-edge: ±2 texels
    float wq = max(0.0, 1.0 - abs(d_perp) * 2.0);   // cross-edge: ±0.5 texels
    return wp * wp * wq * wq;
}

void main() {
    float scX  = uintBitsToFloat(con0.x);
    float scY  = uintBitsToFloat(con0.y);
    float ofX  = uintBitsToFloat(con0.z);
    float ofY  = uintBitsToFloat(con0.w);
    float rcpW = uintBitsToFloat(con1.x);
    float rcpH = uintBitsToFloat(con1.y);

    // Map output pixel (0-based via gl_FragCoord − 0.5) to input texel space.
    vec2 pp   = gl_FragCoord.xy * vec2(scX, scY) + vec2(ofX, ofY);
    vec2 fp   = floor(pp);   // integer base texel
    vec2 pf   = pp - fp;     // sub-texel fraction in [0,1)

    // Base UV = center of integer base texel.
    vec2 base = (fp + vec2(0.5)) * vec2(rcpW, rcpH);
    vec2 d    = vec2(rcpW, rcpH);

    // 12-tap neighborhood (4×4 minus 4 corners).
    //      B C
    //    E F G H
    //    I J K L
    //      N O
    // Center (filter origin) is at sub-pixel (pf.x, pf.y) within the F→G/J→K quad.
    vec3 vB = samp(base, d, vec2( 0.0, -1.0));
    vec3 vC = samp(base, d, vec2( 1.0, -1.0));
    vec3 vE = samp(base, d, vec2(-1.0,  0.0));
    vec3 vF = samp(base, d, vec2( 0.0,  0.0));
    vec3 vG = samp(base, d, vec2( 1.0,  0.0));
    vec3 vH = samp(base, d, vec2( 2.0,  0.0));
    vec3 vI = samp(base, d, vec2(-1.0,  1.0));
    vec3 vJ = samp(base, d, vec2( 0.0,  1.0));
    vec3 vK = samp(base, d, vec2( 1.0,  1.0));
    vec3 vL = samp(base, d, vec2( 2.0,  1.0));
    vec3 vN = samp(base, d, vec2( 0.0,  2.0));
    vec3 vO = samp(base, d, vec2( 1.0,  2.0));

    float lB = luma(vB), lC = luma(vC);
    float lE = luma(vE), lF = luma(vF);
    float lG = luma(vG), lH = luma(vH);
    float lI = luma(vI), lJ = luma(vJ);
    float lK = luma(vK), lL = luma(vL);
    float lN = luma(vN), lO = luma(vO);

    // Gradient from the central 2x2 quad (F G / J K) plus weighted outer taps.
    // All differences: (right/down) - (left/up) → consistent sign convention.
    // gx positive = brighter on the right side.
    // gy positive = brighter at the bottom.
    float gx = (lG - lF) + (lK - lJ)
             + 0.5 * ((lC - lB) + (lH - lE) + (lL - lI) + (lO - lN));
    float gy = (lJ - lF) + (lK - lG)
             + 0.5 * ((lI - lE) + (lL - lH) + (lN - lB) + (lO - lC));

    float gl_  = sqrt(gx * gx + gy * gy);

    // Flat region — fall back to plain bilinear at sub-pixel position.
    if (gl_ < 1e-3) {
        fragColor = vec4(texture(texSrc, base + d * pf).rgb, 1.0);
        return;
    }

    // Unit vector pointing in the gradient direction (across the edge).
    vec2 dir = vec2(gx, gy) / gl_;

    // For each tap, compute its relative position from the filter center,
    // then decompose into components along/across the edge.
    // Weight is anisotropic: wide along the edge, narrow across it.
    vec3  col  = vec3(0.0);
    float wsum = 0.0;

    #define TAP(v, ox, oy) {                                  \
        vec2  rel   = vec2(float(ox), float(oy)) - pf;       \
        float d_par  = rel.x * (-dir.y) + rel.y * dir.x;    \
        float d_perp = rel.x *   dir.x  + rel.y * dir.y;    \
        float wt    = fw(d_par, d_perp);                     \
        col  += (v) * wt;                                     \
        wsum += wt;                                           \
    }

    TAP(vB,  0.0, -1.0)
    TAP(vC,  1.0, -1.0)
    TAP(vE, -1.0,  0.0)
    TAP(vF,  0.0,  0.0)
    TAP(vG,  1.0,  0.0)
    TAP(vH,  2.0,  0.0)
    TAP(vI, -1.0,  1.0)
    TAP(vJ,  0.0,  1.0)
    TAP(vK,  1.0,  1.0)
    TAP(vL,  2.0,  1.0)
    TAP(vN,  0.0,  2.0)
    TAP(vO,  1.0,  2.0)

    #undef TAP

    fragColor = vec4(col / max(wsum, 1e-5), 1.0);
}
)GLSL";

// RCAS fragment shader — Robust Contrast-Adaptive Sharpening.
//
// Constants:
//   con0.x = f(exp2(-sharpness))    — sharpening amplitude (AMD convention)
//   con0.y = f(1 / output_width)
//   con0.z = f(1 / output_height)
//
// Algorithm: 5-tap cross filter.  Detect local min/max luma → compute
// negative-lobe weight → blend center with neighbors.
static constexpr char kRcasFragSrc[] = R"GLSL(
#version 330 core
precision highp float;

uniform sampler2D texSrc;
uniform uvec4 con0;   // [f(exp2(-sharpness)), f(1/outW), f(1/outH), 0]

in  vec2 vUV;
out vec4 fragColor;

float luma(vec3 c) { return 0.213 * c.r + 0.715 * c.g + 0.072 * c.b; }

void main() {
    float amp  = uintBitsToFloat(con0.x);   // exp2(-sharpness), range (0,1]
    float rcpW = uintBitsToFloat(con0.y);
    float rcpH = uintBitsToFloat(con0.z);

    vec2 dx = vec2(rcpW, 0.0);
    vec2 dy = vec2(0.0,  rcpH);

    // 5-tap cross.
    vec3 e = texture(texSrc, vUV         ).rgb;
    vec3 b = texture(texSrc, vUV + dy    ).rgb;
    vec3 d = texture(texSrc, vUV - dx    ).rgb;
    vec3 f = texture(texSrc, vUV + dx    ).rgb;
    vec3 h = texture(texSrc, vUV - dy    ).rgb;

    float le = luma(e), lb = luma(b), ld = luma(d), lf = luma(f), lh = luma(h);

    float mn = min(le, min(lb, min(ld, min(lf, lh))));
    float mx = max(le, max(lb, max(ld, max(lf, lh))));

    // Negative-lobe sharpening weight (AMD RCAS formula).
    float peak  = clamp(min(mn, 2.0 - mx) / mx, 0.0, 1.0);
    float w     = -sqrt(peak) * amp * 0.25;   // 0.25 = 1/(4 taps)

    vec3  col   = (b + d + f + h) * w + e;
    float norm  = 1.0 + 4.0 * w;
    fragColor   = vec4(clamp(col / norm, 0.0, 1.0), 1.0);
}
)GLSL";

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Float → uint bit-cast (IEEE 754).
static inline uint32_t f2u(float f) noexcept {
    uint32_t u; std::memcpy(&u, &f, 4); return u;
}

// Compile a single GLSL shader stage and return its handle (0 on failure).
static GLuint compile_stage(GLenum type, const char* src) noexcept {
    GLuint s = gl_CreateShader(type);
    gl_ShaderSource(s, 1, &src, nullptr);
    gl_CompileShader(s);
    GLint ok = GL_FALSE;
    gl_GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[512];
        gl_GetShaderInfoLog(s, sizeof(buf), nullptr, buf);
        std::fprintf(stderr, "[FSR1Pass] Shader compile error:\n%s\n", buf);
        gl_DeleteShader(s);
        return 0u;
    }
    return s;
}

// Link a program from two shader stages. Deletes stages afterwards.
static GLuint link_program(GLuint vs, GLuint fs) noexcept {
    if (!vs || !fs) { gl_DeleteShader(vs); gl_DeleteShader(fs); return 0u; }
    GLuint p = gl_CreateProgram();
    gl_AttachShader(p, vs);
    gl_AttachShader(p, fs);
    gl_LinkProgram(p);
    gl_DeleteShader(vs);
    gl_DeleteShader(fs);
    GLint ok = GL_FALSE;
    gl_GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[512];
        gl_GetProgramInfoLog(p, sizeof(buf), nullptr, buf);
        std::fprintf(stderr, "[FSR1Pass] Program link error:\n%s\n", buf);
        gl_DeleteProgram(p);
        return 0u;
    }
    return p;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
namespace phyriad::render::opengl3 {

FSR1Pass::FSR1Pass()  noexcept = default;
FSR1Pass::~FSR1Pass() noexcept { shutdown(); }

// ── init ─────────────────────────────────────────────────────────────────────
std::expected<void, phyriad::Error>
FSR1Pass::init(const Config& cfg) noexcept
{
    if (initialized_) shutdown();

    if (cfg.input_width  == 0 || cfg.input_height  == 0 ||
        cfg.output_width == 0 || cfg.output_height == 0) {
        std::fprintf(stderr, "[FSR1Pass] init: zero dimensions\n");
        return std::unexpected(phyriad::Error{
            .code = phyriad::ErrorCode::ResourceInitFailed,
            .source_node_id = 0, .timestamp_ns = 0});
    }

    cfg_ = cfg;

    if (!load_gl_functions()) goto fail;
    if (!compile_shaders())   goto fail;
    if (!create_fbo())        goto fail;
    if (!create_quad())       goto fail;

    initialized_ = true;
    return {};

fail:
    shutdown();
    return std::unexpected(phyriad::Error{
        .code = phyriad::ErrorCode::ResourceInitFailed,
        .source_node_id = 0, .timestamp_ns = 0});
}

// ── shutdown ──────────────────────────────────────────────────────────────────
void FSR1Pass::shutdown() noexcept
{
    destroy_quad();
    destroy_fbo();
    destroy_shaders();
    initialized_ = false;
}

// ── apply ─────────────────────────────────────────────────────────────────────
void FSR1Pass::apply(uint32_t input_tex_id) noexcept
{
    if (!initialized_) [[unlikely]] return;

    // Save caller's draw framebuffer so we can restore it after EASU.
    // (RCAS renders into it directly.)
    GLint saved_fbo = 0;
    // Note: GetIntegerv is GL 1.0 — available without the loader.
    // We keep this as a raw pointer call via glfwGetProcAddress to avoid
    // pulling in more loader machinery.  In practice, ImGui already called
    // glGetIntegerv via its loader, so the pointer is resident in the driver.
    // We use the same function-pointer approach:
    typedef void (APIENTRYP PFNGLGETINTEGERVPROC)(GLenum, GLint*);
    static PFNGLGETINTEGERVPROC gl_GetIntegerv_local = nullptr;
    if (!gl_GetIntegerv_local)
        gl_GetIntegerv_local = (PFNGLGETINTEGERVPROC)(void*)
            glfwGetProcAddress("glGetIntegerv");
    if (gl_GetIntegerv_local)
        gl_GetIntegerv_local(0x8CA6u /* GL_DRAW_FRAMEBUFFER_BINDING */, &saved_fbo);

    // ── Pass 1: EASU → easu_fbo_ ─────────────────────────────────────────────
    gl_BindFramebuffer(GL_FRAMEBUFFER, easu_fbo_);
    gl_Viewport_(0, 0, static_cast<GLsizei>(cfg_.output_width),
                       static_cast<GLsizei>(cfg_.output_height));
    gl_UseProgram(easu_prog_);
    gl_ActiveTexture(GL_TEXTURE0);
    gl_BindTexture(GL_TEXTURE_2D, input_tex_id);
    gl_Uniform1i(u_easu_tex_, 0);
    upload_easu_constants();
    gl_BindVertexArray(quad_vao_);
    gl_DrawArrays(GL_TRIANGLES, 0, 6);

    // ── Pass 2: RCAS → caller's FBO ─────────────────────────────────────────
    gl_BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(saved_fbo));
    gl_UseProgram(rcas_prog_);
    gl_ActiveTexture(GL_TEXTURE0);
    gl_BindTexture(GL_TEXTURE_2D, easu_tex_);
    gl_Uniform1i(u_rcas_tex_, 0);
    upload_rcas_constants();
    gl_DrawArrays(GL_TRIANGLES, 0, 6);

    gl_BindVertexArray(0u);
    gl_UseProgram(0u);
    gl_BindTexture(GL_TEXTURE_2D, 0u);
}

// ── resize ────────────────────────────────────────────────────────────────────
void FSR1Pass::resize(uint32_t input_w, uint32_t input_h,
                      uint32_t output_w, uint32_t output_h) noexcept
{
    if (!initialized_) [[unlikely]] return;
    if (input_w == 0 || input_h == 0 || output_w == 0 || output_h == 0) return;

    cfg_.input_width   = input_w;
    cfg_.input_height  = input_h;
    cfg_.output_width  = output_w;
    cfg_.output_height = output_h;

    // Recreate intermediate EASU texture at new output dimensions.
    destroy_fbo();
    create_fbo();
}

// ─────────────────────────────────────────────────────────────────────────────
// Private helpers
// ─────────────────────────────────────────────────────────────────────────────

bool FSR1Pass::load_gl_functions() noexcept
{
    if (s_gl_loaded) return true;

#define LOAD(var, name)                                                 \
    var = (decltype(var))(void*)glfwGetProcAddress(name);              \
    if (!var) {                                                         \
        std::fprintf(stderr, "[FSR1Pass] Failed to load %s\n", name); \
        return false;                                                   \
    }

    LOAD(gl_GenFramebuffers,         "glGenFramebuffers")
    LOAD(gl_BindFramebuffer,         "glBindFramebuffer")
    LOAD(gl_FramebufferTexture2D,    "glFramebufferTexture2D")
    LOAD(gl_CheckFramebufferStatus,  "glCheckFramebufferStatus")
    LOAD(gl_DeleteFramebuffers,      "glDeleteFramebuffers")
    LOAD(gl_GenTextures,             "glGenTextures")
    LOAD(gl_BindTexture,             "glBindTexture")
    LOAD(gl_TexImage2D,              "glTexImage2D")
    LOAD(gl_TexParameteri,           "glTexParameteri")
    LOAD(gl_DeleteTextures,          "glDeleteTextures")
    LOAD(gl_ActiveTexture,           "glActiveTexture")
    LOAD(gl_CreateShader,            "glCreateShader")
    LOAD(gl_ShaderSource,            "glShaderSource")
    LOAD(gl_CompileShader,           "glCompileShader")
    LOAD(gl_GetShaderiv,             "glGetShaderiv")
    LOAD(gl_GetShaderInfoLog,        "glGetShaderInfoLog")
    LOAD(gl_DeleteShader,            "glDeleteShader")
    LOAD(gl_CreateProgram,           "glCreateProgram")
    LOAD(gl_AttachShader,            "glAttachShader")
    LOAD(gl_LinkProgram,             "glLinkProgram")
    LOAD(gl_GetProgramiv,            "glGetProgramiv")
    LOAD(gl_GetProgramInfoLog,       "glGetProgramInfoLog")
    LOAD(gl_DeleteProgram,           "glDeleteProgram")
    LOAD(gl_UseProgram,              "glUseProgram")
    LOAD(gl_GetUniformLocation,      "glGetUniformLocation")
    LOAD(gl_Uniform1i,               "glUniform1i")
    LOAD(gl_Uniform4uiv,             "glUniform4uiv")
    LOAD(gl_GenVertexArrays,         "glGenVertexArrays")
    LOAD(gl_BindVertexArray,         "glBindVertexArray")
    LOAD(gl_DeleteVertexArrays,      "glDeleteVertexArrays")
    LOAD(gl_GenBuffers,              "glGenBuffers")
    LOAD(gl_BindBuffer,              "glBindBuffer")
    LOAD(gl_BufferData,              "glBufferData")
    LOAD(gl_DeleteBuffers,           "glDeleteBuffers")
    LOAD(gl_VertexAttribPointer,     "glVertexAttribPointer")
    LOAD(gl_EnableVertexAttribArray, "glEnableVertexAttribArray")
    LOAD(gl_DrawArrays,              "glDrawArrays")
    LOAD(gl_Viewport_,               "glViewport")

#undef LOAD

    s_gl_loaded = true;
    return true;
}

bool FSR1Pass::compile_shaders() noexcept
{
    // ── EASU ─────────────────────────────────────────────────────────────────
    {
        GLuint vs = compile_stage(GL_VERTEX_SHADER,   kVertSrc);
        GLuint fs = compile_stage(GL_FRAGMENT_SHADER, kEasuFragSrc);
        easu_prog_ = link_program(vs, fs);
        if (!easu_prog_) return false;

        u_easu_tex_  = gl_GetUniformLocation(easu_prog_, "texSrc");
        u_easu_con0_ = gl_GetUniformLocation(easu_prog_, "con0");
        u_easu_con1_ = gl_GetUniformLocation(easu_prog_, "con1");
    }

    // ── RCAS ─────────────────────────────────────────────────────────────────
    {
        GLuint vs = compile_stage(GL_VERTEX_SHADER,   kVertSrc);
        GLuint fs = compile_stage(GL_FRAGMENT_SHADER, kRcasFragSrc);
        rcas_prog_ = link_program(vs, fs);
        if (!rcas_prog_) return false;

        u_rcas_tex_ = gl_GetUniformLocation(rcas_prog_, "texSrc");
        u_rcas_con_ = gl_GetUniformLocation(rcas_prog_, "con0");
    }

    return true;
}

bool FSR1Pass::create_fbo() noexcept
{
    auto w = static_cast<GLsizei>(cfg_.output_width);
    auto h = static_cast<GLsizei>(cfg_.output_height);

    // Texture.
    gl_GenTextures(1, &easu_tex_);
    gl_BindTexture(GL_TEXTURE_2D, easu_tex_);
    gl_TexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8),
                  w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR));
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     static_cast<GLint>(GL_CLAMP_TO_EDGE));
    gl_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     static_cast<GLint>(GL_CLAMP_TO_EDGE));
    gl_BindTexture(GL_TEXTURE_2D, 0u);

    // Framebuffer.
    gl_GenFramebuffers(1, &easu_fbo_);
    gl_BindFramebuffer(GL_FRAMEBUFFER, easu_fbo_);
    gl_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_TEXTURE_2D, easu_tex_, 0);

    GLenum status = gl_CheckFramebufferStatus(GL_FRAMEBUFFER);
    gl_BindFramebuffer(GL_FRAMEBUFFER, 0u);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[FSR1Pass] FBO incomplete: 0x%X\n", (unsigned)status);
        return false;
    }
    return true;
}

bool FSR1Pass::create_quad() noexcept
{
    // Two triangles covering clip space [-1,1]×[-1,1], UV [0,1]×[0,1].
    // Layout: x, y, u, v (all float32).
    static constexpr float kVerts[] = {
        // triangle 0
        -1.f, -1.f,  0.f, 0.f,
         1.f, -1.f,  1.f, 0.f,
         1.f,  1.f,  1.f, 1.f,
        // triangle 1
        -1.f, -1.f,  0.f, 0.f,
         1.f,  1.f,  1.f, 1.f,
        -1.f,  1.f,  0.f, 1.f,
    };

    gl_GenVertexArrays(1, &quad_vao_);
    gl_BindVertexArray(quad_vao_);

    gl_GenBuffers(1, &quad_vbo_);
    gl_BindBuffer(GL_ARRAY_BUFFER, quad_vbo_);
    gl_BufferData(GL_ARRAY_BUFFER,
                  static_cast<GLsizeiptr>(sizeof(kVerts)),
                  kVerts, GL_STATIC_DRAW);

    // location 0: position (vec2)
    gl_VertexAttribPointer(0u, 2, GL_FLOAT, GL_FALSE,
                           4 * static_cast<GLsizei>(sizeof(float)),
                           reinterpret_cast<const void*>(0));
    gl_EnableVertexAttribArray(0u);

    // location 1: UV (vec2)
    gl_VertexAttribPointer(1u, 2, GL_FLOAT, GL_FALSE,
                           4 * static_cast<GLsizei>(sizeof(float)),
                           reinterpret_cast<const void*>(2 * sizeof(float)));
    gl_EnableVertexAttribArray(1u);

    gl_BindVertexArray(0u);
    gl_BindBuffer(GL_ARRAY_BUFFER, 0u);
    return true;
}

void FSR1Pass::upload_easu_constants() noexcept
{
    float iw = static_cast<float>(cfg_.input_width);
    float ih = static_cast<float>(cfg_.input_height);
    float ow = static_cast<float>(cfg_.output_width);
    float oh = static_cast<float>(cfg_.output_height);

    const uint32_t con0[4] = {
        f2u(iw / ow),
        f2u(ih / oh),
        f2u(0.5f * iw / ow - 0.5f),
        f2u(0.5f * ih / oh - 0.5f),
    };
    const uint32_t con1[4] = {
        f2u(1.0f / iw),
        f2u(1.0f / ih),
        0u, 0u,
    };

    gl_Uniform4uiv(u_easu_con0_, 1, con0);
    gl_Uniform4uiv(u_easu_con1_, 1, con1);
}

void FSR1Pass::upload_rcas_constants() noexcept
{
    // AMD RCAS sharpness: amp = exp2(-sharpness), range (0,1].
    float amp = std::exp2(-cfg_.sharpness);
    const uint32_t con[4] = {
        f2u(amp),
        f2u(1.0f / static_cast<float>(cfg_.output_width)),
        f2u(1.0f / static_cast<float>(cfg_.output_height)),
        0u,
    };
    gl_Uniform4uiv(u_rcas_con_, 1, con);
}

void FSR1Pass::destroy_shaders() noexcept
{
    if (s_gl_loaded) {
        if (easu_prog_) { gl_DeleteProgram(easu_prog_); easu_prog_ = 0u; }
        if (rcas_prog_) { gl_DeleteProgram(rcas_prog_); rcas_prog_ = 0u; }
    }
}

void FSR1Pass::destroy_fbo() noexcept
{
    if (s_gl_loaded) {
        if (easu_fbo_) { gl_DeleteFramebuffers(1, &easu_fbo_); easu_fbo_ = 0u; }
        if (easu_tex_) { gl_DeleteTextures(1,     &easu_tex_); easu_tex_ = 0u; }
    }
}

void FSR1Pass::destroy_quad() noexcept
{
    if (s_gl_loaded) {
        if (quad_vao_) { gl_DeleteVertexArrays(1, &quad_vao_); quad_vao_ = 0u; }
        if (quad_vbo_) { gl_DeleteBuffers(1,      &quad_vbo_); quad_vbo_ = 0u; }
    }
}

} // namespace phyriad::render::opengl3
// Made with my soul - Swately <3
