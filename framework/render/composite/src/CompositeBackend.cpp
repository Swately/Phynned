// framework/render/composite/src/CompositeBackend.cpp
// CompositeBackend implementation.
//
// The external-memory interop (VK_KHR_external_memory_* ↔ GL_EXT_external_objects_*)
// is conditionally compiled:
//   PHYRIAD_COMPOSITE_INTEROP_AVAILABLE — set by CMake when the platform and
//   Vulkan extension headers support external-memory interop (Windows HANDLE
//   or POSIX fd path).
//
// When PHYRIAD_COMPOSITE_INTEROP_AVAILABLE=0 (default, most headless CI targets):
//   init() succeeds in "Vulkan-only" or "OpenGL-only" mode without interop.
//   interop_active() returns false.
//   The scene quad is not drawn; the Vulkan backend drives the frame.
//
#include <phyriad/render/composite/CompositeBackend.hpp>
#include <phyriad/schema/Error.hpp>
#ifdef PHYRIAD_BUILD_VULKAN
#   include <phyriad/render/vulkan/VulkanContext.hpp>
#endif
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// Minimal GL function-pointer subset for the fullscreen-quad pipeline.
// Same convention as opengl3/FSR1Pass.cpp + ExternalTexture.cpp — we do
// not link a GL loader library; instead we load via glfwGetProcAddress.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef APIENTRY
#   ifdef _WIN32
#       define APIENTRY __stdcall
#   else
#       define APIENTRY
#   endif
#endif
#define APIENTRYP APIENTRY *

typedef unsigned int  GLenum;
typedef unsigned int  GLuint;
typedef int           GLint;
typedef int           GLsizei;
typedef char          GLchar;
typedef unsigned char GLboolean;

#define GL_FALSE                                 0
#define GL_TRUE                                  1
#define GL_TRIANGLE_STRIP                        0x0005
#define GL_TEXTURE_2D                            0x0DE1
#define GL_TEXTURE0                              0x84C0
#define GL_VERTEX_SHADER                         0x8B31
#define GL_FRAGMENT_SHADER                       0x8B30
#define GL_COMPILE_STATUS                        0x8B81
#define GL_LINK_STATUS                           0x8B82
#define GL_INFO_LOG_LENGTH                       0x8B84
#define GL_NO_ERROR                              0

typedef GLenum  (APIENTRYP PFNGLGETERRORPROC)(void);
typedef GLuint  (APIENTRYP PFNGLCREATESHADERPROC)(GLenum type);
typedef void    (APIENTRYP PFNGLSHADERSOURCEPROC)(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length);
typedef void    (APIENTRYP PFNGLCOMPILESHADERPROC)(GLuint shader);
typedef void    (APIENTRYP PFNGLGETSHADERIVPROC)(GLuint shader, GLenum pname, GLint* params);
typedef void    (APIENTRYP PFNGLGETSHADERINFOLOGPROC)(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
typedef void    (APIENTRYP PFNGLDELETESHADERPROC)(GLuint shader);
typedef GLuint  (APIENTRYP PFNGLCREATEPROGRAMPROC)(void);
typedef void    (APIENTRYP PFNGLATTACHSHADERPROC)(GLuint program, GLuint shader);
typedef void    (APIENTRYP PFNGLLINKPROGRAMPROC)(GLuint program);
typedef void    (APIENTRYP PFNGLGETPROGRAMIVPROC)(GLuint program, GLenum pname, GLint* params);
typedef void    (APIENTRYP PFNGLGETPROGRAMINFOLOGPROC)(GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
typedef void    (APIENTRYP PFNGLDELETEPROGRAMPROC)(GLuint program);
typedef void    (APIENTRYP PFNGLUSEPROGRAMPROC)(GLuint program);
typedef GLint   (APIENTRYP PFNGLGETUNIFORMLOCATIONPROC)(GLuint program, const GLchar* name);
typedef void    (APIENTRYP PFNGLUNIFORM1IPROC)(GLint location, GLint v0);
typedef void    (APIENTRYP PFNGLGENVERTEXARRAYSPROC)(GLsizei n, GLuint* arrays);
typedef void    (APIENTRYP PFNGLBINDVERTEXARRAYPROC)(GLuint array);
typedef void    (APIENTRYP PFNGLDELETEVERTEXARRAYSPROC)(GLsizei n, const GLuint* arrays);
typedef void    (APIENTRYP PFNGLACTIVETEXTUREPROC)(GLenum texture);
typedef void    (APIENTRYP PFNGLBINDTEXTUREPROC)(GLenum target, GLuint texture);
typedef void    (APIENTRYP PFNGLDRAWARRAYSPROC)(GLenum mode, GLint first, GLsizei count);
typedef void    (APIENTRYP PFNGLVIEWPORTPROC)(GLint x, GLint y, GLsizei width, GLsizei height);

namespace {

PFNGLGETERRORPROC             p_glGetError             = nullptr;
PFNGLCREATESHADERPROC         p_glCreateShader         = nullptr;
PFNGLSHADERSOURCEPROC         p_glShaderSource         = nullptr;
PFNGLCOMPILESHADERPROC        p_glCompileShader        = nullptr;
PFNGLGETSHADERIVPROC          p_glGetShaderiv          = nullptr;
PFNGLGETSHADERINFOLOGPROC     p_glGetShaderInfoLog     = nullptr;
PFNGLDELETESHADERPROC         p_glDeleteShader         = nullptr;
PFNGLCREATEPROGRAMPROC        p_glCreateProgram        = nullptr;
PFNGLATTACHSHADERPROC         p_glAttachShader         = nullptr;
PFNGLLINKPROGRAMPROC          p_glLinkProgram          = nullptr;
PFNGLGETPROGRAMIVPROC         p_glGetProgramiv         = nullptr;
PFNGLGETPROGRAMINFOLOGPROC    p_glGetProgramInfoLog    = nullptr;
PFNGLDELETEPROGRAMPROC        p_glDeleteProgram        = nullptr;
PFNGLUSEPROGRAMPROC           p_glUseProgram           = nullptr;
PFNGLGETUNIFORMLOCATIONPROC   p_glGetUniformLocation   = nullptr;
PFNGLUNIFORM1IPROC            p_glUniform1i            = nullptr;
PFNGLGENVERTEXARRAYSPROC      p_glGenVertexArrays      = nullptr;
PFNGLBINDVERTEXARRAYPROC      p_glBindVertexArray      = nullptr;
PFNGLDELETEVERTEXARRAYSPROC   p_glDeleteVertexArrays   = nullptr;
PFNGLACTIVETEXTUREPROC        p_glActiveTexture        = nullptr;
PFNGLBINDTEXTUREPROC          p_glBindTexture          = nullptr;
PFNGLDRAWARRAYSPROC           p_glDrawArrays           = nullptr;
PFNGLVIEWPORTPROC             p_glViewport             = nullptr;
bool g_gl_quad_loaded = false;

template <typename Fn>
[[nodiscard]] bool load_proc(Fn& out, const char* name) noexcept {
    out = reinterpret_cast<Fn>(glfwGetProcAddress(name));
    if (!out) {
        std::fprintf(stderr, "[CompositeBackend] missing GL entry: %s\n", name);
        return false;
    }
    return true;
}

[[nodiscard]] bool load_gl_quad_procs() noexcept {
    if (g_gl_quad_loaded) return true;
    bool ok = true;
    ok &= load_proc(p_glGetError,             "glGetError");
    ok &= load_proc(p_glCreateShader,         "glCreateShader");
    ok &= load_proc(p_glShaderSource,         "glShaderSource");
    ok &= load_proc(p_glCompileShader,        "glCompileShader");
    ok &= load_proc(p_glGetShaderiv,          "glGetShaderiv");
    ok &= load_proc(p_glGetShaderInfoLog,     "glGetShaderInfoLog");
    ok &= load_proc(p_glDeleteShader,         "glDeleteShader");
    ok &= load_proc(p_glCreateProgram,        "glCreateProgram");
    ok &= load_proc(p_glAttachShader,         "glAttachShader");
    ok &= load_proc(p_glLinkProgram,          "glLinkProgram");
    ok &= load_proc(p_glGetProgramiv,         "glGetProgramiv");
    ok &= load_proc(p_glGetProgramInfoLog,    "glGetProgramInfoLog");
    ok &= load_proc(p_glDeleteProgram,        "glDeleteProgram");
    ok &= load_proc(p_glUseProgram,           "glUseProgram");
    ok &= load_proc(p_glGetUniformLocation,   "glGetUniformLocation");
    ok &= load_proc(p_glUniform1i,            "glUniform1i");
    ok &= load_proc(p_glGenVertexArrays,      "glGenVertexArrays");
    ok &= load_proc(p_glBindVertexArray,      "glBindVertexArray");
    ok &= load_proc(p_glDeleteVertexArrays,   "glDeleteVertexArrays");
    ok &= load_proc(p_glActiveTexture,        "glActiveTexture");
    ok &= load_proc(p_glBindTexture,          "glBindTexture");
    ok &= load_proc(p_glDrawArrays,           "glDrawArrays");
    ok &= load_proc(p_glViewport,             "glViewport");
    g_gl_quad_loaded = ok;
    return ok;
}

// Vertex shader: generate a triangle strip covering [-1,1]² from gl_VertexID
// (no VBO needed — the strip's 4 vertices are derived from the integer ID
// via the standard bit-trick).
constexpr const char* kQuadVertSrc = R"(
#version 330 core
out vec2 v_uv;
void main() {
    vec2 pos = vec2(float((gl_VertexID & 1) * 2 - 1),
                    float((gl_VertexID & 2) - 1));
    v_uv = pos * 0.5 + 0.5;
    // Flip Y so the Vulkan-rendered image (top-left origin) shows the
    // right way up in the GL window (bottom-left origin).
    v_uv.y = 1.0 - v_uv.y;
    gl_Position = vec4(pos, 0.0, 1.0);
}
)";

constexpr const char* kQuadFragSrc = R"(
#version 330 core
in  vec2 v_uv;
out vec4 frag_color;
uniform sampler2D u_scene;
void main() {
    frag_color = texture(u_scene, v_uv);
}
)";

[[nodiscard]] GLuint compile_shader(GLenum stage, const char* src) noexcept {
    const GLuint sh = p_glCreateShader(stage);
    p_glShaderSource(sh, 1, &src, nullptr);
    p_glCompileShader(sh);
    GLint ok = 0;
    p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        p_glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        char log[1024]{};
        const GLsizei n = (len < 1023) ? len : 1023;
        p_glGetShaderInfoLog(sh, n, nullptr, log);
        std::fprintf(stderr, "[CompositeBackend] shader compile failed: %s\n", log);
        p_glDeleteShader(sh);
        return 0u;
    }
    return sh;
}

[[nodiscard]] GLuint link_quad_program() noexcept {
    const GLuint vs = compile_shader(GL_VERTEX_SHADER,   kQuadVertSrc);
    if (!vs) return 0u;
    const GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kQuadFragSrc);
    if (!fs) { p_glDeleteShader(vs); return 0u; }
    const GLuint pr = p_glCreateProgram();
    p_glAttachShader(pr, vs);
    p_glAttachShader(pr, fs);
    p_glLinkProgram(pr);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    GLint ok = 0;
    p_glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        p_glGetProgramiv(pr, GL_INFO_LOG_LENGTH, &len);
        char log[1024]{};
        const GLsizei n = (len < 1023) ? len : 1023;
        p_glGetProgramInfoLog(pr, n, nullptr, log);
        std::fprintf(stderr, "[CompositeBackend] program link failed: %s\n", log);
        p_glDeleteProgram(pr);
        return 0u;
    }
    return pr;
}

} // anonymous

// ─────────────────────────────────────────────────────────────────────────────
// Platform interop detection
// ─────────────────────────────────────────────────────────────────────────────
#ifndef PHYRIAD_COMPOSITE_INTEROP_AVAILABLE
#   define PHYRIAD_COMPOSITE_INTEROP_AVAILABLE 0
#endif

#if PHYRIAD_COMPOSITE_INTEROP_AVAILABLE && defined(PHYRIAD_BUILD_VULKAN)
// Full interop path: platform-specific external-memory extension calls.
// Vulkan side: VK_KHR_external_memory + platform variant
// GL    side:  GL_EXT_memory_object    + platform variant
#   ifdef _WIN32
#       ifndef WIN32_LEAN_AND_MEAN
#           define WIN32_LEAN_AND_MEAN
#       endif
#       ifndef NOMINMAX
#           define NOMINMAX
#       endif
#       include <windows.h>                // HANDLE, CloseHandle
#       include <vulkan/vulkan_win32.h>    // VK_KHR_external_memory_win32
#   else
#       include <unistd.h>                 // close(fd)
#       include <vulkan/vulkan.h>          // VK_KHR_external_memory_fd
#   endif
#endif

namespace phyriad::render::composite {

// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────
CompositeBackend::CompositeBackend() noexcept = default;

CompositeBackend::~CompositeBackend() noexcept
{
    shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
// init
// ─────────────────────────────────────────────────────────────────────────────
std::expected<void, phyriad::Error>
CompositeBackend::init(GLFWwindow* window, FrameArena* arena) noexcept
{
    if (!window) {
        return std::unexpected(phyriad::Error{phyriad::ErrorCode::InvalidArgument});
    }

    window_ = window;
    arena_  = arena;

#ifdef PHYRIAD_BUILD_VULKAN
    // ── Vulkan scene backend ──────────────────────────────────────────────────
    {
        auto r = scene_.init(window, arena);
        if (r.has_value()) {
            scene_initialized_ = true;
        } else {
            // Vulkan unavailable — degrade to OpenGL-only composite.
            std::fprintf(stderr,
                "[CompositeBackend] Vulkan scene backend init failed (code=%u)"
                "  — falling back to OpenGL-only mode.\n",
                static_cast<unsigned>(r.error().code));
        }
    }
#endif

    // ── OpenGL UI backend ─────────────────────────────────────────────────────
    // OpenGL backend requires the GL context to be current. When Vulkan is
    // active, we do NOT make a GL context current (GLFW_NO_API hint was set).
    // In that case, ui_ is left uninitialised and the interop path handles
    // compositing via external memory.
    if (!scene_initialized_) {
        // Pure OpenGL path (no Vulkan or Vulkan failed).
        auto r = ui_.init(window, arena);
        if (!r.has_value()) {
            return std::unexpected(phyriad::Error{phyriad::ErrorCode::ResourceInitFailed});
        }
        ui_initialized_ = true;
    }
    // Note: when both backends are active the interop setup happens lazily on
    // the first new_frame() call once window dimensions are known.

    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// new_frame
// ─────────────────────────────────────────────────────────────────────────────
void CompositeBackend::new_frame() noexcept
{
#ifdef PHYRIAD_BUILD_VULKAN
    if (scene_initialized_) {
        // Attempt interop setup on the first real frame (window size is valid).
        if (!interop_active_ && !shared_frame_.valid) {
            // Query current framebuffer size.
            int w = 0, h = 0;
            // glfwGetFramebufferSize would be called here; placeholder below.
            // In a full implementation: glfwGetFramebufferSize(window_, &w, &h);
            if (w > 0 && h > 0) {
                interop_active_ = setup_interop(
                    static_cast<uint32_t>(w),
                    static_cast<uint32_t>(h));
            }
        }
        scene_.new_frame();
        return;
    }
#endif
    if (ui_initialized_) {
        ui_.new_frame();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// end_frame
// ─────────────────────────────────────────────────────────────────────────────
RenderStats CompositeBackend::end_frame() noexcept
{
#ifdef PHYRIAD_BUILD_VULKAN
    if (scene_initialized_) {
        RenderStats stats = scene_.end_frame();

        if (interop_active_ && ui_initialized_) {
            // Composite path: draw the VK scene as a fullscreen quad,
            // then render the ImGui overlay on top.
            ui_.new_frame();     // ImGui::NewFrame() in GL context
            draw_scene_quad();   // blit VK texture → screen
            RenderStats ui_stats = ui_.end_frame();
            // Merge: take GPU timing from Vulkan, present timing from OpenGL.
            stats.present_tsc   = ui_stats.present_tsc;
            stats.frame_time_ms = ui_stats.frame_time_ms;
        }
        return stats;
    }
#endif
    if (ui_initialized_) {
        return ui_.end_frame();
    }
    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// present
// ─────────────────────────────────────────────────────────────────────────────
void CompositeBackend::present() noexcept
{
    // When interop is active, present via the OpenGL context (glfwSwapBuffers).
    // When Vulkan-only, present via the Vulkan swapchain.
    if (interop_active_ && ui_initialized_) {
        ui_.present();
        return;
    }
#ifdef PHYRIAD_BUILD_VULKAN
    if (scene_initialized_) {
        scene_.present();
        return;
    }
#endif
    if (ui_initialized_) {
        ui_.present();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// resize
// ─────────────────────────────────────────────────────────────────────────────
void CompositeBackend::resize(uint32_t w, uint32_t h, float dpi) noexcept
{
    if (interop_active_) {
        teardown_interop();
        interop_active_ = setup_interop(w, h);
    }
#ifdef PHYRIAD_BUILD_VULKAN
    if (scene_initialized_) scene_.resize(w, h, dpi);
#endif
    if (ui_initialized_)   ui_.resize(w, h, dpi);
}

// ─────────────────────────────────────────────────────────────────────────────
// shutdown
// ─────────────────────────────────────────────────────────────────────────────
void CompositeBackend::shutdown() noexcept
{
    if (shutdown_done_) return;
    shutdown_done_ = true;

    teardown_interop();

#ifdef PHYRIAD_BUILD_VULKAN
    if (scene_initialized_) {
        scene_.shutdown();
        scene_initialized_ = false;
    }
#endif
    if (ui_initialized_) {
        ui_.shutdown();
        ui_initialized_ = false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// setup_interop (internal)
// ─────────────────────────────────────────────────────────────────────────────
bool CompositeBackend::setup_interop(uint32_t width, uint32_t height) noexcept
{
#if PHYRIAD_COMPOSITE_INTEROP_AVAILABLE && defined(PHYRIAD_BUILD_VULKAN)
    // ── Verify that the Vulkan device exposes the external-memory extension.
    //    VulkanContext detects this opportunistically during create_device.
    auto* ctx = scene_.context();
    if (!ctx || !ctx->external_memory_supported()) {
        std::fprintf(stderr,
            "[CompositeBackend] VK external memory not available — "
            "falling back to Vulkan-only mode.\n");
        return false;
    }

    // ── Step 1: Allocate the Vulkan-side ExternalImage.
    vulkan::ExternalImage::Config cfg{};
    cfg.width  = width;
    cfg.height = height;
    cfg.format = VK_FORMAT_R8G8B8A8_UNORM;     // matches GL_RGBA8
    if (!ext_image_.init(ctx->physical_device(), ctx->device(), cfg)) {
        std::fprintf(stderr,
            "[CompositeBackend] ExternalImage::init failed (%ux%u) — "
            "interop disabled.\n", width, height);
        return false;
    }

    // ── Step 2: Export the OS handle.
#ifdef _WIN32
    void* os_handle = nullptr;
    if (!ext_image_.export_handle_win32(ctx->device(), &os_handle) ||
        os_handle == nullptr)
    {
        std::fprintf(stderr,
            "[CompositeBackend] export_handle_win32 failed — interop disabled.\n");
        ext_image_.shutdown(ctx->device());
        return false;
    }
    shared_frame_.win32_handle = os_handle;
#else
    int os_handle = -1;
    if (!ext_image_.export_handle_fd(ctx->device(), &os_handle) ||
        os_handle < 0)
    {
        std::fprintf(stderr,
            "[CompositeBackend] export_handle_fd failed — interop disabled.\n");
        ext_image_.shutdown(ctx->device());
        return false;
    }
    shared_frame_.fd = os_handle;
#endif

    // ── Step 3: Import the handle into a GL texture.
    // The GL context MUST be current here. CompositeBackend's init() makes
    // sure the GL context (created with the window) is current before
    // calling setup_interop on the first new_frame.
    constexpr uint32_t kGlRGBA8 = 0x8058u;    // GL_RGBA8
    bool gl_ok = false;
#ifdef _WIN32
    gl_ok = ext_texture_.init(os_handle,
                              static_cast<uint64_t>(ext_image_.memory_size()),
                              width, height, kGlRGBA8);
#else
    gl_ok = ext_texture_.init(os_handle,
                              static_cast<uint64_t>(ext_image_.memory_size()),
                              width, height, kGlRGBA8);
#endif
    if (!gl_ok) {
        std::fprintf(stderr,
            "[CompositeBackend] ExternalTexture::init failed — interop disabled. "
            "GL_EXT_memory_object{,_win32/_fd} may be missing.\n");
        // OS handle ownership: ExternalTexture takes it on init success.
        // On init failure we lose the chance to close it through the GL path,
        // so close it manually here to avoid a leak.
#ifdef _WIN32
        if (os_handle) ::CloseHandle(static_cast<HANDLE>(os_handle));
#else
        if (os_handle >= 0) ::close(os_handle);
#endif
        ext_image_.shutdown(ctx->device());
        return false;
    }

    // ── Step 4: Record the shared-frame metadata for downstream use.
    shared_frame_.gl_texture = ext_texture_.texture();
    shared_frame_.width      = width;
    shared_frame_.height     = height;
    shared_frame_.valid      = true;

    // ── Step 5: Compile the fullscreen-quad GL program + empty VAO.
    // GL_VERTEX_SHADER/FRAGMENT_SHADER procs are loaded here on the
    // first interop setup (idempotent across resizes).
    if (!load_gl_quad_procs()) {
        std::fprintf(stderr,
            "[CompositeBackend] failed to load GL entry points for quad pipeline\n");
        ext_texture_.shutdown();
        ext_image_.shutdown(ctx->device());
        return false;
    }
    quad_prog_ = link_quad_program();
    if (quad_prog_ == 0u) {
        ext_texture_.shutdown();
        ext_image_.shutdown(ctx->device());
        return false;
    }
    // GL 3.3 core requires a bound VAO even for "vertex-pulling" shaders
    // that don't read any attributes. We bind an empty one before each
    // draw call.
    p_glGenVertexArrays(1, &quad_vao_);

    std::fprintf(stderr,
        "[CompositeBackend] interop wired (%ux%u): "
        "ExternalImage + ExternalTexture + quad program ready.\n",
        width, height);
    return true;
#else
    (void)width; (void)height;
    return false;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// teardown_interop (internal)
// ─────────────────────────────────────────────────────────────────────────────
void CompositeBackend::teardown_interop() noexcept
{
    if (!interop_active_) return;

    // ── GL side: destroy the imported texture + memory object. Requires
    //    the GL context to be current. The caller (shutdown / resize)
    //    is responsible for that — CompositeBackend doesn't manage GL
    //    context current-ness internally beyond what GLFW provides.
    ext_texture_.shutdown();
    // The OS handle (HANDLE / fd) was taken by ExternalTexture::init()
    // on success and freed by ext_texture_.shutdown() above. Nothing
    // to close manually here.

    // ── VK side: free the underlying image + memory.
#ifdef PHYRIAD_BUILD_VULKAN
    if (auto* ctx = scene_.context()) {
        ext_image_.shutdown(ctx->device());
    }
#endif

    shared_frame_ = {};

    // Tear down the GL quad program + VAO. Requires a current GL context
    // — the caller (shutdown / resize) is responsible for that.
    if (g_gl_quad_loaded) {
        if (quad_prog_ != 0u && p_glDeleteProgram)      p_glDeleteProgram(quad_prog_);
        if (quad_vao_  != 0u && p_glDeleteVertexArrays) p_glDeleteVertexArrays(1, &quad_vao_);
    }
    quad_vao_  = 0u;
    quad_vbo_  = 0u;
    quad_prog_ = 0u;

#if PHYRIAD_COMPOSITE_INTEROP_AVAILABLE
#   ifdef _WIN32
    if (shared_frame_.win32_handle) {
        shared_frame_.win32_handle = nullptr;
    }
#   else
    if (shared_frame_.fd >= 0) {
        shared_frame_.fd = -1;
    }
#   endif
#endif
    interop_active_ = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// draw_scene_quad (internal)
// ─────────────────────────────────────────────────────────────────────────────
void CompositeBackend::draw_scene_quad() noexcept
{
    if (!quad_prog_ || !shared_frame_.gl_texture) return;
    if (!g_gl_quad_loaded || !p_glUseProgram) return;

    p_glUseProgram(quad_prog_);
    p_glBindVertexArray(quad_vao_);
    p_glActiveTexture(GL_TEXTURE0);
    p_glBindTexture(GL_TEXTURE_2D, shared_frame_.gl_texture);
    // u_scene is bound to texture unit 0. We set it once per draw because
    // the program may be re-linked across resize; cost is one indirect
    // call + uniform write, negligible.
    const GLint loc = p_glGetUniformLocation(quad_prog_, "u_scene");
    if (loc >= 0) p_glUniform1i(loc, 0);
    p_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Unbind for tidiness — the next consumer (ImGui) will rebind its
    // own program + VAO, but explicit teardown avoids state leaks if a
    // future caller forgets to.
    p_glBindVertexArray(0u);
    p_glUseProgram(0u);
}

} // namespace phyriad::render::composite
// Made with my soul - Swately <3
