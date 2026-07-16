// framework/render/vulkan/src/VulkanContext.cpp
// VulkanContext — implementación de init/shutdown.
//
// Flujo de init:
//   1. Verificar soporte Vulkan en GLFW
//   2. Crear VkInstance con extensiones GLFW + debug_utils (debug builds)
//   3. Setup debug messenger (debug builds)
//   4. Crear VkSurfaceKHR via glfwCreateWindowSurface
//   5. Seleccionar VkPhysicalDevice (discrete GPU preferido)
//   6. Encontrar familias de colas: graphics + present
//   7. Crear VkDevice con VK_KHR_swapchain
//   8. Obtener VkQueues
//   9. Crear VkDescriptorPool para ImGui (256 sets)
//
#include <phyriad/render/vulkan/VulkanContext.hpp>
// Platform-specific extension-name macros live behind these headers.
// Including them here keeps VulkanContext.hpp clean of platform headers.
#ifdef _WIN32
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>            // HANDLE, HINSTANCE — needed by vulkan_win32.h
#   include <vulkan/vulkan_win32.h>
#endif
#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstring>
#include <array>
#include <algorithm>

namespace phyriad::render::vulkan {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers internos (file-scope)
// ─────────────────────────────────────────────────────────────────────────────

static bool check_vk(VkResult r, const char* op) noexcept {
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[VulkanContext] %s → VkResult %d\n", op, (int)r);
        return false;
    }
    return true;
}

// Nombre de la extension requerida por el device para el swapchain.
static constexpr const char* kSwapchainExt = VK_KHR_SWAPCHAIN_EXTENSION_NAME;

// Nombre del layer de validación de Khronos.
static constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

// Verificar si un layer está disponible.
static bool layer_available(const char* name) noexcept {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    if (count == 0u) return false;

    // Stack allocation: los layers instalados raramente superan 32.
    VkLayerProperties layers[64];
    if (count > 64u) count = 64u;
    vkEnumerateInstanceLayerProperties(&count, layers);

    for (uint32_t i = 0u; i < count; ++i) {
        if (std::strcmp(layers[i].layerName, name) == 0)
            return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Debug messenger — solo en builds de debug (NDEBUG no definido)
// ─────────────────────────────────────────────────────────────────────────────
#ifndef NDEBUG

static VKAPI_ATTR VkBool32 VKAPI_CALL vk_debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
    VkDebugUtilsMessageTypeFlagsEXT             /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*user_data*/) noexcept
{
    // Solo imprimir warnings y errores — los verbosos son demasiado ruidosos.
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::fprintf(stderr, "[Vulkan Validation] %s\n", data->pMessage);
    }
    return VK_FALSE;
}

// Cargar la función de extensión de debug utils (no está en el core loader).
static PFN_vkCreateDebugUtilsMessengerEXT  s_vkCreateDebugUtilsMessengerEXT  = nullptr;
static PFN_vkDestroyDebugUtilsMessengerEXT s_vkDestroyDebugUtilsMessengerEXT = nullptr;

static void load_debug_ext_fns(VkInstance inst) noexcept {
    s_vkCreateDebugUtilsMessengerEXT  = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(inst, "vkCreateDebugUtilsMessengerEXT"));
    s_vkDestroyDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(inst, "vkDestroyDebugUtilsMessengerEXT"));
}

#endif // !NDEBUG

// ─────────────────────────────────────────────────────────────────────────────
// VulkanContext — constructor / destructor
// ─────────────────────────────────────────────────────────────────────────────
VulkanContext::VulkanContext()  noexcept = default;
VulkanContext::~VulkanContext() noexcept { shutdown(); }

// ─────────────────────────────────────────────────────────────────────────────
// init
// ─────────────────────────────────────────────────────────────────────────────
std::expected<void, phyriad::Error>
VulkanContext::init(GLFWwindow* window) noexcept
{
    if (initialized_) shutdown();

    if (!create_instance(window))         goto fail;
    if (!create_surface(window))          goto fail;
    if (!select_physical_device())        goto fail;
    if (!find_queue_families())           goto fail;
    if (!create_device())                 goto fail;
    if (!create_descriptor_pool())        goto fail;

    initialized_ = true;
    return {};

fail:
    shutdown();
    return std::unexpected(phyriad::Error{
        .code           = phyriad::ErrorCode::ResourceInitFailed,
        .source_node_id = 0,
        .timestamp_ns   = 0});
}

// ─────────────────────────────────────────────────────────────────────────────
// shutdown
// ─────────────────────────────────────────────────────────────────────────────
void VulkanContext::shutdown() noexcept
{
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }

    if (desc_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
        desc_pool_ = VK_NULL_HANDLE;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }

#ifndef NDEBUG
    if (debug_messenger_ != VK_NULL_HANDLE && s_vkDestroyDebugUtilsMessengerEXT) {
        s_vkDestroyDebugUtilsMessengerEXT(instance_, debug_messenger_, nullptr);
        debug_messenger_ = VK_NULL_HANDLE;
    }
#endif

    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

    phys_dev_      = VK_NULL_HANDLE;
    gfx_queue_     = VK_NULL_HANDLE;
    present_queue_ = VK_NULL_HANDLE;
    gfx_family_    = UINT32_MAX;
    present_family_= UINT32_MAX;
    initialized_   = false;
}

void VulkanContext::device_wait_idle() const noexcept {
    if (device_ != VK_NULL_HANDLE)
        vkDeviceWaitIdle(device_);
}

// ─────────────────────────────────────────────────────────────────────────────
// create_instance
// ─────────────────────────────────────────────────────────────────────────────
bool VulkanContext::create_instance(GLFWwindow* /*window*/) noexcept
{
    // ── Extensiones: GLFW required + debug_utils (debug) ─────────────────────
    uint32_t glfw_ext_count = 0;
    const char** glfw_exts  = glfwGetRequiredInstanceExtensions(&glfw_ext_count);
    if (!glfw_exts) {
        std::fprintf(stderr, "[VulkanContext] glfwGetRequiredInstanceExtensions failed\n");
        return false;
    }

    // Copiar a un array local para poder añadir debug_utils.
    const char* extensions[16]{};
    uint32_t    ext_count = 0u;
    for (uint32_t i = 0u; i < glfw_ext_count && ext_count < 14u; ++i)
        extensions[ext_count++] = glfw_exts[i];

    bool use_validation = false;
#ifndef NDEBUG
    extensions[ext_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    use_validation = layer_available(kValidationLayer);
    if (!use_validation)
        std::fprintf(stderr, "[VulkanContext] Validation layer not available — debug disabled\n");
#endif

    const char* layers[4]{};
    uint32_t    layer_count = 0u;
#ifndef NDEBUG
    if (use_validation)
        layers[layer_count++] = kValidationLayer;
#endif

    // ── AppInfo ───────────────────────────────────────────────────────────────
    VkApplicationInfo app_info{};
    app_info.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName   = "Phyriad";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName        = "Phyriad Engine";
    app_info.engineVersion      = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion         = VK_API_VERSION_1_2;

    // ── InstanceCreateInfo ────────────────────────────────────────────────────
    VkInstanceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo        = &app_info;
    ci.enabledExtensionCount   = ext_count;
    ci.ppEnabledExtensionNames = extensions;
    ci.enabledLayerCount       = layer_count;
    ci.ppEnabledLayerNames     = layers;

    if (!check_vk(vkCreateInstance(&ci, nullptr, &instance_), "vkCreateInstance"))
        return false;

#ifndef NDEBUG
    load_debug_ext_fns(instance_);
    if (!setup_debug_messenger()) {
        // Non-fatal: continue without debug messenger
    }
#endif

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// setup_debug_messenger (debug only)
// ─────────────────────────────────────────────────────────────────────────────
bool VulkanContext::setup_debug_messenger() noexcept
{
#ifndef NDEBUG
    if (!s_vkCreateDebugUtilsMessengerEXT) return false;

    VkDebugUtilsMessengerCreateInfoEXT ci{};
    ci.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT
                       | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                       | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                       | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                       | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = vk_debug_callback;

    return check_vk(
        s_vkCreateDebugUtilsMessengerEXT(instance_, &ci, nullptr, &debug_messenger_),
        "vkCreateDebugUtilsMessengerEXT");
#else
    return true;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// create_surface
// ─────────────────────────────────────────────────────────────────────────────
bool VulkanContext::create_surface(GLFWwindow* window) noexcept
{
    const VkResult r = glfwCreateWindowSurface(instance_, window, nullptr, &surface_);
    return check_vk(r, "glfwCreateWindowSurface");
}

// ─────────────────────────────────────────────────────────────────────────────
// supports_device_extensions
// ─────────────────────────────────────────────────────────────────────────────
bool VulkanContext::supports_device_extensions(VkPhysicalDevice dev) const noexcept
{
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr);
    if (count == 0u) return false;

    // Use a fixed-size buffer (256 extensions is plenty for any real GPU).
    VkExtensionProperties props[256];
    if (count > 256u) count = 256u;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, props);

    for (uint32_t i = 0u; i < count; ++i) {
        if (std::strcmp(props[i].extensionName, kSwapchainExt) == 0)
            return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// select_physical_device
// ─────────────────────────────────────────────────────────────────────────────
bool VulkanContext::select_physical_device() noexcept
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0u) {
        std::fprintf(stderr, "[VulkanContext] No Vulkan-capable GPU found\n");
        return false;
    }

    VkPhysicalDevice devs[16];
    if (count > 16u) count = 16u;
    vkEnumeratePhysicalDevices(instance_, &count, devs);

    // Prefer discrete GPU; fall back to any device that supports the swapchain ext.
    VkPhysicalDevice fallback = VK_NULL_HANDLE;
    for (uint32_t i = 0u; i < count; ++i) {
        if (!supports_device_extensions(devs[i])) continue;

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(devs[i], &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            phys_dev_ = devs[i];
            std::fprintf(stderr, "[VulkanContext] GPU selected: %s (discrete)\n",
                         props.deviceName);
            return true;
        }
        if (fallback == VK_NULL_HANDLE) fallback = devs[i];
    }

    phys_dev_ = fallback;
    if (phys_dev_ != VK_NULL_HANDLE) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(phys_dev_, &props);
        std::fprintf(stderr, "[VulkanContext] GPU selected: %s (fallback)\n",
                     props.deviceName);
        return true;
    }

    std::fprintf(stderr, "[VulkanContext] No suitable GPU with %s found\n", kSwapchainExt);
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// find_queue_families
// ─────────────────────────────────────────────────────────────────────────────
bool VulkanContext::find_queue_families() noexcept
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys_dev_, &count, nullptr);
    if (count == 0u) return false;

    VkQueueFamilyProperties families[32];
    if (count > 32u) count = 32u;
    vkGetPhysicalDeviceQueueFamilyProperties(phys_dev_, &count, families);

    for (uint32_t i = 0u; i < count; ++i) {
        // Graphics family
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
             gfx_family_ == UINT32_MAX) {
            gfx_family_ = i;
        }
        // Present family (may be same as graphics)
        VkBool32 present_support = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(phys_dev_, i, surface_, &present_support);
        if (present_support && present_family_ == UINT32_MAX) {
            present_family_ = i;
        }
        if (gfx_family_ != UINT32_MAX && present_family_ != UINT32_MAX)
            break;
    }

    if (gfx_family_ == UINT32_MAX || present_family_ == UINT32_MAX) {
        std::fprintf(stderr, "[VulkanContext] Required queue families not found\n");
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// create_device
// ─────────────────────────────────────────────────────────────────────────────
bool VulkanContext::create_device() noexcept
{
    // Queue create infos — graphics + present (may be same family).
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_cis[2]{};
    uint32_t queue_ci_count = 0u;

    queue_cis[queue_ci_count].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_cis[queue_ci_count].queueFamilyIndex = gfx_family_;
    queue_cis[queue_ci_count].queueCount       = 1u;
    queue_cis[queue_ci_count].pQueuePriorities = &priority;
    ++queue_ci_count;

    if (present_family_ != gfx_family_) {
        queue_cis[queue_ci_count].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_cis[queue_ci_count].queueFamilyIndex = present_family_;
        queue_cis[queue_ci_count].queueCount       = 1u;
        queue_cis[queue_ci_count].pQueuePriorities = &priority;
        ++queue_ci_count;
    }

    // Enable only the features we need (none for basic ImGui rendering).
    VkPhysicalDeviceFeatures features{};

    // ── Optional external-memory / semaphore extensions for CompositeBackend
    //    VK↔GL interop. We enumerate the supported device extensions and
    //    enable each only when present. Their absence is non-fatal — only
    //    the CompositeBackend interop path is disabled.
    const char* kExtMemExt =
#ifdef _WIN32
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME;
#else
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
#endif
    const char* kExtSemExt =
#ifdef _WIN32
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME;
#else
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
#endif

    {
        uint32_t n_props = 0u;
        vkEnumerateDeviceExtensionProperties(phys_dev_, nullptr, &n_props, nullptr);
        // 256 covers any real driver (typical Vulkan device exposes ~70-150).
        VkExtensionProperties props[256];
        if (n_props > 256u) n_props = 256u;
        vkEnumerateDeviceExtensionProperties(phys_dev_, nullptr, &n_props, props);
        for (uint32_t i = 0u; i < n_props; ++i) {
            if (std::strcmp(props[i].extensionName, kExtMemExt) == 0)
                external_memory_enabled_    = true;
            if (std::strcmp(props[i].extensionName, kExtSemExt) == 0)
                external_semaphore_enabled_ = true;
        }
        if (external_memory_enabled_) {
            std::fprintf(stderr,
                "[VulkanContext] external-memory interop enabled (%s)\n",
                kExtMemExt);
        }
    }

    // Device extensions: required swapchain + (optional) external memory/sem.
    const char* dev_exts[4]{};
    uint32_t    dev_ext_count = 0u;
    dev_exts[dev_ext_count++] = kSwapchainExt;
    if (external_memory_enabled_)    dev_exts[dev_ext_count++] = kExtMemExt;
    if (external_semaphore_enabled_) dev_exts[dev_ext_count++] = kExtSemExt;

    VkDeviceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount    = queue_ci_count;
    ci.pQueueCreateInfos       = queue_cis;
    ci.enabledExtensionCount   = dev_ext_count;
    ci.ppEnabledExtensionNames = dev_exts;
    ci.pEnabledFeatures        = &features;

    if (!check_vk(vkCreateDevice(phys_dev_, &ci, nullptr, &device_),
                  "vkCreateDevice"))
        return false;

    vkGetDeviceQueue(device_, gfx_family_,     0u, &gfx_queue_);
    vkGetDeviceQueue(device_, present_family_,  0u, &present_queue_);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// create_descriptor_pool
// ─────────────────────────────────────────────────────────────────────────────
bool VulkanContext::create_descriptor_pool() noexcept
{
    // Pool grande para ImGui (samplers + combined image samplers, etc.).
    // Valores copiados de la muestra oficial de ImGui Vulkan.
    const VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER,                1000u },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000u },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          1000u },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1000u },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,   1000u },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,   1000u },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1000u },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         1000u },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000u },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000u },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,       1000u },
    };

    VkDescriptorPoolCreateInfo ci{};
    ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    ci.maxSets       = 1000u;
    ci.poolSizeCount = static_cast<uint32_t>(std::size(pool_sizes));
    ci.pPoolSizes    = pool_sizes;

    return check_vk(vkCreateDescriptorPool(device_, &ci, nullptr, &desc_pool_),
                    "vkCreateDescriptorPool");
}

} // namespace phyriad::render::vulkan
// Made with my soul - Swately <3
