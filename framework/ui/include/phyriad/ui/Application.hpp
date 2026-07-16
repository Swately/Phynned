// framework/ui/include/phyriad/ui/Application.hpp
// High-level phyriad::ui application entry point.
//
// Application::run() orchestrates the full GLFW + ImGui + render-backend
// init / graph-build / main-loop / shutdown sequence for a phyriad::ui app.
//
// Design:
//   The caller supplies a BuildFn that receives a NodeRegistry, a
//   DslGraphBuilder, the IRenderBackend, and a FrameArena.  It registers
//   nodes and wires, then returns.  Application::run() finalises the graph,
//   creates the GraphRuntime, and runs the tick loop on the calling thread.
//
//   UIThreadNode (ThreadRole::UI_MAIN) is pre-registered under the name "ui"
//   and is always node 0 in topological order — it calls glfwPollEvents()
//   on every iteration of GraphRuntime::run(), which is the correct GLFW
//   threading model (event loop on the window-creating thread).
//
// Profile mapping (phyriad::ProfileKind → phyriad::runtime::PerformanceProfile):
//   BALANCED — default PerformanceProfile{} (OS-driven, zero overhead).
//   LATENCY  — make_auto_profile() with live HardwareTopology probe.
//   POWER    — default profile with use_busy_wait_loop = false.
//
// Backend selection (ApplicationConfig::render_backend):
//   OpenGL3   — OpenGL 3.3 Core + ImGui (always available).
//   Vulkan    — Vulkan 1.2 + ImGui (requires PHYRIAD_BUILD_VULKAN cmake option).
//   Auto      — Vulkan if glfwVulkanSupported() and compiled in, else OpenGL3.
//   Composite — not yet wired here.
//
// Usage:
//   phyriad::ui::ApplicationConfig cfg;
//   cfg.profile        = phyriad::ProfileKind::LATENCY;
//   cfg.render_backend = phyriad::render::RenderBackendKind::Auto;
//   return phyriad::ui::Application::run(cfg, [](auto& reg, auto& builder,
//                                            auto& backend, auto& arena) {
//       // register nodes, wire them up
//   });
//
#pragma once
#include <phyriad/ui/ApplicationConfig.hpp>
#include <phyriad/ui/UIThreadNode.hpp>
#include <phyriad/ui/text/FontAtlas.hpp>
#include <phyriad/render/IRenderBackend.hpp>
#include <phyriad/render/opengl3/OpenGL3Backend.hpp>
#ifdef PHYRIAD_BUILD_VULKAN
#   include <phyriad/render/vulkan/VulkanBackend.hpp>
#endif
#include <phyriad/render/FrameArena.hpp>
#include <phyriad/api/NodeRegistry.hpp>
#include <phyriad/api/NodeHandle.hpp>
#include <phyriad/api/GraphDSL.hpp>
#include <phyriad/api/placement.hpp>
#include <phyriad/runtime/GraphRuntime.hpp>
#include <phyriad/runtime/PerformanceProfile.hpp>
#include <phyriad/topology/HardwareTopology.hpp>
#include <phyriad/schema/Error.hpp>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <memory>
#include <cstddef>
#include <cstdio>

// Native dialog for GPU driver errors. We want a visible, modal warning
// when ayama-ui.exe is double-clicked from Explorer and there is no
// working GPU driver — stderr is invisible in that path. MessageBoxW +
// ShellExecuteW from the standard Windows API are sufficient; no
// comctl32 / WinRT / Toast dependency.
#ifdef _WIN32
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>
#   include <shellapi.h>
#endif

namespace phyriad::ui {

#ifdef _WIN32
namespace detail {
    // Show a native Windows error dialog when no OpenGL context can be
    // created — typical on machines where the GPU driver is missing or
    // Windows fell back to the Basic Display Adapter. Three buttons:
    //   Yes    → opens NVIDIA driver download page
    //   No     → opens Intel driver download page
    //   Cancel → close the dialog (caller exits app)
    // AMD users see the AMD URL in the dialog body and copy it manually
    // (we only have three buttons; NVIDIA + Intel cover ~90% of Windows
    // laptops with this failure mode).
    inline void show_gpu_driver_required_dialog(const char* app_name) noexcept {
        // Build a friendly title from the app's window title when present.
        wchar_t title_buf[128] = L"Phyriad — Graphics driver required";
        if (app_name != nullptr && app_name[0] != '\0') {
            // Best-effort ASCII → wide (titles are short, ASCII-only).
            int n = 0;
            for (int i = 0; app_name[i] != '\0' && n < 96; ++i) {
                title_buf[n++] = static_cast<wchar_t>(
                    static_cast<unsigned char>(app_name[i]));
            }
            // Suffix " — Graphics driver required" (plain ASCII for portability).
            const wchar_t* suffix = L" - Graphics driver required";
            for (int i = 0; suffix[i] != L'\0' && n < 127; ++i) {
                title_buf[n++] = suffix[i];
            }
            title_buf[n] = L'\0';
        }

        const wchar_t* body =
            L"Could not initialize the GPU. No working OpenGL driver was "
            L"found on this system.\n\n"
            L"Likely cause: your graphics driver is missing or out of date. "
            L"Windows may be falling back to the \"Microsoft Basic Display "
            L"Adapter\" which has no OpenGL support.\n\n"
            L"To fix this, install up-to-date drivers from your GPU "
            L"manufacturer's website:\n\n"
            L"   [Yes]      Open NVIDIA driver download page\n"
            L"   [No]       Open Intel driver download page\n"
            L"   [Cancel]   Close (no browser opened)\n\n"
            L"AMD users: visit https://www.amd.com/en/support\n\n"
            L"After installing drivers, restart your PC and try again.";

        const int choice = MessageBoxW(
            nullptr, body, title_buf,
            MB_ICONERROR | MB_YESNOCANCEL | MB_TOPMOST | MB_SETFOREGROUND);

        const wchar_t* url = nullptr;
        if (choice == IDYES) {
            url = L"https://www.nvidia.com/drivers/";
        } else if (choice == IDNO) {
            url = L"https://www.intel.com/content/www/us/en/download-center/home.html";
        }
        if (url != nullptr) {
            ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
} // namespace detail
#endif // _WIN32

// ── Application ───────────────────────────────────────────────────────────────
class Application {
public:
    // BuildFn signature:
    //   void build_fn(api::NodeRegistry&,
    //                 api::DslGraphBuilder&,
    //                 render::IRenderBackend&,
    //                 render::FrameArena&)
    template <typename BuildFn>
    [[nodiscard]] static int run(ApplicationConfig const& cfg,
                                 BuildFn&&               build_fn) noexcept
    {
        // ── 1. GLFW init ──────────────────────────────────────────────────────
        if (!glfwInit()) {
            std::fprintf(stderr, "[Application] glfwInit() failed\n");
            return 1;
        }

        // ── 2. Resolve backend kind ───────────────────────────────────────────
        // Auto-select: prefer Vulkan when the driver supports it and the
        // Vulkan backend was compiled in; fall back to OpenGL3 otherwise.
        auto backend_kind = cfg.render_backend;
        if (backend_kind == render::RenderBackendKind::Auto) {
#ifdef PHYRIAD_BUILD_VULKAN
            backend_kind = glfwVulkanSupported()
                ? render::RenderBackendKind::Vulkan
                : render::RenderBackendKind::OpenGL3;
#else
            backend_kind = render::RenderBackendKind::OpenGL3;
#endif
        }

        // ── 3+4. Window hints + create window (per-backend) ──────────────────
        // Vulkan: GLFW_NO_API client, single attempt — version doesn't apply.
        // OpenGL: 3.3 Core is preferred; if that fails (outdated driver,
        //   Windows Basic Display Adapter, hybrid laptop with iGPU driver
        //   stuck), try progressively older Core profiles before giving up.
        //   3.2 Core is the floor for the ImGui OpenGL3 backend.
        GLFWwindow* window = nullptr;

        if (backend_kind == render::RenderBackendKind::Vulkan) {
            glfwDefaultWindowHints();
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            if (!cfg.window.resizable) glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
            window = glfwCreateWindow(
                static_cast<int>(cfg.window.width),
                static_cast<int>(cfg.window.height),
                cfg.window.title,
                nullptr, nullptr);
            if (!window) {
                const char* err_desc = nullptr;
                glfwGetError(&err_desc);
                std::fprintf(stderr,
                    "[Application] glfwCreateWindow() failed (Vulkan): %s\n",
                    err_desc ? err_desc : "(no error string)");
                glfwTerminate();
                return 2;
            }
        } else {
            // OpenGL path — try each version in turn until one creates a
            // valid window. The first success wins. We log each failure so
            // the user can see what their driver actually offers.
            struct GlAttempt {
                int         major;
                int         minor;
                const char* label;
            };
            constexpr GlAttempt kAttempts[] = {
                {3, 3, "3.3 Core"},  // preferred — modern ImGui backend
                {3, 2, "3.2 Core"},  // minimum for ImGui OpenGL3 backend
                {3, 1, "3.1 Core"},  // last-resort core profile
            };

            for (const auto& a : kAttempts) {
                // Reset all hints to defaults between attempts — leftover
                // hints from a failed attempt can poison the next try.
                glfwDefaultWindowHints();
                glfwWindowHint(GLFW_CLIENT_API,           GLFW_OPENGL_API);
                glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, a.major);
                glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, a.minor);
                glfwWindowHint(GLFW_OPENGL_PROFILE,        GLFW_OPENGL_CORE_PROFILE);
                if (!cfg.window.resizable) glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

                window = glfwCreateWindow(
                    static_cast<int>(cfg.window.width),
                    static_cast<int>(cfg.window.height),
                    cfg.window.title,
                    nullptr, nullptr);
                if (window) {
                    std::fprintf(stderr,
                        "[Application] OpenGL %s context created\n", a.label);
                    break;
                }
                const char* err_desc = nullptr;
                glfwGetError(&err_desc);
                std::fprintf(stderr,
                    "[Application] OpenGL %s unavailable: %s\n",
                    a.label, err_desc ? err_desc : "(no error string)");
            }

            if (!window) {
                // Console diagnostic — visible to users running from a
                // command line. Same text as before for log-scrapers.
                std::fprintf(stderr,
                    "[Application] All OpenGL context attempts failed.\n"
                    "[Application] Likely causes:\n"
                    "  1. GPU driver not installed (Windows running on Basic\n"
                    "     Display Adapter — OpenGL 1.1 only).\n"
                    "  2. Hybrid laptop using a stale iGPU driver. ayama-ui.exe\n"
                    "     already exports NvOptimusEnablement; check that the\n"
                    "     discrete GPU appears under Settings -> Display ->\n"
                    "     Graphics for this app.\n"
                    "  3. Headless / Remote Desktop session with software-only\n"
                    "     OpenGL (Microsoft GDI Renderer caps at 1.1).\n"
                    "Update GPU drivers and try again.\n");
#ifdef _WIN32
                // Native modal dialog — visible to users who double-click
                // the .exe from Explorer (stderr is hidden in that path).
                // Offers one-click access to NVIDIA / Intel driver pages.
                detail::show_gpu_driver_required_dialog(cfg.window.title);
#endif
                glfwTerminate();
                return 2;
            }
        }

        // ── 5. GL context current (OpenGL only) ───────────────────────────────
        // For Vulkan the context is managed by the VulkanBackend (VkSurface /
        // VkSwapchain); calling glfwMakeContextCurrent would be a no-op or
        // an error with GLFW_NO_API windows.
        if (backend_kind == render::RenderBackendKind::OpenGL3) {
            glfwMakeContextCurrent(window);
        }

        // ── 6. Frame arena ────────────────────────────────────────────────────
        render::FrameArena arena(cfg.frame_arena_capacity);

        // ── 7. ImGui context ──────────────────────────────────────────────────
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();

        // Build the font atlas with the phyriad default glyph ranges:
        // Latin-1 Supplement (á é í ó ú ñ ü ¿ ¡) and General Punctuation
        // (— … " "). Without this, ImGui's lazy-built default atlas only
        // covers Basic Latin and non-ASCII characters render as tofu.
        text::install_default_font(ImGui::GetIO(), /*font_global_scale=*/1.0f);

        // ── 8. UIThreadNode — BEFORE backend.init() ───────────────────────────
        // Constructor registers raw GLFW callbacks. The backend's init()
        // (ImGui_ImplGlfw_InitForOpenGL or ImGui_ImplGlfw_InitForVulkan) then
        // wraps them in chaining callbacks, preserving our handlers.
        // Never re-register after that point.
        UIThreadNode ui_node(window);

        // ── 9. Render backend — dynamic selection ─────────────────────────────
        std::unique_ptr<render::IRenderBackend> backend_ptr;

        if (backend_kind == render::RenderBackendKind::Vulkan) {
#ifdef PHYRIAD_BUILD_VULKAN
            backend_ptr = std::make_unique<render::vulkan::VulkanBackend>();
#else
            std::fprintf(stderr,
                "[Application] Vulkan backend requested but "
                "PHYRIAD_BUILD_VULKAN is OFF — recompile with -DPHYRIAD_BUILD_VULKAN=ON\n");
            ImGui::DestroyContext();
            glfwDestroyWindow(window);
            glfwTerminate();
            return 3;
#endif
        } else {
            // OpenGL3 (and any future fallback from Auto)
            backend_ptr = std::make_unique<render::opengl3::OpenGL3Backend>();
        }

        auto init_res = backend_ptr->init(window, &arena);
        if (!init_res.has_value()) {
            std::fprintf(stderr, "[Application] render backend init() failed\n");
            ImGui::DestroyContext();
            glfwDestroyWindow(window);
            glfwTerminate();
            return 3;
        }

        // ── 10. Graph builder — pre-add "ui" with UI_MAIN placement ──────────
        api::DslGraphBuilder builder;
        builder.node<UIThreadNode>("ui", api::placement::ui_main());

        // ── 11. Node registry — pre-register "ui" via wrap ───────────────────
        // UIThreadNode is non-default-constructible (takes GLFWwindow*), so we
        // retain ownership and hand a non-owning NodeHandle to the runtime.
        api::NodeRegistry reg;
        reg.register_factory(
            "ui",
            [&ui_node](NodeId id) noexcept -> api::NodeHandle {
                return api::NodeHandle::wrap(&ui_node, id);
            },
            schema::schema_hash<InputEvent>(),
            schema::Hash128{});

        // Register wire types needed by UIThreadNode outlets.
        reg.wire_registry().register_type<InputEvent>();
        reg.wire_registry().register_type<WindowState>();

        // ── 12. User build callback ───────────────────────────────────────────
        build_fn(reg, builder, *backend_ptr, arena);

        // ── 13. Build graph ───────────────────────────────────────────────────
        auto graph_res = std::move(builder).build();
        if (!graph_res.has_value()) {
            std::fprintf(stderr, "[Application] graph build failed\n");
            backend_ptr->shutdown();
            ImGui::DestroyContext();
            glfwDestroyWindow(window);
            glfwTerminate();
            return 4;
        }
        auto const& graph = *graph_res;

        // ── 14. Performance profile ───────────────────────────────────────────
        runtime::PerformanceProfile profile = make_profile(cfg.profile, graph);

        // ── 15. Create runtime ────────────────────────────────────────────────
        auto rt_res = runtime::GraphRuntime::create(
            graph, reg, reg.wire_registry(), profile);
        if (!rt_res.has_value()) {
            std::fprintf(stderr,
                "[Application] GraphRuntime::create() failed\n");
            backend_ptr->shutdown();
            ImGui::DestroyContext();
            glfwDestroyWindow(window);
            glfwTerminate();
            return 5;
        }
        auto& rt = *rt_res;

        // ── 16. Start nodes ───────────────────────────────────────────────────
        auto start_res = rt.start();
        if (!start_res.has_value()) {
            std::fprintf(stderr, "[Application] GraphRuntime::start() failed\n");
            backend_ptr->shutdown();
            ImGui::DestroyContext();
            glfwDestroyWindow(window);
            glfwTerminate();
            return 5;
        }

        // ── 17. Main loop ─────────────────────────────────────────────────────
        // run() ticks all nodes (including UIThreadNode → glfwPollEvents) on
        // this thread in topological order.  UIThreadNode returns WindowClosed
        // when glfwWindowShouldClose() is set, which stops the loop.
        rt.run();

        // ── 18. Shutdown ──────────────────────────────────────────────────────
        (void)rt.shutdown();
        backend_ptr->shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();

        return 0;
    }

private:
    // ── Profile translation ───────────────────────────────────────────────────
    [[nodiscard]] static runtime::PerformanceProfile
    make_profile(phyriad::ProfileKind kind,
                 api::BuiltGraph const& graph) noexcept
    {
        const auto n_nodes = static_cast<uint32_t>(graph.nodes.size());
        const auto n_wires = static_cast<uint32_t>(graph.wires.size());

        switch (kind) {
        case phyriad::ProfileKind::LATENCY: {
            // Probe hardware and build an auto-tuned profile.
            auto topo = phyriad::HardwareTopology::probe()
                            .value_or(phyriad::HardwareTopology{});
            return runtime::make_auto_profile(topo, graph.hints, n_nodes, n_wires);
        }
        case phyriad::ProfileKind::POWER: {
            // Default profile, disable busy-wait to save power.
            runtime::PerformanceProfile p{};
            p.node_profiles.resize(n_nodes);
            p.ring_profiles.resize(n_wires);
            p.use_busy_wait_loop = false;
            return p;
        }
        case phyriad::ProfileKind::BALANCED:
        default: {
            // Safe default — OS manages thread scheduling.
            runtime::PerformanceProfile p{};
            p.node_profiles.resize(n_nodes);
            p.ring_profiles.resize(n_wires);
            return p;
        }
        }
    }
};

} // namespace phyriad::ui
// Made with my soul - Swately <3
