// Vulkan 后端：Win32 窗口 + 实例/设备/交换链 + 清屏循环。
//
// 设计取舍：
//   1. 不依赖 Vulkan SDK。构建时只拉 Vulkan-Headers 头文件，运行时用
//      LoadLibrary("vulkan-1.dll") + vkGetInstanceProcAddr 取入口函数，
//      因此不链接 vulkan-1.lib，构建机也不需要 VULKAN_SDK。
//   2. 窗口直接用 Win32 API，不引入 GLFW/SDL，保持骨架零第三方依赖的性质。
//   3. 本阶段只做清屏。清屏颜色由相机朝向决定，用来肉眼确认
//      「按时间轴取到的机位在连续变化」；窗口标题同步显示帧号与机位。
//      舞台与角色的真实渲染在下一个里程碑接入。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "render_vulkan.h"

namespace liveview {
namespace {

constexpr uint32_t kFramesInFlight = 2;
constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;

// ---- 运行时取出的 Vulkan 入口 ----
// 全部是函数指针，避免链接导入库。
PFN_vkGetInstanceProcAddr g_get_instance_proc = nullptr;

PFN_vkCreateInstance g_vkCreateInstance = nullptr;
PFN_vkEnumerateInstanceExtensionProperties g_vkEnumerateInstanceExtensionProperties = nullptr;

PFN_vkDestroyInstance g_vkDestroyInstance = nullptr;
PFN_vkEnumeratePhysicalDevices g_vkEnumeratePhysicalDevices = nullptr;
PFN_vkEnumerateDeviceExtensionProperties g_vkEnumerateDeviceExtensionProperties = nullptr;
PFN_vkGetPhysicalDeviceProperties g_vkGetPhysicalDeviceProperties = nullptr;
PFN_vkGetPhysicalDeviceQueueFamilyProperties g_vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
PFN_vkGetPhysicalDeviceSurfaceSupportKHR g_vkGetPhysicalDeviceSurfaceSupportKHR = nullptr;
PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR g_vkGetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
PFN_vkGetPhysicalDeviceSurfaceFormatsKHR g_vkGetPhysicalDeviceSurfaceFormatsKHR = nullptr;
PFN_vkCreateWin32SurfaceKHR g_vkCreateWin32SurfaceKHR = nullptr;
PFN_vkDestroySurfaceKHR g_vkDestroySurfaceKHR = nullptr;
PFN_vkCreateDevice g_vkCreateDevice = nullptr;
PFN_vkGetDeviceProcAddr g_vkGetDeviceProcAddr = nullptr;

PFN_vkDestroyDevice g_vkDestroyDevice = nullptr;
PFN_vkGetDeviceQueue g_vkGetDeviceQueue = nullptr;
PFN_vkDeviceWaitIdle g_vkDeviceWaitIdle = nullptr;
PFN_vkCreateSwapchainKHR g_vkCreateSwapchainKHR = nullptr;
PFN_vkDestroySwapchainKHR g_vkDestroySwapchainKHR = nullptr;
PFN_vkGetSwapchainImagesKHR g_vkGetSwapchainImagesKHR = nullptr;
PFN_vkAcquireNextImageKHR g_vkAcquireNextImageKHR = nullptr;
PFN_vkQueuePresentKHR g_vkQueuePresentKHR = nullptr;
PFN_vkQueueSubmit g_vkQueueSubmit = nullptr;
PFN_vkCreateRenderPass g_vkCreateRenderPass = nullptr;
PFN_vkDestroyRenderPass g_vkDestroyRenderPass = nullptr;
PFN_vkCreateImageView g_vkCreateImageView = nullptr;
PFN_vkDestroyImageView g_vkDestroyImageView = nullptr;
PFN_vkCreateFramebuffer g_vkCreateFramebuffer = nullptr;
PFN_vkDestroyFramebuffer g_vkDestroyFramebuffer = nullptr;
PFN_vkCreateCommandPool g_vkCreateCommandPool = nullptr;
PFN_vkDestroyCommandPool g_vkDestroyCommandPool = nullptr;
PFN_vkAllocateCommandBuffers g_vkAllocateCommandBuffers = nullptr;
PFN_vkBeginCommandBuffer g_vkBeginCommandBuffer = nullptr;
PFN_vkEndCommandBuffer g_vkEndCommandBuffer = nullptr;
PFN_vkCmdBeginRenderPass g_vkCmdBeginRenderPass = nullptr;
PFN_vkCmdEndRenderPass g_vkCmdEndRenderPass = nullptr;
PFN_vkCreateSemaphore g_vkCreateSemaphore = nullptr;
PFN_vkDestroySemaphore g_vkDestroySemaphore = nullptr;
PFN_vkCreateFence g_vkCreateFence = nullptr;
PFN_vkDestroyFence g_vkDestroyFence = nullptr;
PFN_vkWaitForFences g_vkWaitForFences = nullptr;
PFN_vkResetFences g_vkResetFences = nullptr;

// ---- 窗口状态 ----
HWND g_window = nullptr;
bool g_window_resized = false;

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            g_window = nullptr;
            PostQuitMessage(0);
            return 0;
        case WM_SIZE:
            g_window_resized = true;
            return 0;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                DestroyWindow(window);
                return 0;
            }
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
    }
}

std::string ResultText(VkResult result) {
    return "VkResult " + std::to_string(static_cast<int>(result));
}

std::string FormatPose(const CameraKey& pose) {
    char buffer[192];
    std::snprintf(buffer, sizeof(buffer), "(%.1f, %.1f, %.1f) -> (%.1f, %.1f, %.1f)",
                  pose.position[0], pose.position[1], pose.position[2],
                  pose.target[0], pose.target[1], pose.target[2]);
    return buffer;
}

bool CreateRenderWindow(std::string* error) {
    HINSTANCE hinstance = GetModuleHandleW(nullptr);

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(WNDCLASSEXW);
    window_class.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = hinstance;
    // 这里不能用 LoadCursorW + IDC_ARROW：未定义 UNICODE 时 IDC_ARROW 展开成
    // MAKEINTRESOURCEA（char*），和 LoadCursorW 的 LPCWSTR 参数类型不匹配。
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.lpszClassName = L"liveview_window";
    if (RegisterClassExW(&window_class) == 0) {
        const DWORD code = GetLastError();
        if (code != ERROR_CLASS_ALREADY_EXISTS) {
            *error = "注册窗口类失败（Win32 错误 " + std::to_string(code) + "）";
            return false;
        }
    }

    RECT rect = {0, 0, kWindowWidth, kWindowHeight};
    if (AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0) == 0) {
        *error = "计算窗口尺寸失败（Win32 错误 " + std::to_string(GetLastError()) + "）";
        return false;
    }

    g_window = CreateWindowExW(0, L"liveview_window", L"liveview", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                               rect.bottom - rect.top, nullptr, nullptr, hinstance, nullptr);
    if (g_window == nullptr) {
        *error = "创建窗口失败（Win32 错误 " + std::to_string(GetLastError()) + "）";
        return false;
    }

    ShowWindow(g_window, SW_SHOW);
    UpdateWindow(g_window);
    return true;
}

struct Renderer {
    HWND window = nullptr;
    HINSTANCE hinstance = nullptr;
    HMODULE loader = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkSurfaceFormatKHR surface_format{};
    VkExtent2D extent{};
    uint32_t image_count = 0;
    uint32_t frame_index = 0;
    std::vector<VkImage> images;
    std::vector<VkImageView> image_views;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkCommandBuffer> command_buffers;
    std::vector<VkSemaphore> image_available;
    std::vector<VkSemaphore> render_finished;
    std::vector<VkFence> in_flight;

    bool Init(HWND target, std::string* error);
    void Shutdown();
    bool DrawFrame(const float* color, std::string* error);

private:
    bool LoadGlobalFunctions(std::string* error);
    bool LoadInstanceFunctions(std::string* error);
    bool LoadDeviceFunctions(std::string* error);
    bool PickPhysicalDevice(std::string* error);
    bool CreateDeviceObject(std::string* error);
    bool CreateRenderPass(std::string* error);
    bool CreateCommandResources(std::string* error);
    bool CreateSyncObjects(std::string* error);
    bool CreateSwapchain(std::string* error);
    bool RecreateSwapchain(std::string* error);
    void DestroySwapchainResources();
    bool RecordCommandBuffer(uint32_t image_index, const float* color);
};

#define LOAD_INSTANCE_FN(name)                                                        \
    do {                                                                              \
        g_##name = reinterpret_cast<decltype(g_##name)>(g_get_instance_proc(instance, #name)); \
        if (g_##name == nullptr) {                                                    \
            *error = "Vulkan 运行时缺少入口: " #name;                                  \
            return false;                                                             \
        }                                                                             \
    } while (false)

#define LOAD_DEVICE_FN(name)                                                    \
    do {                                                                        \
        g_##name = reinterpret_cast<decltype(g_##name)>(g_vkGetDeviceProcAddr(device, #name)); \
        if (g_##name == nullptr) {                                              \
            *error = "Vulkan 设备缺少入口: " #name;                              \
            return false;                                                       \
        }                                                                       \
    } while (false)

bool Renderer::LoadGlobalFunctions(std::string* error) {
    g_vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
        g_get_instance_proc(nullptr, "vkCreateInstance"));
    g_vkEnumerateInstanceExtensionProperties =
        reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
            g_get_instance_proc(nullptr, "vkEnumerateInstanceExtensionProperties"));
    if (g_vkCreateInstance == nullptr || g_vkEnumerateInstanceExtensionProperties == nullptr) {
        *error = "Vulkan 运行时缺少全局入口函数";
        return false;
    }
    return true;
}

bool Renderer::LoadInstanceFunctions(std::string* error) {
    LOAD_INSTANCE_FN(vkDestroyInstance);
    LOAD_INSTANCE_FN(vkEnumeratePhysicalDevices);
    LOAD_INSTANCE_FN(vkEnumerateDeviceExtensionProperties);
    LOAD_INSTANCE_FN(vkGetPhysicalDeviceProperties);
    LOAD_INSTANCE_FN(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_INSTANCE_FN(vkGetPhysicalDeviceSurfaceSupportKHR);
    LOAD_INSTANCE_FN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD_INSTANCE_FN(vkGetPhysicalDeviceSurfaceFormatsKHR);
    LOAD_INSTANCE_FN(vkCreateWin32SurfaceKHR);
    LOAD_INSTANCE_FN(vkDestroySurfaceKHR);
    LOAD_INSTANCE_FN(vkCreateDevice);
    LOAD_INSTANCE_FN(vkGetDeviceProcAddr);
    return true;
}

bool Renderer::LoadDeviceFunctions(std::string* error) {
    LOAD_DEVICE_FN(vkDestroyDevice);
    LOAD_DEVICE_FN(vkGetDeviceQueue);
    LOAD_DEVICE_FN(vkDeviceWaitIdle);
    LOAD_DEVICE_FN(vkCreateSwapchainKHR);
    LOAD_DEVICE_FN(vkDestroySwapchainKHR);
    LOAD_DEVICE_FN(vkGetSwapchainImagesKHR);
    LOAD_DEVICE_FN(vkAcquireNextImageKHR);
    LOAD_DEVICE_FN(vkQueuePresentKHR);
    LOAD_DEVICE_FN(vkQueueSubmit);
    LOAD_DEVICE_FN(vkCreateRenderPass);
    LOAD_DEVICE_FN(vkDestroyRenderPass);
    LOAD_DEVICE_FN(vkCreateImageView);
    LOAD_DEVICE_FN(vkDestroyImageView);
    LOAD_DEVICE_FN(vkCreateFramebuffer);
    LOAD_DEVICE_FN(vkDestroyFramebuffer);
    LOAD_DEVICE_FN(vkCreateCommandPool);
    LOAD_DEVICE_FN(vkDestroyCommandPool);
    LOAD_DEVICE_FN(vkAllocateCommandBuffers);
    LOAD_DEVICE_FN(vkBeginCommandBuffer);
    LOAD_DEVICE_FN(vkEndCommandBuffer);
    LOAD_DEVICE_FN(vkCmdBeginRenderPass);
    LOAD_DEVICE_FN(vkCmdEndRenderPass);
    LOAD_DEVICE_FN(vkCreateSemaphore);
    LOAD_DEVICE_FN(vkDestroySemaphore);
    LOAD_DEVICE_FN(vkCreateFence);
    LOAD_DEVICE_FN(vkDestroyFence);
    LOAD_DEVICE_FN(vkWaitForFences);
    LOAD_DEVICE_FN(vkResetFences);
    return true;
}

bool Renderer::PickPhysicalDevice(std::string* error) {
    uint32_t count = 0;
    if (g_vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0) {
        *error = "没有找到可用的 Vulkan 物理设备";
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    if (g_vkEnumeratePhysicalDevices(instance, &count, devices.data()) != VK_SUCCESS) {
        *error = "枚举 Vulkan 物理设备失败";
        return false;
    }

    int best_score = -1;
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties properties{};
        g_vkGetPhysicalDeviceProperties(candidate, &properties);

        uint32_t family_count = 0;
        g_vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        if (family_count == 0) {
            continue;
        }
        std::vector<VkQueueFamilyProperties> families(family_count);
        g_vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());

        uint32_t chosen_family = UINT32_MAX;
        for (uint32_t i = 0; i < family_count; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                continue;
            }
            VkBool32 present_supported = VK_FALSE;
            g_vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &present_supported);
            if (present_supported == VK_TRUE) {
                chosen_family = i;
                break;
            }
        }
        if (chosen_family == UINT32_MAX) {
            continue;
        }

        uint32_t extension_count = 0;
        g_vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extension_count, nullptr);
        std::vector<VkExtensionProperties> extensions(extension_count);
        if (extension_count > 0) {
            g_vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extension_count,
                                                   extensions.data());
        }
        bool has_swapchain = false;
        for (const VkExtensionProperties& extension : extensions) {
            if (std::strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                has_swapchain = true;
                break;
            }
        }
        if (!has_swapchain) {
            continue;
        }

        int score = 1;
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            score = 10;
        } else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            score = 5;
        } else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) {
            score = 3;
        }
        if (score > best_score) {
            best_score = score;
            physical_device = candidate;
            queue_family = chosen_family;
        }
    }

    if (physical_device == VK_NULL_HANDLE) {
        *error = "没有找到同时支持图形队列与窗口呈现的 Vulkan 设备";
        return false;
    }

    VkPhysicalDeviceProperties properties{};
    g_vkGetPhysicalDeviceProperties(physical_device, &properties);
    std::printf("显卡 %s（Vulkan %u.%u.%u）\n", properties.deviceName,
                VK_VERSION_MAJOR(properties.apiVersion),
                VK_VERSION_MINOR(properties.apiVersion),
                VK_VERSION_PATCH(properties.apiVersion));
    return true;
}

bool Renderer::CreateDeviceObject(std::string* error) {
    float priority = 1.0f;

    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    const char* extensions[1] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 1;
    device_info.ppEnabledExtensionNames = extensions;

    const VkResult result = g_vkCreateDevice(physical_device, &device_info, nullptr, &device);
    if (result != VK_SUCCESS) {
        *error = "创建 Vulkan 逻辑设备失败（" + ResultText(result) + "）";
        return false;
    }
    g_vkGetDeviceQueue(device, queue_family, 0, &queue);
    return true;
}

bool Renderer::CreateRenderPass(std::string* error) {
    VkAttachmentDescription attachment{};
    attachment.format = surface_format.format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_reference{};
    color_reference.attachment = 0;
    color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_reference;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    create_info.attachmentCount = 1;
    create_info.pAttachments = &attachment;
    create_info.subpassCount = 1;
    create_info.pSubpasses = &subpass;
    create_info.dependencyCount = 1;
    create_info.pDependencies = &dependency;

    const VkResult result = g_vkCreateRenderPass(device, &create_info, nullptr, &render_pass);
    if (result != VK_SUCCESS) {
        *error = "创建渲染通道失败（" + ResultText(result) + "）";
        return false;
    }
    return true;
}

bool Renderer::CreateCommandResources(std::string* error) {
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family;
    const VkResult result = g_vkCreateCommandPool(device, &pool_info, nullptr, &command_pool);
    if (result != VK_SUCCESS) {
        *error = "创建命令池失败（" + ResultText(result) + "）";
        return false;
    }

    VkCommandBufferAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.commandPool = command_pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = kFramesInFlight;
    command_buffers.resize(kFramesInFlight);
    if (g_vkAllocateCommandBuffers(device, &allocate_info, command_buffers.data()) != VK_SUCCESS) {
        *error = "分配命令缓冲失败";
        return false;
    }
    return true;
}

bool Renderer::CreateSyncObjects(std::string* error) {
    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkSemaphore available = VK_NULL_HANDLE;
        if (g_vkCreateSemaphore(device, &semaphore_info, nullptr, &available) != VK_SUCCESS) {
            *error = "创建图像可用信号量失败";
            return false;
        }
        image_available.push_back(available);

        VkSemaphore finished = VK_NULL_HANDLE;
        if (g_vkCreateSemaphore(device, &semaphore_info, nullptr, &finished) != VK_SUCCESS) {
            *error = "创建渲染完成信号量失败";
            return false;
        }
        render_finished.push_back(finished);

        VkFence fence = VK_NULL_HANDLE;
        if (g_vkCreateFence(device, &fence_info, nullptr, &fence) != VK_SUCCESS) {
            *error = "创建栅栏失败";
            return false;
        }
        in_flight.push_back(fence);
    }
    return true;
}

void Renderer::DestroySwapchainResources() {
    if (device != VK_NULL_HANDLE) {
        if (g_vkDestroyFramebuffer != nullptr) {
            for (VkFramebuffer framebuffer : framebuffers) {
                g_vkDestroyFramebuffer(device, framebuffer, nullptr);
            }
        }
        if (g_vkDestroyImageView != nullptr) {
            for (VkImageView view : image_views) {
                g_vkDestroyImageView(device, view, nullptr);
            }
        }
        if (g_vkDestroySwapchainKHR != nullptr && swapchain != VK_NULL_HANDLE) {
            g_vkDestroySwapchainKHR(device, swapchain, nullptr);
        }
    }
    framebuffers.clear();
    image_views.clear();
    images.clear();
    swapchain = VK_NULL_HANDLE;
}

bool Renderer::CreateSwapchain(std::string* error) {
    VkSurfaceCapabilitiesKHR capabilities{};
    const VkResult caps_result = g_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        physical_device, surface, &capabilities);
    if (caps_result != VK_SUCCESS) {
        *error = "查询窗口表面能力失败（" + ResultText(caps_result) + "）";
        return false;
    }

    uint32_t format_count = 0;
    g_vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &format_count, nullptr);
    if (format_count == 0) {
        *error = "窗口表面没有可用的像素格式";
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    g_vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &format_count, formats.data());

    if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
        surface_format.format = VK_FORMAT_B8G8R8A8_UNORM;
        surface_format.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    } else {
        surface_format = formats[0];
        const VkFormat wanted[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB,
                                   VK_FORMAT_R8G8B8A8_UNORM};
        bool found = false;
        for (VkFormat format : wanted) {
            for (const VkSurfaceFormatKHR& candidate : formats) {
                if (candidate.format == format &&
                    candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                    surface_format = candidate;
                    found = true;
                    break;
                }
            }
            if (found) {
                break;
            }
        }
    }

    if (capabilities.currentExtent.width != UINT32_MAX) {
        extent = capabilities.currentExtent;
    } else {
        RECT client{};
        GetClientRect(window, &client);
        uint32_t width = static_cast<uint32_t>(client.right - client.left);
        uint32_t height = static_cast<uint32_t>(client.bottom - client.top);
        if (width < capabilities.minImageExtent.width) {
            width = capabilities.minImageExtent.width;
        }
        if (width > capabilities.maxImageExtent.width) {
            width = capabilities.maxImageExtent.width;
        }
        if (height < capabilities.minImageExtent.height) {
            height = capabilities.minImageExtent.height;
        }
        if (height > capabilities.maxImageExtent.height) {
            height = capabilities.maxImageExtent.height;
        }
        extent.width = width;
        extent.height = height;
    }
    if (extent.width == 0 || extent.height == 0) {
        *error = "窗口尺寸为 0，无法创建交换链";
        return false;
    }

    image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
        image_count = capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create_info.surface = surface;
    create_info.minImageCount = image_count;
    create_info.imageFormat = surface_format.format;
    create_info.imageColorSpace = surface_format.colorSpace;
    create_info.imageExtent = extent;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create_info.preTransform = capabilities.currentTransform;
    create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    create_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    create_info.clipped = VK_TRUE;
    create_info.oldSwapchain = VK_NULL_HANDLE;

    const VkResult result = g_vkCreateSwapchainKHR(device, &create_info, nullptr, &swapchain);
    if (result != VK_SUCCESS) {
        *error = "创建交换链失败（" + ResultText(result) + "）";
        return false;
    }

    uint32_t actual_count = 0;
    g_vkGetSwapchainImagesKHR(device, swapchain, &actual_count, nullptr);
    if (actual_count == 0) {
        *error = "交换链没有任何图像";
        return false;
    }
    images.resize(actual_count);
    g_vkGetSwapchainImagesKHR(device, swapchain, &actual_count, images.data());
    image_count = actual_count;

    image_views.resize(actual_count);
    framebuffers.resize(actual_count);

    for (uint32_t i = 0; i < actual_count; ++i) {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = images[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = surface_format.format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;
        const VkResult view_result =
            g_vkCreateImageView(device, &view_info, nullptr, &image_views[i]);
        if (view_result != VK_SUCCESS) {
            *error = "创建交换链图像视图失败（" + ResultText(view_result) + "）";
            return false;
        }

        VkFramebufferCreateInfo framebuffer_info{};
        framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer_info.renderPass = render_pass;
        framebuffer_info.attachmentCount = 1;
        framebuffer_info.pAttachments = &image_views[i];
        framebuffer_info.width = extent.width;
        framebuffer_info.height = extent.height;
        framebuffer_info.layers = 1;
        const VkResult framebuffer_result =
            g_vkCreateFramebuffer(device, &framebuffer_info, nullptr, &framebuffers[i]);
        if (framebuffer_result != VK_SUCCESS) {
            *error = "创建帧缓冲失败（" + ResultText(framebuffer_result) + "）";
            return false;
        }
    }
    return true;
}

bool Renderer::RecreateSwapchain(std::string* error) {
    if (g_vkDeviceWaitIdle(device) != VK_SUCCESS) {
        *error = "等待设备空闲失败";
        return false;
    }
    DestroySwapchainResources();
    return CreateSwapchain(error);
}

bool Renderer::Init(HWND target, std::string* error) {
    window = target;
    hinstance = GetModuleHandleW(nullptr);

    loader = LoadLibraryW(L"vulkan-1.dll");
    if (loader == nullptr) {
        *error = "无法加载 vulkan-1.dll（请安装显卡驱动或 Vulkan 运行时）";
        return false;
    }
    g_get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(loader, "vkGetInstanceProcAddr"));
    if (g_get_instance_proc == nullptr) {
        *error = "vulkan-1.dll 中找不到 vkGetInstanceProcAddr";
        return false;
    }
    if (!LoadGlobalFunctions(error)) {
        return false;
    }

    uint32_t extension_count = 0;
    g_vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> extensions(extension_count);
    if (extension_count > 0) {
        g_vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, extensions.data());
    }
    bool has_surface = false;
    bool has_win32_surface = false;
    for (const VkExtensionProperties& extension : extensions) {
        if (std::strcmp(extension.extensionName, VK_KHR_SURFACE_EXTENSION_NAME) == 0) {
            has_surface = true;
        }
        if (std::strcmp(extension.extensionName, VK_KHR_WIN32_SURFACE_EXTENSION_NAME) == 0) {
            has_win32_surface = true;
        }
    }
    if (!has_surface || !has_win32_surface) {
        *error = "Vulkan 运行时缺少 VK_KHR_surface / VK_KHR_win32_surface 扩展";
        return false;
    }

    VkApplicationInfo application_info{};
    application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application_info.pApplicationName = "liveview";
    application_info.applicationVersion = 1;
    application_info.pEngineName = "liveview";
    application_info.engineVersion = 1;
    application_info.apiVersion = VK_API_VERSION_1_0;

    const char* enabled_extensions[2] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                         VK_KHR_WIN32_SURFACE_EXTENSION_NAME};

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application_info;
    instance_info.enabledExtensionCount = 2;
    instance_info.ppEnabledExtensionNames = enabled_extensions;

    const VkResult instance_result = g_vkCreateInstance(&instance_info, nullptr, &instance);
    if (instance_result != VK_SUCCESS) {
        *error = "创建 Vulkan 实例失败（" + ResultText(instance_result) +
                 "），请确认显卡驱动支持 Vulkan";
        return false;
    }
    if (!LoadInstanceFunctions(error)) {
        return false;
    }

    VkWin32SurfaceCreateInfoKHR surface_info{};
    surface_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surface_info.hinstance = hinstance;
    surface_info.hwnd = window;
    const VkResult surface_result =
        g_vkCreateWin32SurfaceKHR(instance, &surface_info, nullptr, &surface);
    if (surface_result != VK_SUCCESS) {
        *error = "创建 Win32 窗口表面失败（" + ResultText(surface_result) + "）";
        return false;
    }

    if (!PickPhysicalDevice(error)) {
        return false;
    }
    if (!CreateDeviceObject(error)) {
        return false;
    }
    if (!LoadDeviceFunctions(error)) {
        return false;
    }
    if (!CreateRenderPass(error)) {
        return false;
    }
    if (!CreateCommandResources(error)) {
        return false;
    }
    if (!CreateSyncObjects(error)) {
        return false;
    }
    if (!CreateSwapchain(error)) {
        return false;
    }
    return true;
}

void Renderer::Shutdown() {
    if (device != VK_NULL_HANDLE) {
        if (g_vkDeviceWaitIdle != nullptr) {
            g_vkDeviceWaitIdle(device);
        }
        DestroySwapchainResources();
        if (g_vkDestroyRenderPass != nullptr && render_pass != VK_NULL_HANDLE) {
            g_vkDestroyRenderPass(device, render_pass, nullptr);
        }
        if (g_vkDestroySemaphore != nullptr) {
            for (VkSemaphore semaphore : image_available) {
                g_vkDestroySemaphore(device, semaphore, nullptr);
            }
            for (VkSemaphore semaphore : render_finished) {
                g_vkDestroySemaphore(device, semaphore, nullptr);
            }
        }
        if (g_vkDestroyFence != nullptr) {
            for (VkFence fence : in_flight) {
                g_vkDestroyFence(device, fence, nullptr);
            }
        }
        if (g_vkDestroyCommandPool != nullptr && command_pool != VK_NULL_HANDLE) {
            g_vkDestroyCommandPool(device, command_pool, nullptr);
        }
        if (g_vkDestroyDevice != nullptr) {
            g_vkDestroyDevice(device, nullptr);
        }
    }
    image_available.clear();
    render_finished.clear();
    in_flight.clear();
    command_buffers.clear();
    command_pool = VK_NULL_HANDLE;
    render_pass = VK_NULL_HANDLE;
    device = VK_NULL_HANDLE;
    queue = VK_NULL_HANDLE;

    if (instance != VK_NULL_HANDLE) {
        if (g_vkDestroySurfaceKHR != nullptr && surface != VK_NULL_HANDLE) {
            g_vkDestroySurfaceKHR(instance, surface, nullptr);
        }
        if (g_vkDestroyInstance != nullptr) {
            g_vkDestroyInstance(instance, nullptr);
        }
    }
    surface = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;

    if (loader != nullptr) {
        FreeLibrary(loader);
        loader = nullptr;
    }
    g_get_instance_proc = nullptr;
}

bool Renderer::RecordCommandBuffer(uint32_t image_index, const float* color) {
    VkCommandBuffer command = command_buffers[frame_index];

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (g_vkBeginCommandBuffer(command, &begin_info) != VK_SUCCESS) {
        return false;
    }

    VkClearValue clear_value{};
    clear_value.color.float32[0] = color[0];
    clear_value.color.float32[1] = color[1];
    clear_value.color.float32[2] = color[2];
    clear_value.color.float32[3] = 1.0f;

    VkRenderPassBeginInfo pass_info{};
    pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass_info.renderPass = render_pass;
    pass_info.framebuffer = framebuffers[image_index];
    pass_info.renderArea.offset.x = 0;
    pass_info.renderArea.offset.y = 0;
    pass_info.renderArea.extent = extent;
    pass_info.clearValueCount = 1;
    pass_info.pClearValues = &clear_value;

    g_vkCmdBeginRenderPass(command, &pass_info, VK_SUBPASS_CONTENTS_INLINE);
    g_vkCmdEndRenderPass(command);
    return g_vkEndCommandBuffer(command) == VK_SUCCESS;
}

bool Renderer::DrawFrame(const float* color, std::string* error) {
    if (g_window_resized) {
        g_window_resized = false;
        RECT client{};
        GetClientRect(window, &client);
        if (client.right - client.left > 0 && client.bottom - client.top > 0) {
            if (!RecreateSwapchain(error)) {
                return false;
            }
        }
    }

    if (g_vkWaitForFences(device, 1, &in_flight[frame_index], VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        *error = "等待帧栅栏失败";
        return false;
    }

    uint32_t image_index = 0;
    const VkResult acquire_result = g_vkAcquireNextImageKHR(
        device, swapchain, UINT64_MAX, image_available[frame_index], VK_NULL_HANDLE, &image_index);
    if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
        return RecreateSwapchain(error);
    }
    if (acquire_result != VK_SUCCESS && acquire_result != VK_SUBOPTIMAL_KHR) {
        *error = "获取交换链图像失败（" + ResultText(acquire_result) + "）";
        return false;
    }

    if (g_vkResetFences(device, 1, &in_flight[frame_index]) != VK_SUCCESS) {
        *error = "重置帧栅栏失败";
        return false;
    }

    if (!RecordCommandBuffer(image_index, color)) {
        *error = "录制命令缓冲失败";
        return false;
    }

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &image_available[frame_index];
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    submit_info.pWaitDstStageMask = &wait_stage;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffers[frame_index];
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &render_finished[frame_index];

    const VkResult submit_result =
        g_vkQueueSubmit(queue, 1, &submit_info, in_flight[frame_index]);
    if (submit_result != VK_SUCCESS) {
        *error = "提交命令失败（" + ResultText(submit_result) + "）";
        return false;
    }

    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &render_finished[frame_index];
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain;
    present_info.pImageIndices = &image_index;

    const VkResult present_result = g_vkQueuePresentKHR(queue, &present_info);

    frame_index = (frame_index + 1) % kFramesInFlight;

    if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR) {
        return RecreateSwapchain(error);
    }
    if (present_result != VK_SUCCESS) {
        *error = "呈现失败（" + ResultText(present_result) + "）";
        return false;
    }
    return true;
}

}  // namespace

int RunRender(const Bundle& bundle, double seconds) {
    if (bundle.tick_fps <= 0.0) {
        std::fprintf(stderr, "包内 tick 非法，无法播放\n");
        return 1;
    }
    if (bundle.range_end <= bundle.range_start) {
        std::fprintf(stderr, "导出范围为空，无法播放\n");
        return 1;
    }

    double duration = seconds;
    if (duration < 0.0) {
        duration = static_cast<double>(bundle.range_end - bundle.range_start) / bundle.tick_fps;
    }
    if (duration <= 0.0) {
        std::fprintf(stderr, "播放时长必须大于 0\n");
        return 1;
    }

    std::string error;
    if (!CreateRenderWindow(&error)) {
        MessageBoxA(nullptr, error.c_str(), "liveview", MB_OK | MB_ICONERROR);
        return 1;
    }

    Renderer renderer;
    if (!renderer.Init(g_window, &error)) {
        MessageBoxA(g_window, error.c_str(), "liveview", MB_OK | MB_ICONERROR);
        renderer.Shutdown();
        DestroyWindow(g_window);
        return 1;
    }

    std::printf("播放 %.2f 秒（tick %.0f fps）；ESC 或关闭窗口结束\n", duration,
                bundle.tick_fps);
    std::fflush(stdout);

    LARGE_INTEGER frequency{};
    LARGE_INTEGER start{};
    LARGE_INTEGER now{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);

    MSG message{};
    double title_time = -1.0;
    float color[3] = {0.2f, 0.2f, 0.2f};
    bool running = true;

    while (running) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!running) {
            break;
        }

        QueryPerformanceCounter(&now);
        const double elapsed = static_cast<double>(now.QuadPart - start.QuadPart) /
                              static_cast<double>(frequency.QuadPart);
        if (elapsed >= duration) {
            break;
        }

        RECT client{};
        GetClientRect(g_window, &client);
        if (client.right - client.left <= 0 || client.bottom - client.top <= 0) {
            Sleep(10);
            continue;
        }

        const double frame =
            static_cast<double>(bundle.range_start) + elapsed * bundle.tick_fps;
        const CameraKey pose = SampleCamera(bundle, frame);

        double direction[3] = {pose.target[0] - pose.position[0],
                               pose.target[1] - pose.position[1],
                               pose.target[2] - pose.position[2]};
        const double length =
            std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] +
                      direction[2] * direction[2]);
        const double scale = length > 1e-6 ? 0.35 / length : 0.0;
        color[0] = static_cast<float>(0.5 + direction[0] * scale);
        color[1] = static_cast<float>(0.5 + direction[1] * scale);
        color[2] = static_cast<float>(0.5 + direction[2] * scale);

        if (!renderer.DrawFrame(color, &error)) {
            MessageBoxA(g_window, error.c_str(), "liveview", MB_OK | MB_ICONERROR);
            break;
        }

        if (elapsed - title_time >= 0.25) {
            title_time = elapsed;
            const std::string title =
                "liveview frame " + std::to_string(static_cast<long long>(frame)) + "  " +
                FormatPose(pose);
            SetWindowTextA(g_window, title.c_str());
        }
    }

    renderer.Shutdown();

    if (g_window != nullptr && IsWindow(g_window)) {
        DestroyWindow(g_window);
    }
    return 0;
}

}  // namespace liveview
