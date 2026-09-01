// VulkanRenderer.cpp â€” primary render path for the M0 timing spike.
//
// Design goals (in order of importance):
//   1. Frame-exact target presentation, verifiable by external audit.
//   2. VK_GOOGLE_display_timing for both scheduling (desiredPresentTime) and
//      retrieval (actualPresentTime).
//   3. Zero allocations, zero JNI, no exceptions in the per-frame path.
//   4. Cleanly recoverable teardown on any Vulkan failure.
//
// Non-goals:
//   - Pretty content. The stimulus is a full-field grayscale patch.
//   - Multiple render passes or post-processing.
//   - Depth buffer, MSAA, HDR â€” none of it helps timing.
//
// The renderer is single-threaded from Kotlin's perspective. Internally it
// owns a render thread pinned to THREAD_PRIORITY_URGENT_AUDIO. Only that
// thread touches Vulkan objects after Init() returns.

#include "RenderPath.h"
#include "../timing/FrameScheduler.h"
#include "../fiducial/Fiducial.h"
#include "StimulusShadersSpv.h"

#include <android/log.h>
#include <android/native_window.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdlib>

#define TAG "VulkanRenderer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define VK_CHECK(expr) do { \
    VkResult _r = (expr); \
    if (_r != VK_SUCCESS) { \
        LOGE("VK_CHECK failed: " #expr " = %d at %s:%d", (int)_r, __FILE__, __LINE__); \
        return false; \
    } \
} while (0)

namespace rsp::render {

// Fixed constants for the swapchain layout â€” kept small, no dynamic knobs.
constexpr uint32_t kSwapchainImageCountRequested = 3;   // may end up 2 or 3 depending on caps
constexpr VkFormat kPreferredSurfaceFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr int kMinFutureFrames = 2;  // spec Â§5.2.2 rule 5

// Presentation-source codes match the Kotlin bridge.
constexpr int kSourceVkDisplayTiming = 2;
constexpr int kSourceMissing = 3;

// Push-constant struct â€” mirrors the shader PC layout exactly.
struct PushConstants {
    uint32_t cells0 = 0;
    uint32_t cells1 = 0;
    uint32_t cells2 = 0;
    uint32_t cells3 = 0;
    float target_intensity = 0.0f;
    float ambient_intensity = 0.0f;
    float viewport_w = 0.0f;
    float viewport_h = 0.0f;
};

// Pack the 100 fiducial cells into 4 uint32s.
static void PackCells(const uint8_t cells[fiducial::kCells], PushConstants& out) {
    out.cells0 = out.cells1 = out.cells2 = out.cells3 = 0;
    for (int i = 0; i < fiducial::kCells; ++i) {
        uint32_t bit = cells[i] & 1u;
        if      (i < 32) out.cells0 |= (bit << i);
        else if (i < 64) out.cells1 |= (bit << (i - 32));
        else if (i < 96) out.cells2 |= (bit << (i - 64));
        else             out.cells3 |= (bit << (i - 96));
    }
}

// Function-pointer holders for VK_GOOGLE_display_timing.
struct DisplayTimingFns {
    PFN_vkGetPastPresentationTimingGOOGLE getPast = nullptr;
    PFN_vkGetRefreshCycleDurationGOOGLE   getRefresh = nullptr;
    bool ok() const { return getPast && getRefresh; }
};

namespace {

class VulkanRendererImpl final : public RenderPath {
public:
    ~VulkanRendererImpl() override { Shutdown(); }

    int Init() override {
        if (!CreateInstance()) return -1;
        if (!SelectPhysicalDeviceAndQueueFamily()) return -2;
        if (!CreateDeviceAndQueue()) return -3;
        // Surface + swapchain deferred to SetSurface() because we need the ANativeWindow.
        return 0;
    }

    bool PreflightOK() override {
        return has_display_timing_ && instance_ && device_ && physical_device_;
    }

    void SetSurface(ANativeWindow* window) override {
        if (window == nullptr) {
            // Called at teardown.
            DestroySurfaceAndSwapchain();
            window_ = nullptr;
            return;
        }
        window_ = window;
        if (!CreateSurface()) { LOGE("CreateSurface failed"); return; }
        if (!CreateSwapchainAndImageResources()) { LOGE("CreateSwapchain failed"); return; }
        if (!CreatePipeline()) { LOGE("CreatePipeline failed"); return; }
        if (!CreateSyncAndCommandResources()) { LOGE("CreateSync failed"); return; }
    }

    RefreshProbe ProbeRefresh(int sample_frames) override {
        RefreshProbe rp{};
        if (!has_display_timing_ || !swapchain_) return rp;

        // Prefer vkGetRefreshCycleDurationGOOGLE â€” one call, no rendering.
        VkRefreshCycleDurationGOOGLE cycle{};
        if (dt_fns_.getRefresh(device_, swapchain_, &cycle) == VK_SUCCESS &&
            cycle.refreshDuration > 0) {
            rp.frame_period_ns_median = static_cast<int64_t>(cycle.refreshDuration);
            rp.measured_hz = 1e9 / static_cast<double>(rp.frame_period_ns_median);
            // Jitter estimation via rendering N ambient frames and diffing actual times.
            rp.jitter_p99_ns = ProbeJitterByRenderingFrames(sample_frames);
            frame_period_ns_ = rp.frame_period_ns_median;
            LOGI("Refresh probe: %.3f Hz period=%lld ns jitter_p99=%lld ns",
                 rp.measured_hz, (long long)rp.frame_period_ns_median, (long long)rp.jitter_p99_ns);
            return rp;
        }
        LOGE("vkGetRefreshCycleDurationGOOGLE failed");
        return rp;
    }

    void ConfigureSequence(const SequenceConfig& cfg) override {
        cfg_ = cfg;
        plan_ = timing::FrameScheduler::Plan(cfg.target_duration_ms, frame_period_ns_, cfg.tolerance_ms);
        LOGI("Sequence configured: target=%.2fms frames=%d achieved=%.3fms dev=%.3fms in_tol=%d",
             cfg.target_duration_ms, plan_.frame_count, plan_.achieved_duration_ms,
             plan_.deviation_from_target_ms, plan_.within_tolerance ? 1 : 0);
    }

    void Start() override {
        if (running_.exchange(true)) return;
        stop_requested_.store(false);
        pthread_create(&render_thread_, nullptr, &RenderThreadStatic, this);
    }

    void Stop() override { stop_requested_.store(true); }

    bool IsRunning() override { return running_.load(); }

    int DrainEvents(timing::StimulusEventRecord* out, int max) override {
        return log_.Drain(out, max);
    }

    void Shutdown() override {
        if (running_.load()) {
            Stop();
            pthread_join(render_thread_, nullptr);
        }
        if (device_) vkDeviceWaitIdle(device_);
        DestroySurfaceAndSwapchain();
        if (device_) { vkDestroyDevice(device_, nullptr); device_ = VK_NULL_HANDLE; }
        if (instance_) { vkDestroyInstance(instance_, nullptr); instance_ = VK_NULL_HANDLE; }
    }

private:
    // ---------------- Instance / Device ----------------

    bool CreateInstance() {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "rsp-m0";
        app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        app.pEngineName = "rsp-timing";
        app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        app.apiVersion = VK_API_VERSION_1_1;

        const char* extensions[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
        };

        VkInstanceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &app;
        ci.enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]);
        ci.ppEnabledExtensionNames = extensions;

        VK_CHECK(vkCreateInstance(&ci, nullptr, &instance_));
        return true;
    }

    bool SelectPhysicalDeviceAndQueueFamily() {
        uint32_t count = 0;
        VK_CHECK(vkEnumeratePhysicalDevices(instance_, &count, nullptr));
        if (count == 0) { LOGE("No physical devices"); return false; }
        // We only need up to 8; static array avoids dynamic allocation.
        VkPhysicalDevice devs[8];
        if (count > 8) count = 8;
        VK_CHECK(vkEnumeratePhysicalDevices(instance_, &count, devs));

        for (uint32_t i = 0; i < count; ++i) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(devs[i], &props);
            // Prefer discrete or integrated over anything else; on phones there's
            // typically only one anyway.
            physical_device_ = devs[i];

            uint32_t qcount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qcount, nullptr);
            VkQueueFamilyProperties qprops[8];
            if (qcount > 8) qcount = 8;
            vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qcount, qprops);
            for (uint32_t q = 0; q < qcount; ++q) {
                if (qprops[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                    // Presentation-family check happens after surface is created; the graphics
                    // family also presents on Android in practice.
                    queue_family_index_ = q;
                    break;
                }
            }
            if (queue_family_index_ != UINT32_MAX) break;
        }
        if (physical_device_ == VK_NULL_HANDLE || queue_family_index_ == UINT32_MAX) {
            LOGE("No suitable GPU/queue family");
            return false;
        }
        // Probe device extensions for VK_GOOGLE_display_timing NOW so PreflightOK() is meaningful.
        uint32_t ecount = 0;
        VK_CHECK(vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &ecount, nullptr));
        VkExtensionProperties exts[512];
        if (ecount > 512) ecount = 512;
        VK_CHECK(vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &ecount, exts));
        for (uint32_t i = 0; i < ecount; ++i) {
            if (strcmp(exts[i].extensionName, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME) == 0) {
                has_display_timing_ = true;
                break;
            }
        }
        if (!has_display_timing_) {
            LOGE("Device does not support VK_GOOGLE_display_timing â€” this device is not "
                 "clinical-tier eligible under Redline Patch 7");
        }
        return true;
    }

    bool CreateDeviceAndQueue() {
        if (!has_display_timing_) return false;  // fail closed per Patch 7

        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = queue_family_index_;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;

        const char* dev_exts[] = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME,
        };
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = sizeof(dev_exts) / sizeof(dev_exts[0]);
        dci.ppEnabledExtensionNames = dev_exts;

        VK_CHECK(vkCreateDevice(physical_device_, &dci, nullptr, &device_));
        vkGetDeviceQueue(device_, queue_family_index_, 0, &queue_);

        // Resolve display-timing function pointers.
        dt_fns_.getPast = (PFN_vkGetPastPresentationTimingGOOGLE)
            vkGetDeviceProcAddr(device_, "vkGetPastPresentationTimingGOOGLE");
        dt_fns_.getRefresh = (PFN_vkGetRefreshCycleDurationGOOGLE)
            vkGetDeviceProcAddr(device_, "vkGetRefreshCycleDurationGOOGLE");
        if (!dt_fns_.ok()) {
            LOGE("Failed to resolve VK_GOOGLE_display_timing function pointers");
            return false;
        }
        return true;
    }

    // ---------------- Surface / Swapchain ----------------

    bool CreateSurface() {
        VkAndroidSurfaceCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        sci.window = window_;
        VK_CHECK(vkCreateAndroidSurfaceKHR(instance_, &sci, nullptr, &surface_));

        VkBool32 supported = VK_FALSE;
        VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, queue_family_index_,
                                                     surface_, &supported));
        if (!supported) { LOGE("Queue family cannot present"); return false; }
        return true;
    }

    bool CreateSwapchainAndImageResources() {
        VkSurfaceCapabilitiesKHR caps{};
        VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps));

        uint32_t fmt_count = 0;
        VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &fmt_count, nullptr));
        VkSurfaceFormatKHR fmts[32];
        if (fmt_count > 32) fmt_count = 32;
        VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &fmt_count, fmts));

        VkSurfaceFormatKHR chosen = fmts[0];
        for (uint32_t i = 0; i < fmt_count; ++i) {
            if (fmts[i].format == kPreferredSurfaceFormat &&
                fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                chosen = fmts[i]; break;
            }
        }
        surface_format_ = chosen;

        // Extent â€” trust the surface caps.
        swapchain_extent_ = caps.currentExtent;
        if (swapchain_extent_.width == 0xFFFFFFFF) {
            swapchain_extent_.width  = 1080;
            swapchain_extent_.height = 2400;
        }

        uint32_t image_count = kSwapchainImageCountRequested;
        if (image_count < caps.minImageCount) image_count = caps.minImageCount;
        if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) image_count = caps.maxImageCount;

        // IMPORTANT: FIFO present mode is what we want. It's the only mode that
        // guarantees vsync alignment, which is required for VK_GOOGLE_display_timing
        // to produce meaningful actualPresentTime values.
        VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;

        VkSwapchainCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        ci.surface = surface_;
        ci.minImageCount = image_count;
        ci.imageFormat = chosen.format;
        ci.imageColorSpace = chosen.colorSpace;
        ci.imageExtent = swapchain_extent_;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = caps.currentTransform;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode = present_mode;
        ci.clipped = VK_TRUE;
        ci.oldSwapchain = VK_NULL_HANDLE;

        VK_CHECK(vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_));

        VK_CHECK(vkGetSwapchainImagesKHR(device_, swapchain_, &image_count_, nullptr));
        if (image_count_ > kMaxSwapchainImages) image_count_ = kMaxSwapchainImages;
        VK_CHECK(vkGetSwapchainImagesKHR(device_, swapchain_, &image_count_, swapchain_images_));

        // Image views
        for (uint32_t i = 0; i < image_count_; ++i) {
            VkImageViewCreateInfo vci{};
            vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vci.image = swapchain_images_[i];
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = chosen.format;
            vci.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
                              VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &image_views_[i]));
        }

        // Render pass â€” one color attachment, no depth.
        VkAttachmentDescription attach{};
        attach.format = chosen.format;
        attach.samples = VK_SAMPLE_COUNT_1_BIT;
        attach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attach.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &cref;

        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = 0;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 1;
        rpci.pAttachments = &attach;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sub;
        rpci.dependencyCount = 1;
        rpci.pDependencies = &dep;
        VK_CHECK(vkCreateRenderPass(device_, &rpci, nullptr, &render_pass_));

        // Framebuffers
        for (uint32_t i = 0; i < image_count_; ++i) {
            VkFramebufferCreateInfo fci{};
            fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fci.renderPass = render_pass_;
            fci.attachmentCount = 1;
            fci.pAttachments = &image_views_[i];
            fci.width = swapchain_extent_.width;
            fci.height = swapchain_extent_.height;
            fci.layers = 1;
            VK_CHECK(vkCreateFramebuffer(device_, &fci, nullptr, &framebuffers_[i]));
        }
        return true;
    }

    bool CreatePipeline() {
        // Shader modules
        VkShaderModuleCreateInfo v_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                     nullptr, 0, shaders::kStimulusVertSpv_size,
                                     shaders::kStimulusVertSpv};
        VkShaderModuleCreateInfo f_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                     nullptr, 0, shaders::kStimulusFragSpv_size,
                                     shaders::kStimulusFragSpv};
        VK_CHECK(vkCreateShaderModule(device_, &v_ci, nullptr, &vert_module_));
        VK_CHECK(vkCreateShaderModule(device_, &f_ci, nullptr, &frag_module_));

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert_module_;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag_module_;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkViewport vp{0, 0, (float)swapchain_extent_.width, (float)swapchain_extent_.height, 0, 1};
        VkRect2D sc{{0, 0}, swapchain_extent_};
        VkPipelineViewportStateCreateInfo vs{};
        vs.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vs.viewportCount = 1; vs.pViewports = &vp;
        vs.scissorCount = 1; vs.pScissors = &sc;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = 0xF;
        cba.blendEnable = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;

        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pcr.offset = 0;
        pcr.size = sizeof(PushConstants);

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pcr;
        VK_CHECK(vkCreatePipelineLayout(device_, &plci, nullptr, &pipeline_layout_));

        VkGraphicsPipelineCreateInfo gci{};
        gci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gci.stageCount = 2;
        gci.pStages = stages;
        gci.pVertexInputState = &vi;
        gci.pInputAssemblyState = &ia;
        gci.pViewportState = &vs;
        gci.pRasterizationState = &rs;
        gci.pMultisampleState = &ms;
        gci.pColorBlendState = &cb;
        gci.layout = pipeline_layout_;
        gci.renderPass = render_pass_;
        gci.subpass = 0;
        VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gci, nullptr, &pipeline_));
        return true;
    }

    bool CreateSyncAndCommandResources() {
        VkCommandPoolCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpci.queueFamilyIndex = queue_family_index_;
        VK_CHECK(vkCreateCommandPool(device_, &cpci, nullptr, &command_pool_));

        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = command_pool_;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = kFramesInFlight;
        VK_CHECK(vkAllocateCommandBuffers(device_, &cbai, command_buffers_));

        for (int i = 0; i < kFramesInFlight; ++i) {
            VkSemaphoreCreateInfo sci{}; sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            VkFenceCreateInfo fci{}; fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &sem_image_available_[i]));
            VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &sem_render_done_[i]));
            VK_CHECK(vkCreateFence(device_, &fci, nullptr, &fence_in_flight_[i]));
        }
        return true;
    }

    void DestroySurfaceAndSwapchain() {
        if (!device_) return;
        vkDeviceWaitIdle(device_);
        for (int i = 0; i < kFramesInFlight; ++i) {
            if (sem_image_available_[i]) vkDestroySemaphore(device_, sem_image_available_[i], nullptr);
            if (sem_render_done_[i]) vkDestroySemaphore(device_, sem_render_done_[i], nullptr);
            if (fence_in_flight_[i]) vkDestroyFence(device_, fence_in_flight_[i], nullptr);
            sem_image_available_[i] = VK_NULL_HANDLE;
            sem_render_done_[i] = VK_NULL_HANDLE;
            fence_in_flight_[i] = VK_NULL_HANDLE;
        }
        if (command_pool_) { vkDestroyCommandPool(device_, command_pool_, nullptr); command_pool_ = VK_NULL_HANDLE; }
        if (pipeline_) { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
        if (pipeline_layout_) { vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr); pipeline_layout_ = VK_NULL_HANDLE; }
        if (vert_module_) { vkDestroyShaderModule(device_, vert_module_, nullptr); vert_module_ = VK_NULL_HANDLE; }
        if (frag_module_) { vkDestroyShaderModule(device_, frag_module_, nullptr); frag_module_ = VK_NULL_HANDLE; }
        for (uint32_t i = 0; i < image_count_; ++i) {
            if (framebuffers_[i]) vkDestroyFramebuffer(device_, framebuffers_[i], nullptr);
            if (image_views_[i]) vkDestroyImageView(device_, image_views_[i], nullptr);
            framebuffers_[i] = VK_NULL_HANDLE;
            image_views_[i] = VK_NULL_HANDLE;
        }
        if (render_pass_) { vkDestroyRenderPass(device_, render_pass_, nullptr); render_pass_ = VK_NULL_HANDLE; }
        if (swapchain_) { vkDestroySwapchainKHR(device_, swapchain_, nullptr); swapchain_ = VK_NULL_HANDLE; }
        if (surface_) { vkDestroySurfaceKHR(instance_, surface_, nullptr); surface_ = VK_NULL_HANDLE; }
    }

    // ---------------- Render thread ----------------

    static void* RenderThreadStatic(void* self) {
        static_cast<VulkanRendererImpl*>(self)->RenderThreadMain();
        return nullptr;
    }

    void RenderThreadMain() {
        setpriority(PRIO_PROCESS, static_cast<int>(syscall(SYS_gettid)), -19);

        uint32_t global_frame_counter = 0;
        // Anchor future presentation time. Use CLOCK_MONOTONIC â€” VK_GOOGLE_display_timing
        // uses the same monotonic clock domain on Android.
        int64_t now = NowNanos();
        int64_t next_present_ns = now + int64_t(kMinFutureFrames) * frame_period_ns_;
        int64_t next_present_id = kMinFutureFrames;  // presentID sequence

        // Sequence loop
        for (int ev = 0; ev < cfg_.count && !stop_requested_.load(); ++ev) {
            // Ambient inter-event frames
            for (int i = 0; i < cfg_.inter_event_frames && !stop_requested_.load(); ++i) {
                if (!RenderAndPresent(FrameKind::kAmbient, global_frame_counter,
                                      next_present_id, next_present_ns)) {
                    LOGE("Present failed during ambient; aborting sequence");
                    stop_requested_.store(true);
                    break;
                }
                global_frame_counter++;
                next_present_id++;
                next_present_ns += frame_period_ns_;
            }
            if (stop_requested_.load()) break;

            // Start a new event record
            timing::StimulusEventRecord& b = log_.Building();
            b = timing::StimulusEventRecord{};   // value-initialize (POD-safe)
            b.event_index = ev;
            b.target_duration_ms = cfg_.target_duration_ms;
            b.frame_count = plan_.frame_count;
            b.frame_period_ns = plan_.frame_period_ns;
            b.achieved_duration_ms = plan_.achieved_duration_ms;
            b.timing_deviation_ns = static_cast<int64_t>(
                plan_.deviation_from_target_ms * 1'000'000.0);
            b.refresh_hz_at_event = 1e9 / static_cast<double>(frame_period_ns_);
            b.brightness_at_event = cfg_.brightness_at_event;
            b.scheduled_frame_number = next_present_id;
            for (int i = 0; i < plan_.frame_count; ++i) {
                b.per_frame_source[i] = kSourceMissing;
            }
            EncodeFiducialNonce(b.fiducial_nonce_hex, MakeEventNonce(ev));

            // Target frames
            for (int i = 0; i < plan_.frame_count && !stop_requested_.load(); ++i) {
                b.per_frame_intended_ns[i] = next_present_ns;
                if (!RenderAndPresent(FrameKind::kTarget, global_frame_counter,
                                      next_present_id, next_present_ns)) {
                    LOGE("Present failed during target; aborting sequence");
                    stop_requested_.store(true);
                    break;
                }
                global_frame_counter++;
                next_present_id++;
                next_present_ns += frame_period_ns_;
            }
            if (stop_requested_.load()) break;

            // A few ambient frames after the event to let display timing catch up.
            for (int i = 0; i < 4 && !stop_requested_.load(); ++i) {
                RenderAndPresent(FrameKind::kAmbient, global_frame_counter,
                                 next_present_id, next_present_ns);
                global_frame_counter++;
                next_present_id++;
                next_present_ns += frame_period_ns_;
            }

            RetrievePastPresentationTiming();  // fills log_.Building().per_frame_actual_ns
            FinalizeEventStatus(b);
            log_.PushCompletedEvent(b);
        }

        // Wait for outstanding GPU work before signalling not-running.
        if (device_) vkDeviceWaitIdle(device_);
        running_.store(false);
    }

    // ---------------- Per-frame render/present ----------------

    enum class FrameKind { kAmbient, kTarget };

    bool RenderAndPresent(FrameKind kind, uint32_t counter,
                          int64_t present_id, int64_t desired_present_ns) {
        int slot = frame_slot_;
        frame_slot_ = (frame_slot_ + 1) % kFramesInFlight;

        // Gate: wait for this slot's previous submission to finish.
        VK_CHECK(vkWaitForFences(device_, 1, &fence_in_flight_[slot], VK_TRUE, UINT64_MAX));
        VK_CHECK(vkResetFences(device_, 1, &fence_in_flight_[slot]));

        uint32_t image_index = 0;
        VkResult acq = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                             sem_image_available_[slot],
                                             VK_NULL_HANDLE, &image_index);
        if (acq == VK_ERROR_OUT_OF_DATE_KHR || acq == VK_SUBOPTIMAL_KHR) {
            // Swapchain change (e.g., refresh drift, orientation) â€” per Redline Patch 7 rule,
            // any refresh drift aborts the block.
            LOGE("Swapchain out-of-date at acquire (%d) â€” aborting", (int)acq);
            return false;
        }
        if (acq != VK_SUCCESS) {
            LOGE("vkAcquireNextImageKHR failed: %d", (int)acq);
            return false;
        }

        // Record command buffer
        VkCommandBuffer cb = command_buffers_[slot];
        VK_CHECK(vkResetCommandBuffer(cb, 0));

        VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        VK_CHECK(vkBeginCommandBuffer(cb, &bi));

        VkClearValue clear{};
        // Ambient background is dark; the shader picks the exact intensity.
        clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

        VkRenderPassBeginInfo rpb{}; rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpb.renderPass = render_pass_;
        rpb.framebuffer = framebuffers_[image_index];
        rpb.renderArea.offset = {0, 0};
        rpb.renderArea.extent = swapchain_extent_;
        rpb.clearValueCount = 1;
        rpb.pClearValues = &clear;
        vkCmdBeginRenderPass(cb, &rpb, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

        // Push constants: fiducial cells + intensities + viewport size.
        PushConstants pc{};
        uint8_t cells[fiducial::kCells];
        fiducial::FrameType ft = (kind == FrameKind::kTarget)
            ? fiducial::FRAME_TARGET : fiducial::FRAME_AMBIENT;
        fiducial::ComputeCells(cfg_.session_nonce, counter, ft, cells);
        PackCells(cells, pc);
        pc.target_intensity  = (kind == FrameKind::kTarget) ? 0.7f : 0.05f;
        pc.ambient_intensity = 0.05f;
        pc.viewport_w = (float)swapchain_extent_.width;
        pc.viewport_h = (float)swapchain_extent_.height;
        vkCmdPushConstants(cb, pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT,
                          0, sizeof(pc), &pc);

        // Fullscreen triangle (3 vertices, no buffer)
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
        VK_CHECK(vkEndCommandBuffer(cb));

        // Submit
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &sem_image_available_[slot];
        si.pWaitDstStageMask = &wait_stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &sem_render_done_[slot];
        VK_CHECK(vkQueueSubmit(queue_, 1, &si, fence_in_flight_[slot]));

        // Present WITH VK_GOOGLE_display_timing hints
        VkPresentTimeGOOGLE pt{};
        pt.presentID = static_cast<uint32_t>(present_id);
        pt.desiredPresentTime = static_cast<uint64_t>(desired_present_ns);
        VkPresentTimesInfoGOOGLE pti{}; pti.sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE;
        pti.swapchainCount = 1;
        pti.pTimes = &pt;

        VkPresentInfoKHR pi{}; pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.pNext = &pti;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &sem_render_done_[slot];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain_;
        pi.pImageIndices = &image_index;

        VkResult pr = vkQueuePresentKHR(queue_, &pi);
        if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) {
            LOGE("vkQueuePresentKHR failed: %d", (int)pr);
            return false;
        }
        return true;
    }

    // Retrieve VkPastPresentationTimingGOOGLE entries and bind them to the current event.
    void RetrievePastPresentationTiming() {
        if (!dt_fns_.ok() || !swapchain_) return;
        uint32_t count = 0;
        if (dt_fns_.getPast(device_, swapchain_, &count, nullptr) != VK_SUCCESS) return;
        if (count == 0) return;
        constexpr int kMax = 64;
        if (count > kMax) count = kMax;
        VkPastPresentationTimingGOOGLE pt[kMax];
        if (dt_fns_.getPast(device_, swapchain_, &count, pt) != VK_SUCCESS) return;
        for (uint32_t i = 0; i < count; ++i) {
            log_.BindPresentTimestamp(static_cast<int64_t>(pt[i].presentID),
                                      static_cast<int64_t>(pt[i].actualPresentTime),
                                      kSourceVkDisplayTiming);
        }
    }

    void FinalizeEventStatus(timing::StimulusEventRecord& b) {
        bool any_missing = false;
        int64_t worst_dev = 0;
        for (int i = 0; i < b.frame_count; ++i) {
            if (b.per_frame_source[i] == kSourceMissing) { any_missing = true; continue; }
            int64_t dev = b.per_frame_actual_ns[i] - b.per_frame_intended_ns[i];
            if (std::llabs(dev) > std::llabs(worst_dev)) worst_dev = dev;
        }
        if (any_missing) {
            strncpy(b.verification_status, "missing_pt", sizeof(b.verification_status));
            return;
        }
        int64_t tol_ns = static_cast<int64_t>(cfg_.tolerance_ms * 1'000'000.0);
        if (std::llabs(worst_dev) > tol_ns ||
            std::llabs(b.timing_deviation_ns) > tol_ns) {
            strncpy(b.verification_status, "failed", sizeof(b.verification_status));
        } else if (std::llabs(worst_dev) > tol_ns / 2) {
            strncpy(b.verification_status, "deviation", sizeof(b.verification_status));
        } else {
            strncpy(b.verification_status, "verified", sizeof(b.verification_status));
        }
    }

    // Small helper: render N ambient frames and collect their actual present times,
    // return p99 jitter.
    int64_t ProbeJitterByRenderingFrames(int n) {
        constexpr int kMaxProbe = 512;
        if (n > kMaxProbe) n = kMaxProbe;
        int64_t now = NowNanos();
        int64_t next_present_ns = now + int64_t(kMinFutureFrames) * frame_period_ns_;
        int64_t next_id = 1;
        for (int i = 0; i < n; ++i) {
            if (!RenderAndPresent(FrameKind::kAmbient, static_cast<uint32_t>(i),
                                  next_id, next_present_ns)) {
                LOGE("Probe present failed at frame %d", i);
                break;
            }
            next_id++;
            next_present_ns += frame_period_ns_;
        }
        // Give the compositor a moment to produce timing data.
        struct timespec ts{0, 60'000'000};  // 60 ms
        nanosleep(&ts, nullptr);
        uint32_t count = 0;
        if (dt_fns_.getPast(device_, swapchain_, &count, nullptr) != VK_SUCCESS) return 0;
        if (count == 0) return 0;
        if (count > kMaxProbe) count = kMaxProbe;
        VkPastPresentationTimingGOOGLE pt[kMaxProbe];
        if (dt_fns_.getPast(device_, swapchain_, &count, pt) != VK_SUCCESS) return 0;
        int64_t intervals[kMaxProbe]{};
        int intervals_n = 0;
        for (uint32_t i = 1; i < count; ++i) {
            int64_t dt = static_cast<int64_t>(pt[i].actualPresentTime) -
                         static_cast<int64_t>(pt[i-1].actualPresentTime);
            if (dt > 0) intervals[intervals_n++] = dt;
        }
        if (intervals_n == 0) return 0;
        // Compute p99 deviation from median by insertion sort (nâ‰¤kMaxProbe, in-place).
        for (int i = 1; i < intervals_n; ++i) {
            int64_t v = intervals[i];
            int j = i - 1;
            while (j >= 0 && intervals[j] > v) { intervals[j+1] = intervals[j]; j--; }
            intervals[j+1] = v;
        }
        int64_t median = intervals[intervals_n / 2];
        int64_t p99_index = (intervals_n * 99) / 100;
        if (p99_index >= intervals_n) p99_index = intervals_n - 1;
        int64_t p99 = std::llabs(intervals[p99_index] - median);
        return p99;
    }

    // ---------------- Utilities ----------------

    static int64_t NowNanos() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return int64_t(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
    }

    uint64_t MakeEventNonce(int event_index) {
        uint64_t n = cfg_.session_nonce;
        n ^= (uint64_t(event_index) * 0x9E3779B97F4A7C15ULL);
        return n;
    }

    void EncodeFiducialNonce(char* out, uint64_t nonce) {
        static const char hex[] = "0123456789abcdef";
        for (int i = 0; i < 16; ++i) {
            out[15 - i] = hex[(nonce >> (i * 4)) & 0xF];
        }
        out[16] = 0;
    }

    // ---------------- State ----------------

    ANativeWindow* window_ = nullptr;

    // Vulkan handles
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    uint32_t queue_family_index_ = UINT32_MAX;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue  queue_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSurfaceFormatKHR surface_format_{};
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D swapchain_extent_{};

    static constexpr int kMaxSwapchainImages = 4;
    uint32_t image_count_ = 0;
    VkImage swapchain_images_[kMaxSwapchainImages]{};
    VkImageView image_views_[kMaxSwapchainImages]{};
    VkFramebuffer framebuffers_[kMaxSwapchainImages]{};
    VkRenderPass render_pass_ = VK_NULL_HANDLE;

    VkShaderModule vert_module_ = VK_NULL_HANDLE;
    VkShaderModule frag_module_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    VkCommandPool command_pool_ = VK_NULL_HANDLE;

    static constexpr int kFramesInFlight = 2;
    VkCommandBuffer command_buffers_[kFramesInFlight]{};
    VkSemaphore sem_image_available_[kFramesInFlight]{};
    VkSemaphore sem_render_done_[kFramesInFlight]{};
    VkFence fence_in_flight_[kFramesInFlight]{};
    int frame_slot_ = 0;

    bool has_display_timing_ = false;
    DisplayTimingFns dt_fns_{};
    int64_t frame_period_ns_ = 8'333'333;

    SequenceConfig cfg_{};
    timing::FramePlan plan_{};
    timing::TimingLog log_;

    pthread_t render_thread_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
};

}  // namespace

RenderPath* CreateVulkanRenderer() {
    return new VulkanRendererImpl();
}

}  // namespace rsp::render
