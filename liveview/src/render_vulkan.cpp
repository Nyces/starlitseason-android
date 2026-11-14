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

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "render_vulkan.h"

#include "config.h"
#include "log.h"
#include "scene_frag_spirv.h"
#include "scene_vert_spirv.h"
#include "show.h"

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
PFN_vkGetPhysicalDeviceMemoryProperties g_vkGetPhysicalDeviceMemoryProperties = nullptr;
PFN_vkGetPhysicalDeviceQueueFamilyProperties g_vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
PFN_vkGetPhysicalDeviceSurfaceSupportKHR g_vkGetPhysicalDeviceSurfaceSupportKHR = nullptr;
PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR g_vkGetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
PFN_vkGetPhysicalDeviceSurfaceFormatsKHR g_vkGetPhysicalDeviceSurfaceFormatsKHR = nullptr;
PFN_vkGetPhysicalDeviceSurfacePresentModesKHR g_vkGetPhysicalDeviceSurfacePresentModesKHR =
    nullptr;
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

// 网格渲染需要的入口
PFN_vkCreateBuffer g_vkCreateBuffer = nullptr;
PFN_vkDestroyBuffer g_vkDestroyBuffer = nullptr;
PFN_vkGetBufferMemoryRequirements g_vkGetBufferMemoryRequirements = nullptr;
PFN_vkBindBufferMemory g_vkBindBufferMemory = nullptr;
PFN_vkCreateImage g_vkCreateImage = nullptr;
PFN_vkDestroyImage g_vkDestroyImage = nullptr;
PFN_vkGetImageMemoryRequirements g_vkGetImageMemoryRequirements = nullptr;
PFN_vkBindImageMemory g_vkBindImageMemory = nullptr;
PFN_vkAllocateMemory g_vkAllocateMemory = nullptr;
PFN_vkFreeMemory g_vkFreeMemory = nullptr;
PFN_vkMapMemory g_vkMapMemory = nullptr;
PFN_vkUnmapMemory g_vkUnmapMemory = nullptr;
PFN_vkCreateShaderModule g_vkCreateShaderModule = nullptr;
PFN_vkDestroyShaderModule g_vkDestroyShaderModule = nullptr;
PFN_vkCreateDescriptorSetLayout g_vkCreateDescriptorSetLayout = nullptr;
PFN_vkDestroyDescriptorSetLayout g_vkDestroyDescriptorSetLayout = nullptr;
PFN_vkCreatePipelineLayout g_vkCreatePipelineLayout = nullptr;
PFN_vkDestroyPipelineLayout g_vkDestroyPipelineLayout = nullptr;
PFN_vkCreateGraphicsPipelines g_vkCreateGraphicsPipelines = nullptr;
PFN_vkDestroyPipeline g_vkDestroyPipeline = nullptr;
PFN_vkCreateDescriptorPool g_vkCreateDescriptorPool = nullptr;
PFN_vkDestroyDescriptorPool g_vkDestroyDescriptorPool = nullptr;
PFN_vkAllocateDescriptorSets g_vkAllocateDescriptorSets = nullptr;
PFN_vkUpdateDescriptorSets g_vkUpdateDescriptorSets = nullptr;
PFN_vkCmdBindPipeline g_vkCmdBindPipeline = nullptr;
PFN_vkCmdBindVertexBuffers g_vkCmdBindVertexBuffers = nullptr;
PFN_vkCmdBindIndexBuffer g_vkCmdBindIndexBuffer = nullptr;
PFN_vkCmdBindDescriptorSets g_vkCmdBindDescriptorSets = nullptr;
PFN_vkCmdDrawIndexed g_vkCmdDrawIndexed = nullptr;
PFN_vkCmdSetViewport g_vkCmdSetViewport = nullptr;
PFN_vkCmdSetScissor g_vkCmdSetScissor = nullptr;
PFN_vkCmdPipelineBarrier g_vkCmdPipelineBarrier = nullptr;
PFN_vkCmdCopyImageToBuffer g_vkCmdCopyImageToBuffer = nullptr;
PFN_vkCmdCopyBufferToImage g_vkCmdCopyBufferToImage = nullptr;
PFN_vkCmdPushConstants g_vkCmdPushConstants = nullptr;
PFN_vkCreateSampler g_vkCreateSampler = nullptr;
PFN_vkDestroySampler g_vkDestroySampler = nullptr;

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

// 每一步都先记 checkpoint：崩溃时日志最后一行就能指出死在哪
#define STEP(name) SetCheckpoint(name)
#define LOG_STEP(...) Logf(__VA_ARGS__)

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

// ---- 仿射变换（3×3 + 平移），CPU 蒙皮用 ----

struct Affine {
    float a[9];   // 行主序 3×3：p' = a · p
    float t[3];
};

Affine IdentityAffine() {
    Affine m{};
    m.a[0] = 1.0f;
    m.a[4] = 1.0f;
    m.a[8] = 1.0f;
    return m;
}

Affine Multiply(const Affine& left, const Affine& right) {
    Affine out{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            float sum = 0.0f;
            for (int k = 0; k < 3; ++k) {
                sum += left.a[row * 3 + k] * right.a[k * 3 + column];
            }
            out.a[row * 3 + column] = sum;
        }
        float translation = left.t[row];
        for (int k = 0; k < 3; ++k) {
            translation += left.a[row * 3 + k] * right.t[k];
        }
        out.t[row] = translation;
    }
    return out;
}

Affine Inverse(const Affine& m) {
    const float a0 = m.a[0], a1 = m.a[1], a2 = m.a[2];
    const float a3 = m.a[3], a4 = m.a[4], a5 = m.a[5];
    const float a6 = m.a[6], a7 = m.a[7], a8 = m.a[8];
    const float det = a0 * (a4 * a8 - a5 * a7) - a1 * (a3 * a8 - a5 * a6) +
                      a2 * (a3 * a7 - a4 * a6);
    if (std::fabs(det) < 1e-12f) {
        return IdentityAffine();
    }
    const float inv_det = 1.0f / det;
    Affine out{};
    out.a[0] = (a4 * a8 - a5 * a7) * inv_det;
    out.a[1] = (a2 * a7 - a1 * a8) * inv_det;
    out.a[2] = (a1 * a5 - a2 * a4) * inv_det;
    out.a[3] = (a5 * a6 - a3 * a8) * inv_det;
    out.a[4] = (a0 * a8 - a2 * a6) * inv_det;
    out.a[5] = (a2 * a3 - a0 * a5) * inv_det;
    out.a[6] = (a3 * a7 - a4 * a6) * inv_det;
    out.a[7] = (a1 * a6 - a0 * a7) * inv_det;
    out.a[8] = (a0 * a4 - a1 * a3) * inv_det;
    for (int row = 0; row < 3; ++row) {
        out.t[row] = -(out.a[row * 3 + 0] * m.t[0] + out.a[row * 3 + 1] * m.t[1] +
                       out.a[row * 3 + 2] * m.t[2]);
    }
    return out;
}

void QuatToMatrix(const float q[4], float out[9]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    out[0] = 1.0f - 2.0f * (y * y + z * z);
    out[1] = 2.0f * (x * y - z * w);
    out[2] = 2.0f * (x * z + y * w);
    out[3] = 2.0f * (x * y + z * w);
    out[4] = 1.0f - 2.0f * (x * x + z * z);
    out[5] = 2.0f * (y * z - x * w);
    out[6] = 2.0f * (x * z - y * w);
    out[7] = 2.0f * (y * z + x * w);
    out[8] = 1.0f - 2.0f * (x * x + y * y);
}

// 与 Python 侧 _transform_matrix 一致：旋转矩阵按列乘缩放。
Affine FromBone(const float quat[4], const float translation[3], const float scale[3]) {
    Affine out{};
    float rotation[9];
    QuatToMatrix(quat, rotation);
    for (int row = 0; row < 3; ++row) {
        out.a[row * 3 + 0] = rotation[row * 3 + 0] * scale[0];
        out.a[row * 3 + 1] = rotation[row * 3 + 1] * scale[1];
        out.a[row * 3 + 2] = rotation[row * 3 + 2] * scale[2];
        out.t[row] = translation[row];
    }
    return out;
}

void TransformPoint(const Affine& m, const float p[3], float out[3]) {
    for (int row = 0; row < 3; ++row) {
        out[row] = m.a[row * 3 + 0] * p[0] + m.a[row * 3 + 1] * p[1] +
                   m.a[row * 3 + 2] * p[2] + m.t[row];
    }
}

void TransformDirection(const Affine& m, const float n[3], float out[3]) {
    for (int row = 0; row < 3; ++row) {
        out[row] = m.a[row * 3 + 0] * n[0] + m.a[row * 3 + 1] * n[1] +
                   m.a[row * 3 + 2] * n[2];
    }
}

// ---- 4×4 矩阵（列主序），相机用 ----

struct Mat4 {
    float m[16];
};

Mat4 MultiplyMat4(const Mat4& left, const Mat4& right) {
    Mat4 out{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += left.m[k * 4 + row] * right.m[column * 4 + k];
            }
            out.m[column * 4 + row] = sum;
        }
    }
    return out;
}

// 列主序矩阵：m[col * 4 + row]。
// Vulkan 的裁剪空间是 0 ≤ z ≤ w、Y 轴朝下，所以 z 的常量项与 w 的 -1 不能写反：
//   z_clip = far/(near-far) * z_view + far*near/(near-far)
//   w_clip = -z_view
// 之前把这两项写反，导致 z_ndc ≈ -0.32（负深度），整幅画面被近平面裁掉 —— 全黑。
Mat4 PerspectiveVulkan(float fov_degrees, float aspect, float near_plane, float far_plane) {
    Mat4 out{};
    const float f = 1.0f / std::tan(fov_degrees * 3.14159265358979323846f / 360.0f);
    out.m[0] = f / (aspect > 0.0f ? aspect : 1.0f);
    out.m[5] = -f;   // Vulkan 的 Y 轴朝下，这里翻一次
    out.m[10] = far_plane / (near_plane - far_plane);
    out.m[11] = -1.0f;
    out.m[14] = far_plane * near_plane / (near_plane - far_plane);
    out.m[15] = 0.0f;
    return out;
}

Mat4 LookAtVulkan(const float eye[3], const float target[3]) {
    float forward[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    const float length = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] +
                                   forward[2] * forward[2]);
    if (length > 1e-8f) {
        forward[0] /= length;
        forward[1] /= length;
        forward[2] /= length;
    } else {
        forward[0] = 1.0f;
    }
    const float up[3] = {0.0f, 0.0f, 1.0f};
    float right[3] = {forward[1] * up[2] - forward[2] * up[1],
                      forward[2] * up[0] - forward[0] * up[2],
                      forward[0] * up[1] - forward[1] * up[0]};
    const float right_length = std::sqrt(right[0] * right[0] + right[1] * right[1] +
                                         right[2] * right[2]);
    if (right_length > 1e-8f) {
        right[0] /= right_length;
        right[1] /= right_length;
        right[2] /= right_length;
    } else {
        right[0] = 1.0f;
    }
    const float true_up[3] = {right[1] * forward[2] - right[2] * forward[1],
                              right[2] * forward[0] - right[0] * forward[2],
                              right[0] * forward[1] - right[1] * forward[0]};

    Mat4 out{};
    out.m[0] = right[0];
    out.m[4] = right[1];
    out.m[8] = right[2];
    out.m[12] = -(right[0] * eye[0] + right[1] * eye[1] + right[2] * eye[2]);
    out.m[1] = true_up[0];
    out.m[5] = true_up[1];
    out.m[9] = true_up[2];
    out.m[13] = -(true_up[0] * eye[0] + true_up[1] * eye[1] + true_up[2] * eye[2]);
    out.m[2] = -forward[0];
    out.m[6] = -forward[1];
    out.m[10] = -forward[2];
    out.m[14] = forward[0] * eye[0] + forward[1] * eye[1] + forward[2] * eye[2];
    out.m[15] = 1.0f;
    return out;
}

// 与着色器一致的矩阵作用：out = M · p（M 按列主序存放）。
void ApplyMat4(const Mat4& m, const float in[4], float out[4]) {
    for (int row = 0; row < 4; ++row) {
        out[row] = m.m[row] * in[0] + m.m[4 + row] * in[1] + m.m[8 + row] * in[2] +
                   m.m[12 + row] * in[3];
    }
}

double NowSeconds() {
    static LARGE_INTEGER frequency{};
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / static_cast<double>(frequency.QuadPart);
}

// 与着色器里的 CameraBlock 一致（std140）。
struct CameraUniform {
    float view_projection[16];
    float view[16];
    float light_direction[4];
    float eye_position[4];
};

// 与 scene.frag 里的 MaterialBlock 一致（push constant，96 字节）。
struct MaterialPush {
    float shadow_tint[4];   // rgb 阴影色，w 明暗阈值
    float blend[4];         // rgb 配色乘，w 边缘光指数
    float add[4];           // rgb 配色加，w 阴影强度
    float sphere[4];        // rgb 球面贴图乘色，w 取幂
    float params[4];        // x 无光照，y 抠像阈值，z 球面强度，w 有球面贴图
    float outline[4];       // x 外扩宽度（0 = 不扩）
};

// 一个材质在 GPU 上的资源：描述符集（内部引用贴图视图）。
struct MaterialGpu {
    VkDescriptorSet set = VK_NULL_HANDLE;
};

// 提前声明：离屏截图目标在 CreateShowPipeline 里创建，而实现放在文件后半部。
bool CreateHostBuffer(VkDevice device, VkPhysicalDevice physical, VkDeviceSize size,
                      VkBufferUsageFlags usage, VkBuffer* buffer, VkDeviceMemory* memory,
                      void** mapped, const char* what, std::string* error);

uint32_t FindMemoryType(VkPhysicalDevice device, uint32_t type_bits,
                        VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memory{};
    g_vkGetPhysicalDeviceMemoryProperties(device, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) != 0 &&
            (memory.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

// 一段绘制：索引缓冲里的区间 + 用哪套顶点缓冲。
struct DrawRange {
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    int32_t vertex_offset = 0;
    bool skinned = false;
    int32_t material = -1;
    int32_t pass = 0;   // 0 不透明/抠像，1 半透明，2 描边
};

struct SkinCpu {
    std::vector<int32_t> anim_bone;     // 网格骨骼 -> 动画骨骼下标（-1 表示不驱动）
    std::vector<Affine> bind_global;    // 绑定姿势全局矩阵
    std::vector<Affine> bind_inverse;   // 绑定姿势全局矩阵的逆
    Affine attach_model;                // 头发/饰品每帧更新的跟随矩阵
    uint32_t vertex_offset = 0;         // 在动态顶点缓冲里的起始顶点
    uint32_t vertex_count = 0;
};

// 演出包在 GPU 上的资源（全部 host-visible，量小，省掉暂存拷贝）。
struct ShowGpu {
    VkBuffer stage_vertices = VK_NULL_HANDLE;
    VkDeviceMemory stage_memory = VK_NULL_HANDLE;
    void* stage_mapped = nullptr;
    size_t stage_vertex_count = 0;

    VkBuffer skin_vertices = VK_NULL_HANDLE;
    VkDeviceMemory skin_memory = VK_NULL_HANDLE;
    void* skin_mapped = nullptr;
    size_t skin_vertex_count = 0;

    VkBuffer indices = VK_NULL_HANDLE;
    VkDeviceMemory index_memory = VK_NULL_HANDLE;
    void* index_mapped = nullptr;
    size_t index_count = 0;

    std::vector<DrawRange> passes[3];   // 0 不透明/抠像，1 半透明，2 描边
    std::vector<SkinCpu> skins;
    int32_t head_skin = -1;      // 提供 SO_Head 的蒙皮网格
    int32_t head_bone = -1;
    float bounds_lo[3] = {0.0f, 0.0f, 0.0f};
    float bounds_hi[3] = {0.0f, 0.0f, 0.0f};

    bool Upload(VkDevice device, VkPhysicalDevice physical, const Show& show,
                std::string* error);
    void Destroy(VkDevice device);
    void Update(const Show& show, double seconds);
};

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

    // 深度缓冲（跟随交换链重建）
    VkImage depth_image = VK_NULL_HANDLE;
    VkDeviceMemory depth_memory = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;

    // 网格渲染管线
    VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipeline pipeline_blended = VK_NULL_HANDLE;   // 半透明/叠加材质
    VkPipeline pipeline_outline = VK_NULL_HANDLE;   // 描边壳：正面剔除 + 法线外扩
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkBuffer camera_buffer = VK_NULL_HANDLE;
    VkDeviceMemory camera_memory = VK_NULL_HANDLE;
    void* camera_mapped = nullptr;

    // 材质资源（贴图 + 描述符集），演出包上传后创建
    std::vector<MaterialGpu> materials_gpu;
    std::vector<MaterialPush> material_push;   // 每个材质的 push constant 预烘焙值
    std::vector<VkImage> texture_images;
    std::vector<VkDeviceMemory> texture_memory;
    std::vector<VkImageView> texture_views;
    VkSampler sampler = VK_NULL_HANDLE;
    // 材质缺贴图时的 1×1 白色占位
    VkImage white_image = VK_NULL_HANDLE;
    VkDeviceMemory white_memory = VK_NULL_HANDLE;
    VkImageView white_view = VK_NULL_HANDLE;

    ShowGpu show_gpu;
    uint64_t frames_drawn = 0;   // 用于节流诊断日志

    // 着色器模块与离屏截图目标
    VkShaderModule vertex_module = VK_NULL_HANDLE;
    VkShaderModule fragment_module = VK_NULL_HANDLE;
    VkRenderPass offscreen_pass = VK_NULL_HANDLE;
    VkPipeline offscreen_pipeline = VK_NULL_HANDLE;
    VkPipeline offscreen_blended = VK_NULL_HANDLE;
    VkPipeline offscreen_outline = VK_NULL_HANDLE;
    VkImage offscreen_color = VK_NULL_HANDLE;
    VkDeviceMemory offscreen_color_memory = VK_NULL_HANDLE;
    VkImageView offscreen_color_view = VK_NULL_HANDLE;
    VkImage offscreen_depth = VK_NULL_HANDLE;
    VkDeviceMemory offscreen_depth_memory = VK_NULL_HANDLE;
    VkImageView offscreen_depth_view = VK_NULL_HANDLE;
    VkFramebuffer offscreen_framebuffer = VK_NULL_HANDLE;
    VkBuffer readback_buffer = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory = VK_NULL_HANDLE;
    void* readback_mapped = nullptr;

    // 把某一时刻渲染成图片（不依赖窗口，用于在没有画面的机器上定位问题）
    bool CaptureFrame(const Show& show, double seconds, const std::wstring& path,
                      std::string* error);

    // 只渲染到离屏图像并等 GPU 完成，像素留在 readback_mapped 里
    bool RenderOffscreenFrame(const Show& show, double seconds, std::string* error);
    // 把 readback 里的像素贴到窗口（GDI），用于显卡不接显示器的情况
    bool BlitToWindow(const Show& show, double seconds, std::string* error);
    // readback（可能是 RGBA/BGRA）转成 BGRA 自顶向下
    void CopyReadbackToBgra(std::vector<uint8_t>* out) const;

    // 选中的显卡是否真的接着显示器；不是就改用「离屏 + GDI 上屏」
    bool device_on_display = false;
    bool present_via_gdi = false;
    std::vector<uint8_t> pixel_buffer;   // GDI 上屏用的 BGRA 像素

    bool Init(HWND target, std::string* error);
    void Shutdown();
    bool DrawFrame(const float* color, std::string* error);
    bool DrawShowFrame(const Show& show, double seconds, std::string* error);
    // 上传材质贴图并分配描述符集（演出包 Upload 之后调用）
    bool CreateMaterialResources(const Show& show, std::string* error);

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
    bool CreateDepthResources(std::string* error);
    void DestroyDepthResources();
    bool CreateShowPipeline(std::string* error);
    // 按渲染通道建三套管线：不透明、混合、描边
    bool CreateShowPipelines(VkRenderPass pass, VkPipeline* opaque, VkPipeline* blended,
                             VkPipeline* outline, std::string* error);
    bool CreateTextureImage(const ShowTexture& texture, VkImage* image,
                            VkDeviceMemory* memory, VkImageView* view, std::string* error);
    void DestroyMaterialResources();
    // 按 pass 顺序画舞台 + 角色（pipelines[0] 不透明，[1] 混合，[2] 描边）
    void DrawShowGeometry(VkCommandBuffer command, const VkPipeline* pipelines);
    bool CreateOffscreenTarget(std::string* error);
    void DestroyOffscreenTarget();
    void DestroyShowPipeline();
    bool CreateCameraBuffer(std::string* error);
    void DestroyCameraBuffer();
    bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* buffer,
                      VkDeviceMemory* memory, void** mapped, const char* what,
                      std::string* error);
    void DestroyBuffer(VkBuffer* buffer, VkDeviceMemory* memory, void** mapped);
    bool RecordShowCommandBuffer(uint32_t image_index);
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
    LOAD_INSTANCE_FN(vkGetPhysicalDeviceMemoryProperties);
    LOAD_INSTANCE_FN(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_INSTANCE_FN(vkGetPhysicalDeviceSurfaceSupportKHR);
    LOAD_INSTANCE_FN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD_INSTANCE_FN(vkGetPhysicalDeviceSurfaceFormatsKHR);
    LOAD_INSTANCE_FN(vkGetPhysicalDeviceSurfacePresentModesKHR);
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

    LOAD_DEVICE_FN(vkCreateBuffer);
    LOAD_DEVICE_FN(vkDestroyBuffer);
    LOAD_DEVICE_FN(vkGetBufferMemoryRequirements);
    LOAD_DEVICE_FN(vkBindBufferMemory);
    LOAD_DEVICE_FN(vkCreateImage);
    LOAD_DEVICE_FN(vkDestroyImage);
    LOAD_DEVICE_FN(vkGetImageMemoryRequirements);
    LOAD_DEVICE_FN(vkBindImageMemory);
    LOAD_DEVICE_FN(vkAllocateMemory);
    LOAD_DEVICE_FN(vkFreeMemory);
    LOAD_DEVICE_FN(vkMapMemory);
    LOAD_DEVICE_FN(vkUnmapMemory);
    LOAD_DEVICE_FN(vkCreateShaderModule);
    LOAD_DEVICE_FN(vkDestroyShaderModule);
    LOAD_DEVICE_FN(vkCreateDescriptorSetLayout);
    LOAD_DEVICE_FN(vkDestroyDescriptorSetLayout);
    LOAD_DEVICE_FN(vkCreatePipelineLayout);
    LOAD_DEVICE_FN(vkDestroyPipelineLayout);
    LOAD_DEVICE_FN(vkCreateGraphicsPipelines);
    LOAD_DEVICE_FN(vkDestroyPipeline);
    LOAD_DEVICE_FN(vkCreateDescriptorPool);
    LOAD_DEVICE_FN(vkDestroyDescriptorPool);
    LOAD_DEVICE_FN(vkAllocateDescriptorSets);
    LOAD_DEVICE_FN(vkUpdateDescriptorSets);
    LOAD_DEVICE_FN(vkCmdBindPipeline);
    LOAD_DEVICE_FN(vkCmdBindVertexBuffers);
    LOAD_DEVICE_FN(vkCmdBindIndexBuffer);
    LOAD_DEVICE_FN(vkCmdBindDescriptorSets);
    LOAD_DEVICE_FN(vkCmdDrawIndexed);
    LOAD_DEVICE_FN(vkCmdSetViewport);
    LOAD_DEVICE_FN(vkCmdSetScissor);
    LOAD_DEVICE_FN(vkCmdPipelineBarrier);
    LOAD_DEVICE_FN(vkCmdCopyImageToBuffer);
    LOAD_DEVICE_FN(vkCmdCopyBufferToImage);
    LOAD_DEVICE_FN(vkCmdPushConstants);
    LOAD_DEVICE_FN(vkCreateSampler);
    LOAD_DEVICE_FN(vkDestroySampler);
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

    // 桌面上的显示适配器名。矿卡这类没有视频输出的卡即使能渲染，画面也到不了屏幕，
    // 所以要优先选「真正驱动显示输出」的那块 GPU。
    std::vector<std::string> display_adapters;
    for (DWORD i = 0;; ++i) {
        DISPLAY_DEVICEW display{};
        display.cb = sizeof(display);
        if (EnumDisplayDevicesW(nullptr, i, &display, 0) == FALSE) {
            break;
        }
        if ((display.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0) {
            continue;
        }
        display_adapters.push_back(NarrowUtf8(display.DeviceString));
        LOG_STEP("[render] 桌面显示适配器: %s", display_adapters.back().c_str());
    }

    int best_score = -1;
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties properties{};
        g_vkGetPhysicalDeviceProperties(candidate, &properties);
        std::string device_name = properties.deviceName;
        std::string device_lower = device_name;
        std::transform(device_lower.begin(), device_lower.end(), device_lower.begin(), ::tolower);

        bool on_display = false;
        for (const std::string& adapter : display_adapters) {
            std::string adapter_lower = adapter;
            std::transform(adapter_lower.begin(), adapter_lower.end(), adapter_lower.begin(),
                           ::tolower);
            if (adapter_lower.find(device_lower) != std::string::npos ||
                device_lower.find(adapter_lower) != std::string::npos) {
                on_display = true;
                break;
            }
        }
        LOG_STEP("[render] 发现设备: %s（类型 %d，接显示器 %s）", properties.deviceName,
                 static_cast<int>(properties.deviceType), on_display ? "是" : "否");

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
            device_on_display = on_display;
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
    LOG_STEP("[render] 显卡 %s（Vulkan %u.%u.%u，驱动 %u）", properties.deviceName,
             VK_VERSION_MAJOR(properties.apiVersion),
             VK_VERSION_MINOR(properties.apiVersion),
             VK_VERSION_PATCH(properties.apiVersion), properties.driverVersion);
    // 这块卡没有接显示器（矿卡）时，交换链呈现到不了屏幕，
    // 改成离屏渲染 + GDI 上屏：渲染还是在这块卡上，只是最后一步交给系统显示。
    present_via_gdi = !device_on_display;
    LogText(present_via_gdi
                ? "[render] 该显卡不接显示器：改用离屏渲染 + GDI 上屏（渲染仍在 GPU）"
                : "[render] 该显卡接显示器：使用交换链直接呈现");
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
    // 注意：g_vkGetDeviceQueue 属于设备级入口，要等 LoadDeviceFunctions 之后才能取队列，
    // 这里不能调用（以前在这里调会跳到空指针）。
    return true;
}

bool Renderer::CreateRenderPass(std::string* error) {
    VkAttachmentDescription attachments[2]{};
    attachments[0].format = surface_format.format;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    attachments[1].format = VK_FORMAT_D32_SFLOAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_reference{};
    color_reference.attachment = 0;
    color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depth_reference{};
    depth_reference.attachment = 1;
    depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_reference;
    subpass.pDepthStencilAttachment = &depth_reference;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    create_info.attachmentCount = 2;
    create_info.pAttachments = attachments;
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

bool Renderer::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* buffer,
                            VkDeviceMemory* memory, void** mapped, const char* what,
                            std::string* error) {
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size > 0 ? size : 1;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (g_vkCreateBuffer(device, &buffer_info, nullptr, buffer) != VK_SUCCESS) {
        *error = std::string("创建缓冲失败：") + what;
        return false;
    }

    VkMemoryRequirements requirements{};
    g_vkGetBufferMemoryRequirements(device, *buffer, &requirements);
    const uint32_t type = FindMemoryType(physical_device, requirements.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) {
        *error = std::string("找不到可映射的显存类型：") + what;
        return false;
    }

    VkMemoryAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = type;
    if (g_vkAllocateMemory(device, &allocate_info, nullptr, memory) != VK_SUCCESS) {
        *error = std::string("分配缓冲内存失败：") + what;
        return false;
    }
    if (g_vkBindBufferMemory(device, *buffer, *memory, 0) != VK_SUCCESS) {
        *error = std::string("绑定缓冲内存失败：") + what;
        return false;
    }
    if (mapped != nullptr) {
        *mapped = nullptr;
        if (g_vkMapMemory(device, *memory, 0, VK_WHOLE_SIZE, 0, mapped) != VK_SUCCESS) {
            *error = std::string("映射缓冲内存失败：") + what;
            return false;
        }
    }
    return true;
}

void Renderer::DestroyBuffer(VkBuffer* buffer, VkDeviceMemory* memory, void** mapped) {
    if (*mapped != nullptr && g_vkUnmapMemory != nullptr) {
        g_vkUnmapMemory(device, *memory);
        *mapped = nullptr;
    }
    if (*buffer != VK_NULL_HANDLE && g_vkDestroyBuffer != nullptr) {
        g_vkDestroyBuffer(device, *buffer, nullptr);
        *buffer = VK_NULL_HANDLE;
    }
    if (*memory != VK_NULL_HANDLE && g_vkFreeMemory != nullptr) {
        g_vkFreeMemory(device, *memory, nullptr);
        *memory = VK_NULL_HANDLE;
    }
}

bool Renderer::CreateDepthResources(std::string* error) {
    DestroyDepthResources();

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_D32_SFLOAT;
    image_info.extent.width = extent.width;
    image_info.extent.height = extent.height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (g_vkCreateImage(device, &image_info, nullptr, &depth_image) != VK_SUCCESS) {
        *error = "创建深度图像失败";
        return false;
    }

    VkMemoryRequirements requirements{};
    g_vkGetImageMemoryRequirements(device, depth_image, &requirements);
    const uint32_t type = FindMemoryType(physical_device, requirements.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        *error = "找不到深度图像的显存类型";
        return false;
    }
    VkMemoryAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = type;
    if (g_vkAllocateMemory(device, &allocate_info, nullptr, &depth_memory) != VK_SUCCESS) {
        *error = "分配深度图像内存失败";
        return false;
    }
    if (g_vkBindImageMemory(device, depth_image, depth_memory, 0) != VK_SUCCESS) {
        *error = "绑定深度图像内存失败";
        return false;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = depth_image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_D32_SFLOAT;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    if (g_vkCreateImageView(device, &view_info, nullptr, &depth_view) != VK_SUCCESS) {
        *error = "创建深度图像视图失败";
        return false;
    }
    return true;
}

void Renderer::DestroyDepthResources() {
    if (device != VK_NULL_HANDLE) {
        if (g_vkDestroyImageView != nullptr && depth_view != VK_NULL_HANDLE) {
            g_vkDestroyImageView(device, depth_view, nullptr);
        }
        if (g_vkDestroyImage != nullptr && depth_image != VK_NULL_HANDLE) {
            g_vkDestroyImage(device, depth_image, nullptr);
        }
        if (g_vkFreeMemory != nullptr && depth_memory != VK_NULL_HANDLE) {
            g_vkFreeMemory(device, depth_memory, nullptr);
        }
    }
    depth_view = VK_NULL_HANDLE;
    depth_image = VK_NULL_HANDLE;
    depth_memory = VK_NULL_HANDLE;
}

bool Renderer::CreateCameraBuffer(std::string* error) {
    if (camera_buffer != VK_NULL_HANDLE) {
        return true;
    }
    return CreateBuffer(sizeof(CameraUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                        &camera_buffer, &camera_memory, &camera_mapped, "相机 uniform",
                        error);
}

void Renderer::DestroyCameraBuffer() {
    DestroyBuffer(&camera_buffer, &camera_memory, &camera_mapped);
}

bool Renderer::CreateShowPipeline(std::string* error) {
    if (!CreateCameraBuffer(error)) {
        return false;
    }

    // 描述符：0 相机 UBO，1/2/3 基础色 / 球面贴图 / 阴影遮罩
    VkDescriptorSetLayoutBinding bindings[4]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (int i = 1; i < 4; ++i) {
        bindings[i].binding = static_cast<uint32_t>(i);
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 4;
    layout_info.pBindings = bindings;
    if (g_vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &descriptor_layout) !=
        VK_SUCCESS) {
        *error = "创建描述符布局失败";
        return false;
    }

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0;
    push_range.size = static_cast<uint32_t>(sizeof(MaterialPush));

    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &descriptor_layout;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    if (g_vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout) !=
        VK_SUCCESS) {
        *error = "创建管线布局失败";
        return false;
    }

    VkShaderModuleCreateInfo shader_info{};
    shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_info.codeSize = kSceneVertexSpirv_size;
    shader_info.pCode = reinterpret_cast<const uint32_t*>(kSceneVertexSpirv);
    if (g_vkCreateShaderModule(device, &shader_info, nullptr, &vertex_module) != VK_SUCCESS) {
        *error = "创建顶点着色器模块失败";
        return false;
    }
    shader_info.codeSize = kSceneFragmentSpirv_size;
    shader_info.pCode = reinterpret_cast<const uint32_t*>(kSceneFragmentSpirv);
    if (g_vkCreateShaderModule(device, &shader_info, nullptr, &fragment_module) != VK_SUCCESS) {
        g_vkDestroyShaderModule(device, vertex_module, nullptr);
        *error = "创建片元着色器模块失败";
        return false;
    }

    if (!CreateShowPipelines(render_pass, &pipeline, &pipeline_blended, &pipeline_outline,
                             error)) {
        return false;
    }
    // 着色器模块留着，离屏截图的管线还要复用。
    // 截图目标建不起来不影响正常播放，只记一条日志。
    std::string offscreen_error;
    if (!CreateOffscreenTarget(&offscreen_error)) {
        LogText("[render] 离屏截图目标创建失败：" + offscreen_error);
        DestroyOffscreenTarget();
    }
    return true;
}

// 按渲染通道建三套管线：不透明（含抠像）、混合、描边。
// 窗口与离屏截图共用同一套状态，避免两边逐渐走偏。
bool Renderer::CreateShowPipelines(VkRenderPass pass, VkPipeline* opaque, VkPipeline* blended,
                                   VkPipeline* outline, std::string* error) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_module;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vertex_binding{};
    vertex_binding.binding = 0;
    vertex_binding.stride = static_cast<uint32_t>(sizeof(ShowVertex));
    vertex_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attributes[3]{};
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[0].offset = static_cast<uint32_t>(offsetof(ShowVertex, position));
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[1].offset = static_cast<uint32_t>(offsetof(ShowVertex, normal));
    attributes[2].location = 2;
    attributes[2].binding = 0;
    attributes[2].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[2].offset = static_cast<uint32_t>(offsetof(ShowVertex, uv));

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 3;
    vertex_input.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                              VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    auto build = [&](VkPipeline* target, VkCullModeFlags cull, bool blend,
                     bool depth_write) -> bool {
        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.cullMode = cull;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.lineWidth = 1.0f;

        VkPipelineDepthStencilStateCreateInfo depth_stencil{};
        depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil.depthTestEnable = VK_TRUE;
        depth_stencil.depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE;
        depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        VkPipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        attachment.blendEnable = blend ? VK_TRUE : VK_FALSE;
        attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        attachment.colorBlendOp = VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        attachment.alphaBlendOp = VK_BLEND_OP_ADD;

        VkPipelineColorBlendStateCreateInfo color_blend{};
        color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blend.attachmentCount = 1;
        color_blend.pAttachments = &attachment;

        VkGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.stageCount = 2;
        pipeline_info.pStages = stages;
        pipeline_info.pVertexInputState = &vertex_input;
        pipeline_info.pInputAssemblyState = &input_assembly;
        pipeline_info.pViewportState = &viewport_state;
        pipeline_info.pRasterizationState = &rasterizer;
        pipeline_info.pMultisampleState = &multisample;
        pipeline_info.pDepthStencilState = &depth_stencil;
        pipeline_info.pColorBlendState = &color_blend;
        pipeline_info.pDynamicState = &dynamic;
        pipeline_info.layout = pipeline_layout;
        pipeline_info.renderPass = pass;
        pipeline_info.subpass = 0;
        const VkResult result = g_vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                                            &pipeline_info, nullptr, target);
        if (result != VK_SUCCESS) {
            *error = "创建图形管线失败（" + ResultText(result) + "）";
            return false;
        }
        return true;
    };

    // 不透明：不剔除（双面布料、发片都有），写深度
    if (!build(opaque, VK_CULL_MODE_NONE, false, true)) {
        return false;
    }
    // 混合：不写深度，按 alpha 叠上去
    if (!build(blended, VK_CULL_MODE_NONE, true, false)) {
        return false;
    }
    // 描边：顶点里已经沿法线外扩，剔除正面只留背面轮廓
    return build(outline, VK_CULL_MODE_FRONT_BIT, false, true);
}

void Renderer::DestroyShowPipeline() {
    DestroyMaterialResources();
    DestroyOffscreenTarget();
    if (device != VK_NULL_HANDLE) {
        if (g_vkDestroyShaderModule != nullptr) {
            if (vertex_module != VK_NULL_HANDLE) {
                g_vkDestroyShaderModule(device, vertex_module, nullptr);
            }
            if (fragment_module != VK_NULL_HANDLE) {
                g_vkDestroyShaderModule(device, fragment_module, nullptr);
            }
        }
        vertex_module = VK_NULL_HANDLE;
        fragment_module = VK_NULL_HANDLE;
    }
    if (device != VK_NULL_HANDLE) {
        if (g_vkDestroyPipeline != nullptr && pipeline != VK_NULL_HANDLE) {
            g_vkDestroyPipeline(device, pipeline, nullptr);
        }
        if (g_vkDestroyPipeline != nullptr && pipeline_blended != VK_NULL_HANDLE) {
            g_vkDestroyPipeline(device, pipeline_blended, nullptr);
        }
        if (g_vkDestroyPipeline != nullptr && pipeline_outline != VK_NULL_HANDLE) {
            g_vkDestroyPipeline(device, pipeline_outline, nullptr);
        }
        if (g_vkDestroyDescriptorPool != nullptr && descriptor_pool != VK_NULL_HANDLE) {
            g_vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
        }
        if (g_vkDestroyPipelineLayout != nullptr && pipeline_layout != VK_NULL_HANDLE) {
            g_vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        }
        if (g_vkDestroyDescriptorSetLayout != nullptr && descriptor_layout != VK_NULL_HANDLE) {
            g_vkDestroyDescriptorSetLayout(device, descriptor_layout, nullptr);
        }
    }
    DestroyCameraBuffer();
    pipeline = VK_NULL_HANDLE;
    pipeline_blended = VK_NULL_HANDLE;
    pipeline_outline = VK_NULL_HANDLE;
    descriptor_pool = VK_NULL_HANDLE;
    descriptor_set = VK_NULL_HANDLE;
    pipeline_layout = VK_NULL_HANDLE;
    descriptor_layout = VK_NULL_HANDLE;
}

bool Renderer::CreateTextureImage(const ShowTexture& texture, VkImage* image,
                                  VkDeviceMemory* memory, VkImageView* view,
                                  std::string* error) {
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {texture.width, texture.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (g_vkCreateImage(device, &image_info, nullptr, image) != VK_SUCCESS) {
        *error = "创建材质贴图失败";
        return false;
    }
    VkMemoryRequirements requirements{};
    g_vkGetImageMemoryRequirements(device, *image, &requirements);
    const uint32_t type = FindMemoryType(physical_device, requirements.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        *error = "贴图找不到显存类型";
        return false;
    }
    VkMemoryAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = type;
    if (g_vkAllocateMemory(device, &allocate_info, nullptr, memory) != VK_SUCCESS ||
        g_vkBindImageMemory(device, *image, *memory, 0) != VK_SUCCESS) {
        *error = "贴图分配显存失败";
        return false;
    }

    // 先写进 host-visible 暂存缓冲，再用一次性命令拷进显存
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    void* staging_mapped = nullptr;
    if (!CreateHostBuffer(device, physical_device, texture.rgba.size(),
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging, &staging_memory,
                          &staging_mapped, "贴图暂存", error)) {
        return false;
    }
    if (staging_mapped != nullptr && !texture.rgba.empty()) {
        std::memcpy(staging_mapped, texture.rgba.data(), texture.rgba.size());
    }

    VkCommandBuffer command = command_buffers.empty() ? VK_NULL_HANDLE : command_buffers[0];
    if (command == VK_NULL_HANDLE) {
        *error = "贴图上传缺少命令缓冲";
        return false;
    }
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (g_vkBeginCommandBuffer(command, &begin_info) != VK_SUCCESS) {
        *error = "贴图上传命令录制失败";
        return false;
    }

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = *image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    g_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &barrier);

    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {texture.width, texture.height, 1};
    g_vkCmdCopyBufferToImage(command, staging, *image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             1, &copy);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    g_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                           1, &barrier);
    if (g_vkEndCommandBuffer(command) != VK_SUCCESS) {
        *error = "贴图上传命令结束失败";
        return false;
    }

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command;
    if (g_vkQueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE) != VK_SUCCESS ||
        g_vkDeviceWaitIdle(device) != VK_SUCCESS) {
        *error = "贴图上传提交失败";
        return false;
    }

    if (staging_mapped != nullptr) {
        g_vkUnmapMemory(device, staging_memory);
    }
    if (g_vkDestroyBuffer != nullptr) {
        g_vkDestroyBuffer(device, staging, nullptr);
    }
    if (g_vkFreeMemory != nullptr) {
        g_vkFreeMemory(device, staging_memory, nullptr);
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = *image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    if (g_vkCreateImageView(device, &view_info, nullptr, view) != VK_SUCCESS) {
        *error = "创建贴图视图失败";
        return false;
    }
    return true;
}

void Renderer::DestroyMaterialResources() {
    materials_gpu.clear();
    material_push.clear();
    if (device != VK_NULL_HANDLE) {
        if (g_vkDestroyImageView != nullptr) {
            for (VkImageView view : texture_views) {
                if (view != VK_NULL_HANDLE) {
                    g_vkDestroyImageView(device, view, nullptr);
                }
            }
            if (white_view != VK_NULL_HANDLE) {
                g_vkDestroyImageView(device, white_view, nullptr);
            }
        }
        if (g_vkDestroyImage != nullptr) {
            for (VkImage image : texture_images) {
                if (image != VK_NULL_HANDLE) {
                    g_vkDestroyImage(device, image, nullptr);
                }
            }
            if (white_image != VK_NULL_HANDLE) {
                g_vkDestroyImage(device, white_image, nullptr);
            }
        }
        if (g_vkFreeMemory != nullptr) {
            for (VkDeviceMemory memory : texture_memory) {
                if (memory != VK_NULL_HANDLE) {
                    g_vkFreeMemory(device, memory, nullptr);
                }
            }
            if (white_memory != VK_NULL_HANDLE) {
                g_vkFreeMemory(device, white_memory, nullptr);
            }
        }
        if (g_vkDestroySampler != nullptr && sampler != VK_NULL_HANDLE) {
            g_vkDestroySampler(device, sampler, nullptr);
        }
    }
    texture_images.clear();
    texture_memory.clear();
    texture_views.clear();
    sampler = VK_NULL_HANDLE;
    white_image = VK_NULL_HANDLE;
    white_memory = VK_NULL_HANDLE;
    white_view = VK_NULL_HANDLE;
}

bool Renderer::CreateMaterialResources(const Show& show, std::string* error) {
    if (device == VK_NULL_HANDLE || descriptor_layout == VK_NULL_HANDLE ||
        camera_buffer == VK_NULL_HANDLE) {
        *error = "渲染资源未就绪，无法上传材质";
        return false;
    }
    if (g_vkDeviceWaitIdle != nullptr) {
        g_vkDeviceWaitIdle(device);
    }
    DestroyMaterialResources();

    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.maxLod = 0.0f;
    if (g_vkCreateSampler(device, &sampler_info, nullptr, &sampler) != VK_SUCCESS) {
        *error = "创建采样器失败";
        return false;
    }

    // 1×1 白色占位：材质缺某张贴图时绑它，采样结果不影响配色
    ShowTexture white;
    white.width = 1;
    white.height = 1;
    white.rgba.assign(4, 255);
    if (!CreateTextureImage(white, &white_image, &white_memory, &white_view, error)) {
        return false;
    }

    texture_images.assign(show.textures.size(), VK_NULL_HANDLE);
    texture_memory.assign(show.textures.size(), VK_NULL_HANDLE);
    texture_views.assign(show.textures.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < show.textures.size(); ++i) {
        if (!CreateTextureImage(show.textures[i], &texture_images[i], &texture_memory[i],
                                &texture_views[i], error)) {
            return false;
        }
    }

    const uint32_t material_count = static_cast<uint32_t>(show.materials.size());
    const uint32_t set_count = material_count + 1;   // 0 号给未指定材质的绘制兜底
    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[0].descriptorCount = set_count;
    sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[1].descriptorCount = set_count * 3;
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = set_count;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = sizes;
    if (g_vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool) != VK_SUCCESS) {
        *error = "创建材质描述符池失败";
        return false;
    }

    std::vector<VkDescriptorSetLayout> layouts(set_count, descriptor_layout);
    std::vector<VkDescriptorSet> sets(set_count, VK_NULL_HANDLE);
    VkDescriptorSetAllocateInfo set_info{};
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_info.descriptorPool = descriptor_pool;
    set_info.descriptorSetCount = set_count;
    set_info.pSetLayouts = layouts.data();
    if (g_vkAllocateDescriptorSets(device, &set_info, sets.data()) != VK_SUCCESS) {
        *error = "分配材质描述符集失败";
        return false;
    }
    descriptor_set = sets[0];

    auto write_set = [&](VkDescriptorSet set, int32_t base, int32_t sphere, int32_t mask) {
        VkDescriptorBufferInfo buffer_info{};
        buffer_info.buffer = camera_buffer;
        buffer_info.offset = 0;
        buffer_info.range = sizeof(CameraUniform);
        VkDescriptorImageInfo images[3]{};
        const int32_t indices[3] = {base, sphere, mask};
        VkWriteDescriptorSet writes[4]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &buffer_info;
        for (int i = 0; i < 3; ++i) {
            VkImageView view = white_view;
            if (indices[i] >= 0 && static_cast<size_t>(indices[i]) < texture_views.size() &&
                texture_views[static_cast<size_t>(indices[i])] != VK_NULL_HANDLE) {
                view = texture_views[static_cast<size_t>(indices[i])];
            }
            images[i].sampler = sampler;
            images[i].imageView = view;
            images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            writes[i + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i + 1].dstSet = set;
            writes[i + 1].dstBinding = static_cast<uint32_t>(i + 1);
            writes[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i + 1].descriptorCount = 1;
            writes[i + 1].pImageInfo = &images[i];
        }
        g_vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);
    };

    write_set(sets[0], -1, -1, -1);

    materials_gpu.resize(material_count);
    material_push.resize(material_count);
    for (uint32_t i = 0; i < material_count; ++i) {
        const ShowMaterial& material = show.materials[i];
        materials_gpu[i].set = sets[i + 1];
        write_set(sets[i + 1], material.base_texture, material.sphere_texture,
                  material.shadow_texture);

        MaterialPush& push = material_push[i];
        push = MaterialPush{};
        std::copy(material.shadow_tint, material.shadow_tint + 3, push.shadow_tint);
        push.shadow_tint[3] = material.threshold;
        std::copy(material.blend, material.blend + 3, push.blend);
        push.blend[3] = material.rim_scale;
        std::copy(material.add, material.add + 3, push.add);
        push.add[3] = 1.5f;   // ToonShadowIntensity（与 Python 预览器默认值一致）
        std::copy(material.sphere_blend, material.sphere_blend + 3, push.sphere);
        push.sphere[3] = material.sphere_pow;
        push.params[0] = material.unlit ? 1.0f : 0.0f;
        push.params[1] = material.blend_mode == 1 ? 0.33f : 0.0f;
        push.params[2] = material.sphere_boost;
        push.params[3] = material.sphere_texture >= 0 ? 1.0f : 0.0f;
        push.outline[0] = material.outline ? material.outline_width : 0.0f;
    }
    LOG_STEP("[render] 材质资源：%u 个材质，%zu 张贴图", material_count, show.textures.size());
    return true;
}

void Renderer::DestroyOffscreenTarget() {
    if (device == VK_NULL_HANDLE) {
        offscreen_pipeline = VK_NULL_HANDLE;
        offscreen_blended = VK_NULL_HANDLE;
        offscreen_outline = VK_NULL_HANDLE;
        offscreen_pass = VK_NULL_HANDLE;
        offscreen_color = VK_NULL_HANDLE;
        offscreen_color_memory = VK_NULL_HANDLE;
        offscreen_color_view = VK_NULL_HANDLE;
        offscreen_depth = VK_NULL_HANDLE;
        offscreen_depth_memory = VK_NULL_HANDLE;
        offscreen_depth_view = VK_NULL_HANDLE;
        offscreen_framebuffer = VK_NULL_HANDLE;
        readback_buffer = VK_NULL_HANDLE;
        readback_memory = VK_NULL_HANDLE;
        readback_mapped = nullptr;
        return;
    }
    if (g_vkDestroyPipeline != nullptr && offscreen_pipeline != VK_NULL_HANDLE) {
        g_vkDestroyPipeline(device, offscreen_pipeline, nullptr);
    }
    if (g_vkDestroyPipeline != nullptr && offscreen_blended != VK_NULL_HANDLE) {
        g_vkDestroyPipeline(device, offscreen_blended, nullptr);
    }
    if (g_vkDestroyPipeline != nullptr && offscreen_outline != VK_NULL_HANDLE) {
        g_vkDestroyPipeline(device, offscreen_outline, nullptr);
    }
    if (g_vkDestroyFramebuffer != nullptr && offscreen_framebuffer != VK_NULL_HANDLE) {
        g_vkDestroyFramebuffer(device, offscreen_framebuffer, nullptr);
    }
    if (g_vkDestroyImageView != nullptr) {
        if (offscreen_color_view != VK_NULL_HANDLE) {
            g_vkDestroyImageView(device, offscreen_color_view, nullptr);
        }
        if (offscreen_depth_view != VK_NULL_HANDLE) {
            g_vkDestroyImageView(device, offscreen_depth_view, nullptr);
        }
    }
    if (g_vkDestroyImage != nullptr) {
        if (offscreen_color != VK_NULL_HANDLE) {
            g_vkDestroyImage(device, offscreen_color, nullptr);
        }
        if (offscreen_depth != VK_NULL_HANDLE) {
            g_vkDestroyImage(device, offscreen_depth, nullptr);
        }
    }
    if (g_vkFreeMemory != nullptr) {
        if (offscreen_color_memory != VK_NULL_HANDLE) {
            g_vkFreeMemory(device, offscreen_color_memory, nullptr);
        }
        if (offscreen_depth_memory != VK_NULL_HANDLE) {
            g_vkFreeMemory(device, offscreen_depth_memory, nullptr);
        }
    }
    if (g_vkDestroyBuffer != nullptr && readback_buffer != VK_NULL_HANDLE) {
        g_vkDestroyBuffer(device, readback_buffer, nullptr);
    }
    if (readback_mapped != nullptr && g_vkUnmapMemory != nullptr) {
        g_vkUnmapMemory(device, readback_memory);
        readback_mapped = nullptr;
    }
    if (g_vkFreeMemory != nullptr && readback_memory != VK_NULL_HANDLE) {
        g_vkFreeMemory(device, readback_memory, nullptr);
    }
    if (g_vkDestroyRenderPass != nullptr && offscreen_pass != VK_NULL_HANDLE) {
        g_vkDestroyRenderPass(device, offscreen_pass, nullptr);
    }
    offscreen_pipeline = VK_NULL_HANDLE;
    offscreen_blended = VK_NULL_HANDLE;
    offscreen_outline = VK_NULL_HANDLE;
    offscreen_pass = VK_NULL_HANDLE;
    offscreen_color = VK_NULL_HANDLE;
    offscreen_color_memory = VK_NULL_HANDLE;
    offscreen_color_view = VK_NULL_HANDLE;
    offscreen_depth = VK_NULL_HANDLE;
    offscreen_depth_memory = VK_NULL_HANDLE;
    offscreen_depth_view = VK_NULL_HANDLE;
    offscreen_framebuffer = VK_NULL_HANDLE;
    readback_buffer = VK_NULL_HANDLE;
    readback_memory = VK_NULL_HANDLE;
    readback_mapped = nullptr;
}

// 截图用的离屏目标：颜色附件最终布局是 TRANSFER_SRC，方便直接拷回内存。
bool Renderer::CreateOffscreenTarget(std::string* error) {
    DestroyOffscreenTarget();
    if (extent.width == 0 || extent.height == 0) {
        *error = "离屏目标尺寸为 0";
        return false;
    }

    VkAttachmentDescription attachments[2]{};
    attachments[0].format = surface_format.format;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachments[1] = attachments[0];
    attachments[1].format = VK_FORMAT_D32_SFLOAT;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_reference{};
    color_reference.attachment = 0;
    color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference depth_reference{};
    depth_reference.attachment = 1;
    depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_reference;
    subpass.pDepthStencilAttachment = &depth_reference;

    VkRenderPassCreateInfo pass_info{};
    pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass_info.attachmentCount = 2;
    pass_info.pAttachments = attachments;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    if (g_vkCreateRenderPass(device, &pass_info, nullptr, &offscreen_pass) != VK_SUCCESS) {
        *error = "创建离屏渲染通道失败";
        return false;
    }

    auto make_image = [&](VkFormat format, VkImageUsageFlags usage, VkImage* image,
                          VkDeviceMemory* memory, VkImageView* view, VkImageAspectFlags aspect)
        -> bool {
        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = format;
        image_info.extent = {extent.width, extent.height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = usage;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (g_vkCreateImage(device, &image_info, nullptr, image) != VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements requirements{};
        g_vkGetImageMemoryRequirements(device, *image, &requirements);
        const uint32_t type = FindMemoryType(physical_device, requirements.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) {
            return false;
        }
        VkMemoryAllocateInfo allocate_info{};
        allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate_info.allocationSize = requirements.size;
        allocate_info.memoryTypeIndex = type;
        if (g_vkAllocateMemory(device, &allocate_info, nullptr, memory) != VK_SUCCESS) {
            return false;
        }
        if (g_vkBindImageMemory(device, *image, *memory, 0) != VK_SUCCESS) {
            return false;
        }
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = *image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format;
        view_info.subresourceRange.aspectMask = aspect;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        return g_vkCreateImageView(device, &view_info, nullptr, view) == VK_SUCCESS;
    };

    if (!make_image(surface_format.format,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    &offscreen_color, &offscreen_color_memory, &offscreen_color_view,
                    VK_IMAGE_ASPECT_COLOR_BIT)) {
        *error = "创建离屏颜色附件失败";
        return false;
    }
    if (!make_image(VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                    &offscreen_depth, &offscreen_depth_memory, &offscreen_depth_view,
                    VK_IMAGE_ASPECT_DEPTH_BIT)) {
        *error = "创建离屏深度附件失败";
        return false;
    }

    VkFramebufferCreateInfo framebuffer_info{};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = offscreen_pass;
    const VkImageView views[2] = {offscreen_color_view, offscreen_depth_view};
    framebuffer_info.attachmentCount = 2;
    framebuffer_info.pAttachments = views;
    framebuffer_info.width = extent.width;
    framebuffer_info.height = extent.height;
    framebuffer_info.layers = 1;
    if (g_vkCreateFramebuffer(device, &framebuffer_info, nullptr, &offscreen_framebuffer) !=
        VK_SUCCESS) {
        *error = "创建离屏帧缓冲失败";
        return false;
    }

    // 截图通道与窗口通道共用同一套管线状态（只换渲染通道对象）
    if (!CreateShowPipelines(offscreen_pass, &offscreen_pipeline, &offscreen_blended,
                             &offscreen_outline, error)) {
        return false;
    }

    if (!CreateHostBuffer(device, physical_device,
                          static_cast<VkDeviceSize>(extent.width) * extent.height * 4,
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback_buffer,
                          &readback_memory, &readback_mapped, "截图回读", error)) {
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
    // 帧缓冲销毁后才能释放深度附件
    DestroyDepthResources();
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

    // 呈现模式：优先 MAILBOX（不阻塞等待垂直同步）。
    // 本机是挖矿卡（CMP 30HX，无显示输出），FIFO 会和合成器互相等，实测每帧卡 5 秒。
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t mode_count = 0;
    if (g_vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &mode_count,
                                                    nullptr) == VK_SUCCESS &&
        mode_count > 0) {
        std::vector<VkPresentModeKHR> modes(mode_count);
        if (g_vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &mode_count,
                                                        modes.data()) == VK_SUCCESS) {
            bool has_mailbox = false;
            for (VkPresentModeKHR mode : modes) {
                if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
                    has_mailbox = true;
                }
            }
            if (has_mailbox) {
                present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
            }
        }
    }
    LOG_STEP("[render] 呈现模式 %s", present_mode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX"
                                                                                : "FIFO");

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
    create_info.presentMode = present_mode;
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

    // 深度附件必须与颜色附件同尺寸
    if (!CreateDepthResources(error)) {
        return false;
    }

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
        const VkImageView attachments[2] = {image_views[i], depth_view};
        framebuffer_info.attachmentCount = 2;
        framebuffer_info.pAttachments = attachments;
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
    if (!CreateSwapchain(error)) {
        return false;
    }
    // 离屏目标（截图 / GDI 上屏）跟着新尺寸重建
    std::string offscreen_error;
    if (!CreateOffscreenTarget(&offscreen_error)) {
        LogText("[render] 离屏目标重建失败：" + offscreen_error);
    }
    return true;
}

bool Renderer::Init(HWND target, std::string* error) {
    window = target;
    hinstance = GetModuleHandleW(nullptr);

    STEP("Init: 加载 vulkan-1.dll");
    loader = LoadLibraryW(L"vulkan-1.dll");
    if (loader == nullptr) {
        *error = "无法加载 vulkan-1.dll（请安装显卡驱动或 Vulkan 运行时）";
        LOG_STEP("[render] %s", error->c_str());
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

    STEP("Init: 选择物理设备");
    if (!PickPhysicalDevice(error)) {
        LOG_STEP("[render] %s", error->c_str());
        return false;
    }
    STEP("Init: 创建逻辑设备");
    if (!CreateDeviceObject(error)) {
        LOG_STEP("[render] %s", error->c_str());
        return false;
    }
    STEP("Init: 取设备入口函数");
    if (!LoadDeviceFunctions(error)) {
        LOG_STEP("[render] %s", error->c_str());
        return false;
    }
    STEP("Init: 取图形队列");
    g_vkGetDeviceQueue(device, queue_family, 0, &queue);
    if (queue == VK_NULL_HANDLE) {
        *error = "取图形队列失败";
        LOG_STEP("[render] %s", error->c_str());
        return false;
    }
    STEP("Init: 创建渲染通道");
    if (!CreateRenderPass(error)) {
        LOG_STEP("[render] %s", error->c_str());
        return false;
    }
    STEP("Init: 创建命令资源");
    if (!CreateCommandResources(error)) {
        LOG_STEP("[render] %s", error->c_str());
        return false;
    }
    STEP("Init: 创建同步对象");
    if (!CreateSyncObjects(error)) {
        LOG_STEP("[render] %s", error->c_str());
        return false;
    }
    STEP("Init: 创建交换链");
    if (!CreateSwapchain(error)) {
        LOG_STEP("[render] %s", error->c_str());
        return false;
    }
    STEP("Init: 创建管线");
    if (!CreateShowPipeline(error)) {
        LOG_STEP("[render] %s", error->c_str());
        return false;
    }
    STEP("Init: 完成");
    LOG_STEP("[render] 初始化完成（%ux%u）", extent.width, extent.height);
    return true;
}

void Renderer::Shutdown() {
    if (device != VK_NULL_HANDLE) {
        if (g_vkDeviceWaitIdle != nullptr) {
            g_vkDeviceWaitIdle(device);
        }
        DestroySwapchainResources();
        show_gpu.Destroy(device);
        DestroyShowPipeline();
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

    VkClearValue clear_values[2]{};
    clear_values[0].color.float32[0] = color[0];
    clear_values[0].color.float32[1] = color[1];
    clear_values[0].color.float32[2] = color[2];
    clear_values[0].color.float32[3] = 1.0f;
    clear_values[1].depthStencil.depth = 1.0f;
    clear_values[1].depthStencil.stencil = 0;

    VkRenderPassBeginInfo pass_info{};
    pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass_info.renderPass = render_pass;
    pass_info.framebuffer = framebuffers[image_index];
    pass_info.renderArea.offset.x = 0;
    pass_info.renderArea.offset.y = 0;
    pass_info.renderArea.extent = extent;
    pass_info.clearValueCount = 2;
    pass_info.pClearValues = clear_values;

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

bool CreateHostBuffer(VkDevice device, VkPhysicalDevice physical, VkDeviceSize size,
                      VkBufferUsageFlags usage, VkBuffer* buffer, VkDeviceMemory* memory,
                      void** mapped, const char* what, std::string* error) {
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size > 0 ? size : 1;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (g_vkCreateBuffer(device, &buffer_info, nullptr, buffer) != VK_SUCCESS) {
        *error = std::string("创建缓冲失败：") + what;
        return false;
    }
    VkMemoryRequirements requirements{};
    g_vkGetBufferMemoryRequirements(device, *buffer, &requirements);
    const uint32_t type = FindMemoryType(physical, requirements.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) {
        *error = std::string("找不到可映射的显存类型：") + what;
        return false;
    }
    VkMemoryAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = type;
    if (g_vkAllocateMemory(device, &allocate_info, nullptr, memory) != VK_SUCCESS) {
        *error = std::string("分配缓冲内存失败：") + what;
        return false;
    }
    if (g_vkBindBufferMemory(device, *buffer, *memory, 0) != VK_SUCCESS) {
        *error = std::string("绑定缓冲内存失败：") + what;
        return false;
    }
    *mapped = nullptr;
    if (size > 0 && g_vkMapMemory(device, *memory, 0, VK_WHOLE_SIZE, 0, mapped) != VK_SUCCESS) {
        *error = std::string("映射缓冲内存失败：") + what;
        return false;
    }
    return true;
}

void ComputeGlobals(const ShowSkin& skin, const SkinCpu& cpu,
                    const std::vector<ShowBonePose>& pose,
                    std::vector<Affine>* globals) {
    const size_t count = skin.bones.size();
    globals->resize(count);
    for (size_t i = 0; i < count; ++i) {
        const ShowBone& bone = skin.bones[i];
        float quat[4] = {bone.rotation[0], bone.rotation[1], bone.rotation[2],
                         bone.rotation[3]};
        float translation[3] = {bone.translation[0], bone.translation[1],
                                bone.translation[2]};
        const int32_t anim = cpu.anim_bone[i];
        if (anim >= 0 && static_cast<size_t>(anim) < pose.size()) {
            const ShowBonePose& sampled = pose[static_cast<size_t>(anim)];
            if (sampled.has_rotation) {
                std::copy(sampled.rotation, sampled.rotation + 4, quat);
            }
            if (sampled.has_translation) {
                std::copy(sampled.translation, sampled.translation + 3, translation);
            }
        }
        const Affine local = FromBone(quat, translation, bone.scale);
        const int32_t parent = bone.parent;
        (*globals)[i] = (parent >= 0 && static_cast<size_t>(parent) < i)
                            ? Multiply((*globals)[static_cast<size_t>(parent)], local)
                            : local;
    }
}

// ---- 角色根骨骼诊断 ----
// 角色会不会在舞台上移动，唯一的数据来源就是动画里的根骨骼（AO_Position）平移。
// 这里把它换算成世界坐标，供日志和窗口标题显示：数值不变就是数据里没有位移，
// 而不是渲染没应用。

struct RootProbe {
    int32_t bone = -1;
    std::vector<int32_t> anim_bone;   // 骨骼 -> 动画轨道
};

RootProbe MakeRootProbe(const Show& show) {
    RootProbe probe;
    if (show.skins.empty()) {
        return probe;
    }
    const ShowSkin& skin = show.skins[0];
    for (size_t i = 0; i < skin.bones.size(); ++i) {
        if (skin.bones[i].name == "AO_Position") {
            probe.bone = static_cast<int32_t>(i);
            break;
        }
    }
    if (probe.bone < 0) {
        for (size_t i = 0; i < skin.bones.size(); ++i) {
            if (skin.bones[i].name == "Root") {
                probe.bone = static_cast<int32_t>(i);
                break;
            }
        }
    }
    if (probe.bone < 0) {
        return probe;
    }
    probe.anim_bone.assign(skin.bones.size(), -1);
    for (size_t i = 0; i < skin.bones.size(); ++i) {
        for (size_t a = 0; a < show.anim_bones.size(); ++a) {
            if (show.anim_bones[a] == skin.bones[i].name) {
                probe.anim_bone[i] = static_cast<int32_t>(a);
                break;
            }
        }
    }
    return probe;
}

bool CharacterRootWorld(const Show& show, const RootProbe& probe, double seconds, float out[3]) {
    if (probe.bone < 0 || show.skins.empty()) {
        return false;
    }
    const ShowSkin& skin = show.skins[0];
    SkinCpu cpu;
    cpu.anim_bone = probe.anim_bone;
    const std::vector<ShowBonePose> pose = show.SamplePose(seconds);
    std::vector<Affine> globals;
    ComputeGlobals(skin, cpu, pose, &globals);
    const Affine& model = globals[static_cast<size_t>(probe.bone)];
    for (int i = 0; i < 3; ++i) {
        out[i] = model.t[i] + show.character_offset[i];
    }
    return true;
}

// 材质决定这一段走哪个 pass：描边壳单独一遍（正面剔除），混合材质放在最后。
int32_t DrawPass(const Show& show, int32_t material) {
    if (material < 0 || static_cast<size_t>(material) >= show.materials.size()) {
        return 0;
    }
    const ShowMaterial& data = show.materials[static_cast<size_t>(material)];
    if (data.outline) {
        return 2;
    }
    return data.blend_mode >= 2 ? 1 : 0;
}

bool ShowGpu::Upload(VkDevice device, VkPhysicalDevice physical, const Show& show,
                     std::string* error) {
    Destroy(device);
    STEP("Upload: 统计尺寸");

    size_t stage_vertices_total = 0;
    size_t index_total = 0;
    for (const ShowStage& stage : show.stages) {
        stage_vertices_total += stage.vertices.size();
        index_total += stage.indices.size();
    }
    size_t skin_vertices_total = 0;
    for (const ShowSkin& skin : show.skins) {
        skin_vertices_total += skin.vertices.size();
        index_total += skin.indices.size();
    }
    stage_vertex_count = stage_vertices_total;
    skin_vertex_count = skin_vertices_total;
    index_count = index_total;
    LOG_STEP("[render] 上传：舞台顶点 %zu，角色顶点 %zu，索引 %zu", stage_vertex_count,
             skin_vertex_count, index_count);

    STEP("Upload: 创建舞台顶点缓冲");
    if (!CreateHostBuffer(device, physical,
                          static_cast<VkDeviceSize>(stage_vertex_count) * sizeof(ShowVertex),
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &stage_vertices, &stage_memory,
                          &stage_mapped, "舞台顶点", error)) {
        LOG_STEP("[render] %s", error->c_str());
        return false;
    }
    STEP("Upload: 创建角色顶点缓冲");
    if (!CreateHostBuffer(device, physical,
                          static_cast<VkDeviceSize>(skin_vertex_count) * sizeof(ShowVertex),
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &skin_vertices, &skin_memory,
                          &skin_mapped, "角色顶点", error)) {
        LOG_STEP("[render] %s", error->c_str());
        return false;
    }
    STEP("Upload: 创建索引缓冲");
    if (!CreateHostBuffer(device, physical,
                          static_cast<VkDeviceSize>(index_count) * sizeof(uint32_t),
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &indices, &index_memory,
                          &index_mapped, "索引", error)) {
        LOG_STEP("[render] %s", error->c_str());
        return false;
    }

    ShowVertex* stage_out = static_cast<ShowVertex*>(stage_mapped);
    ShowVertex* skin_out = static_cast<ShowVertex*>(skin_mapped);
    uint32_t* index_out = static_cast<uint32_t*>(index_mapped);
    if ((stage_vertex_count > 0 && stage_out == nullptr) ||
        (skin_vertex_count > 0 && skin_out == nullptr) ||
        (index_count > 0 && index_out == nullptr)) {
        *error = "顶点/索引缓冲映射失败";
        LOG_STEP("[render] %s", error->c_str());
        return false;
    }

    size_t vertex_at = 0;
    size_t index_at = 0;
    for (int pass = 0; pass < 3; ++pass) {
        passes[pass].clear();
    }
    for (const ShowStage& stage : show.stages) {
        if (!stage.vertices.empty()) {
            std::memcpy(stage_out + vertex_at, stage.vertices.data(),
                        stage.vertices.size() * sizeof(ShowVertex));
        }
        const uint32_t first_index = static_cast<uint32_t>(index_at);
        if (!stage.indices.empty()) {
            std::memcpy(index_out + index_at, stage.indices.data(),
                        stage.indices.size() * sizeof(uint32_t));
        }
        index_at += stage.indices.size();
        // 每个材质分段一段绘制；没有分段信息时整块当一段
        if (stage.sections.empty()) {
            DrawRange range;
            range.first_index = first_index;
            range.index_count = static_cast<uint32_t>(stage.indices.size());
            range.vertex_offset = static_cast<int32_t>(vertex_at);
            range.skinned = false;
            passes[0].push_back(range);
        } else {
            for (const ShowSection& section : stage.sections) {
                const uint32_t total = static_cast<uint32_t>(stage.indices.size());
                const uint32_t first = std::min(section.first_index, total);
                const uint32_t count = std::min(section.index_count, total - first);
                if (count == 0) {
                    continue;
                }
                DrawRange range;
                range.first_index = first_index + first;
                range.index_count = count;
                range.vertex_offset = static_cast<int32_t>(vertex_at);
                range.skinned = false;
                range.material = section.material;
                range.pass = DrawPass(show, section.material);
                passes[range.pass].push_back(range);
            }
        }
        vertex_at += stage.vertices.size();
    }

    skins.clear();
    skins.resize(show.skins.size());
    size_t skin_vertex_at = 0;
    for (size_t s = 0; s < show.skins.size(); ++s) {
        const ShowSkin& skin = show.skins[s];
        const uint32_t first_index = static_cast<uint32_t>(index_at);
        if (!skin.indices.empty()) {
            std::memcpy(index_out + index_at, skin.indices.data(),
                        skin.indices.size() * sizeof(uint32_t));
        }
        index_at += skin.indices.size();
        if (skin.sections.empty()) {
            DrawRange range;
            range.first_index = first_index;
            range.index_count = static_cast<uint32_t>(skin.indices.size());
            range.vertex_offset = static_cast<int32_t>(skin_vertex_at);
            range.skinned = true;
            passes[0].push_back(range);
        } else {
            for (const ShowSection& section : skin.sections) {
                const uint32_t total = static_cast<uint32_t>(skin.indices.size());
                const uint32_t first = std::min(section.first_index, total);
                const uint32_t count = std::min(section.index_count, total - first);
                if (count == 0) {
                    continue;
                }
                DrawRange range;
                range.first_index = first_index + first;
                range.index_count = count;
                range.vertex_offset = static_cast<int32_t>(skin_vertex_at);
                range.skinned = true;
                range.material = section.material;
                range.pass = DrawPass(show, section.material);
                passes[range.pass].push_back(range);
            }
        }

        SkinCpu& cpu = skins[s];
        cpu.vertex_offset = static_cast<uint32_t>(skin_vertex_at);
        cpu.vertex_count = static_cast<uint32_t>(skin.vertices.size());
        skin_vertex_at += skin.vertices.size();

        const size_t bone_count = skin.bones.size();
        cpu.anim_bone.assign(bone_count, -1);
        for (size_t i = 0; i < bone_count; ++i) {
            for (size_t a = 0; a < show.anim_bones.size(); ++a) {
                if (show.anim_bones[a] == skin.bones[i].name) {
                    cpu.anim_bone[i] = static_cast<int32_t>(a);
                    break;
                }
            }
        }

        std::vector<Affine> locals(bone_count);
        for (size_t i = 0; i < bone_count; ++i) {
            const ShowBone& bone = skin.bones[i];
            locals[i] = FromBone(bone.rotation, bone.translation, bone.scale);
        }
        cpu.bind_global.resize(bone_count);
        cpu.bind_inverse.resize(bone_count);
        for (size_t i = 0; i < bone_count; ++i) {
            const int32_t parent = skin.bones[i].parent;
            cpu.bind_global[i] = (parent >= 0 && static_cast<size_t>(parent) < i)
                                     ? Multiply(cpu.bind_global[static_cast<size_t>(parent)],
                                                locals[i])
                                     : locals[i];
            cpu.bind_inverse[i] = Inverse(cpu.bind_global[i]);
        }

        // 初始填入绑定姿势，保证第一帧渲染前缓冲里有合法数据
        ShowVertex* dst = skin_out + cpu.vertex_offset;
        for (size_t v = 0; v < skin.vertices.size(); ++v) {
            const ShowSkinVertex& src = skin.vertices[v];
            dst[v].position[0] = src.position[0] + show.character_offset[0];
            dst[v].position[1] = src.position[1] + show.character_offset[1];
            dst[v].position[2] = src.position[2] + show.character_offset[2];
            std::copy(src.normal, src.normal + 3, dst[v].normal);
            std::copy(src.uv, src.uv + 2, dst[v].uv);
        }
    }

    LOG_STEP("[render] 绘制段：不透明 %zu，半透明 %zu，描边 %zu；材质 %zu，贴图 %zu",
             passes[0].size(), passes[1].size(), passes[2].size(), show.materials.size(),
             show.textures.size());

    head_skin = -1;
    head_bone = -1;
    const char* head_names[3] = {"SO_Head", "AO_Head", "SC_Head"};
    for (int h = 0; h < 3 && head_skin < 0; ++h) {
        for (size_t s = 0; s < show.skins.size() && head_skin < 0; ++s) {
            if (!show.skins[s].attach_bone.empty()) {
                continue;
            }
            for (size_t i = 0; i < show.skins[s].bones.size(); ++i) {
                if (show.skins[s].bones[i].name == head_names[h]) {
                    head_skin = static_cast<int32_t>(s);
                    head_bone = static_cast<int32_t>(i);
                    break;
                }
            }
        }
    }

    // 场景包围盒（只看舞台与非跟随网格，头发是头部局部坐标，不能算进去）
    bool has_bounds = false;
    for (const ShowStage& stage : show.stages) {
        for (const ShowVertex& vertex : stage.vertices) {
            for (int i = 0; i < 3; ++i) {
                if (!has_bounds) {
                    bounds_lo[i] = bounds_hi[i] = vertex.position[i];
                } else {
                    bounds_lo[i] = std::min(bounds_lo[i], vertex.position[i]);
                    bounds_hi[i] = std::max(bounds_hi[i], vertex.position[i]);
                }
            }
            has_bounds = true;
        }
    }
    for (size_t s = 0; s < show.skins.size(); ++s) {
        if (!show.skins[s].attach_bone.empty()) {
            continue;
        }
        for (const ShowSkinVertex& vertex : show.skins[s].vertices) {
            for (int i = 0; i < 3; ++i) {
                const float value = vertex.position[i] + show.character_offset[i];
                if (!has_bounds) {
                    bounds_lo[i] = bounds_hi[i] = value;
                } else {
                    bounds_lo[i] = std::min(bounds_lo[i], value);
                    bounds_hi[i] = std::max(bounds_hi[i], value);
                }
            }
            has_bounds = true;
        }
    }
    if (!has_bounds) {
        for (int i = 0; i < 3; ++i) {
            bounds_lo[i] = -100.0f;
            bounds_hi[i] = 100.0f;
        }
    }
    LOG_STEP("[render] 场景包围盒 (%.1f, %.1f, %.1f) .. (%.1f, %.1f, %.1f)", bounds_lo[0],
             bounds_lo[1], bounds_lo[2], bounds_hi[0], bounds_hi[1], bounds_hi[2]);
    return true;
}

void ShowGpu::Destroy(VkDevice device) {
    auto release = [device](VkBuffer* buffer, VkDeviceMemory* memory, void** mapped) {
        if (device == VK_NULL_HANDLE) {
            return;
        }
        if (*mapped != nullptr && g_vkUnmapMemory != nullptr) {
            g_vkUnmapMemory(device, *memory);
            *mapped = nullptr;
        }
        if (*buffer != VK_NULL_HANDLE && g_vkDestroyBuffer != nullptr) {
            g_vkDestroyBuffer(device, *buffer, nullptr);
            *buffer = VK_NULL_HANDLE;
        }
        if (*memory != VK_NULL_HANDLE && g_vkFreeMemory != nullptr) {
            g_vkFreeMemory(device, *memory, nullptr);
            *memory = VK_NULL_HANDLE;
        }
    };
    release(&stage_vertices, &stage_memory, &stage_mapped);
    release(&skin_vertices, &skin_memory, &skin_mapped);
    release(&indices, &index_memory, &index_mapped);
    passes[0].clear();
    passes[1].clear();
    passes[2].clear();
    skins.clear();
    stage_vertex_count = 0;
    skin_vertex_count = 0;
    index_count = 0;
    head_skin = -1;
    head_bone = -1;
}

void ShowGpu::Update(const Show& show, double seconds) {
    if (skin_vertex_count == 0) {
        return;
    }
    const std::vector<ShowBonePose> pose = show.SamplePose(seconds);

    // 头发/饰品跟随头部：用提供 SO_Head 的网格算刚性跟随矩阵
    Affine head_model = IdentityAffine();
    if (head_skin >= 0 && head_bone >= 0 &&
        static_cast<size_t>(head_skin) < show.skins.size()) {
        const ShowSkin& skin = show.skins[static_cast<size_t>(head_skin)];
        const SkinCpu& cpu = skins[static_cast<size_t>(head_skin)];
        std::vector<Affine> globals;
        ComputeGlobals(skin, cpu, pose, &globals);
        const Affine transform = Multiply(
            globals[static_cast<size_t>(head_bone)],
            cpu.bind_inverse[static_cast<size_t>(head_bone)]);
        Affine place = IdentityAffine();
        const Affine& bind = cpu.bind_global[static_cast<size_t>(head_bone)];
        std::copy(bind.t, bind.t + 3, place.t);
        head_model = Multiply(transform, place);
    }

    const float ox = show.character_offset[0];
    const float oy = show.character_offset[1];
    const float oz = show.character_offset[2];
    ShowVertex* out = static_cast<ShowVertex*>(skin_mapped);

    for (size_t s = 0; s < show.skins.size(); ++s) {
        const ShowSkin& skin = show.skins[s];
        SkinCpu& cpu = skins[s];
        ShowVertex* dst = out + cpu.vertex_offset;

        if (!skin.attach_bone.empty()) {
            for (size_t v = 0; v < skin.vertices.size(); ++v) {
                const ShowSkinVertex& src = skin.vertices[v];
                float position[3];
                float normal[3];
                TransformPoint(head_model, src.position, position);
                TransformDirection(head_model, src.normal, normal);
                const float length = std::sqrt(normal[0] * normal[0] +
                                               normal[1] * normal[1] +
                                               normal[2] * normal[2]);
                if (length > 1e-8f) {
                    normal[0] /= length;
                    normal[1] /= length;
                    normal[2] /= length;
                }
                dst[v].position[0] = position[0] + ox;
                dst[v].position[1] = position[1] + oy;
                dst[v].position[2] = position[2] + oz;
                std::copy(normal, normal + 3, dst[v].normal);
                std::copy(src.uv, src.uv + 2, dst[v].uv);
            }
            continue;
        }

        std::vector<Affine> globals;
        ComputeGlobals(skin, cpu, pose, &globals);
        std::vector<Affine> skinned(skin.bones.size());
        for (size_t i = 0; i < skin.bones.size(); ++i) {
            skinned[i] = Multiply(globals[i], cpu.bind_inverse[i]);
        }

        for (size_t v = 0; v < skin.vertices.size(); ++v) {
            const ShowSkinVertex& src = skin.vertices[v];
            float position[3] = {0.0f, 0.0f, 0.0f};
            float normal[3] = {0.0f, 0.0f, 0.0f};
            for (int k = 0; k < 4; ++k) {
                const float weight = src.weights[k];
                if (weight <= 0.0f) {
                    continue;
                }
                const size_t bone = src.bones[k];
                if (bone >= skinned.size()) {
                    continue;
                }
                float moved[3];
                float turned[3];
                TransformPoint(skinned[bone], src.position, moved);
                TransformDirection(skinned[bone], src.normal, turned);
                for (int i = 0; i < 3; ++i) {
                    position[i] += moved[i] * weight;
                    normal[i] += turned[i] * weight;
                }
            }
            const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] +
                                           normal[2] * normal[2]);
            if (length > 1e-8f) {
                normal[0] /= length;
                normal[1] /= length;
                normal[2] /= length;
            }
            dst[v].position[0] = position[0] + ox;
            dst[v].position[1] = position[1] + oy;
            dst[v].position[2] = position[2] + oz;
            std::copy(normal, normal + 3, dst[v].normal);
            std::copy(src.uv, src.uv + 2, dst[v].uv);
        }
    }
}

// 三个 pass 依序绘制：不透明/抠像 → 混合 → 描边壳。
// 每段都用材质自己的描述符集与 push constant，切换材质时才重绑。
void Renderer::DrawShowGeometry(VkCommandBuffer command, const VkPipeline* pipelines) {
    if (show_gpu.indices == VK_NULL_HANDLE) {
        return;
    }
    const VkDeviceSize offset = 0;
    g_vkCmdBindIndexBuffer(command, show_gpu.indices, 0, VK_INDEX_TYPE_UINT32);

    MaterialPush fallback{};
    fallback.shadow_tint[0] = fallback.shadow_tint[1] = fallback.shadow_tint[2] = 1.0f;
    fallback.shadow_tint[3] = 0.47f;
    fallback.blend[0] = fallback.blend[1] = fallback.blend[2] = 1.0f;
    fallback.blend[3] = 7.5f;
    fallback.add[3] = 1.5f;
    fallback.sphere[0] = fallback.sphere[1] = fallback.sphere[2] = 1.0f;
    fallback.sphere[3] = 1.0f;
    fallback.params[2] = 1.0f;

    for (int pass_index = 0; pass_index < 3; ++pass_index) {
        if (show_gpu.passes[pass_index].empty() || pipelines[pass_index] == VK_NULL_HANDLE) {
            continue;
        }
        g_vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelines[pass_index]);
        VkBuffer bound = VK_NULL_HANDLE;
        VkDescriptorSet bound_set = VK_NULL_HANDLE;
        for (const DrawRange& draw : show_gpu.passes[pass_index]) {
            if (draw.index_count == 0) {
                continue;
            }
            VkBuffer wanted = draw.skinned ? show_gpu.skin_vertices : show_gpu.stage_vertices;
            if (wanted != bound) {
                g_vkCmdBindVertexBuffers(command, 0, 1, &wanted, &offset);
                bound = wanted;
            }
            const MaterialPush* push = &fallback;
            if (draw.material >= 0 &&
                static_cast<size_t>(draw.material) < materials_gpu.size()) {
                push = &material_push[static_cast<size_t>(draw.material)];
            }
            if (materials_gpu.empty() || draw.material < 0) {
                if (bound_set != descriptor_set) {
                    g_vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                              pipeline_layout, 0, 1, &descriptor_set, 0,
                                              nullptr);
                    bound_set = descriptor_set;
                }
            } else if (bound_set != materials_gpu[static_cast<size_t>(draw.material)].set) {
                bound_set = materials_gpu[static_cast<size_t>(draw.material)].set;
                g_vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                          pipeline_layout, 0, 1, &bound_set, 0, nullptr);
            }
            g_vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT |
                                     VK_SHADER_STAGE_FRAGMENT_BIT,
                                 0, static_cast<uint32_t>(sizeof(MaterialPush)), push);
            g_vkCmdDrawIndexed(command, draw.index_count, 1, draw.first_index,
                               draw.vertex_offset, 0);
        }
    }
}

bool Renderer::RecordShowCommandBuffer(uint32_t image_index) {
    VkCommandBuffer command = command_buffers[frame_index];
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (g_vkBeginCommandBuffer(command, &begin_info) != VK_SUCCESS) {
        return false;
    }

    VkClearValue clear_values[2]{};
    // 这里刻意不用纯黑：画面全黑时无法区分“呈现没生效”和“几何没画出来”
    clear_values[0].color.float32[0] = 0.07f;
    clear_values[0].color.float32[1] = 0.09f;
    clear_values[0].color.float32[2] = 0.13f;
    clear_values[0].color.float32[3] = 1.0f;
    clear_values[1].depthStencil.depth = 1.0f;
    clear_values[1].depthStencil.stencil = 0;

    // CPU 每帧重写顶点与 uniform，必须显式做一次 host -> GPU 的可见性屏障，
    // 否则部分驱动会读到旧数据（画面空白或扭曲）。
    VkMemoryBarrier host_barrier{};
    host_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host_barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    host_barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                                 VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT;
    g_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT,
                           VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                               VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           0, 1, &host_barrier, 0, nullptr, 0, nullptr);

    VkRenderPassBeginInfo pass_info{};
    pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass_info.renderPass = render_pass;
    pass_info.framebuffer = framebuffers[image_index];
    pass_info.renderArea.offset.x = 0;
    pass_info.renderArea.offset.y = 0;
    pass_info.renderArea.extent = extent;
    pass_info.clearValueCount = 2;
    pass_info.pClearValues = clear_values;
    g_vkCmdBeginRenderPass(command, &pass_info, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    g_vkCmdSetViewport(command, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.offset.x = 0;
    scissor.offset.y = 0;
    scissor.extent = extent;
    g_vkCmdSetScissor(command, 0, 1, &scissor);

    g_vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    g_vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1,
                              &descriptor_set, 0, nullptr);
    const VkPipeline pipelines[3] = {pipeline, pipeline_blended, pipeline_outline};
    DrawShowGeometry(command, pipelines);

    g_vkCmdEndRenderPass(command);
    return g_vkEndCommandBuffer(command) == VK_SUCCESS;
}

bool Renderer::DrawShowFrame(const Show& show, double seconds, std::string* error) {
    // 显卡不接显示器时：渲染到离屏图像，再把像素贴到窗口上
    if (present_via_gdi) {
        if (g_window_resized) {
            g_window_resized = false;
            RECT client{};
            GetClientRect(window, &client);
            if (client.right - client.left > 0 && client.bottom - client.top > 0) {
                if (!RecreateSwapchain(error)) {
                    LOG_STEP("[render] %s", error->c_str());
                    return false;
                }
            }
        }
        return BlitToWindow(show, seconds, error);
    }

    if (pipeline == VK_NULL_HANDLE || descriptor_set == VK_NULL_HANDLE ||
        show_gpu.indices == VK_NULL_HANDLE || camera_mapped == nullptr) {
        *error = "渲染资源未就绪（管线/描述符/索引缓冲/USO 缺失）";
        LOG_STEP("[render] %s", error->c_str());
        return false;
    }

    if (g_window_resized) {
        g_window_resized = false;
        RECT client{};
        GetClientRect(window, &client);
        if (client.right - client.left > 0 && client.bottom - client.top > 0) {
            STEP("DrawShowFrame: 重建交换链");
            if (!RecreateSwapchain(error)) {
                LOG_STEP("[render] %s", error->c_str());
                return false;
            }
        }
    }

    // 顶点缓冲与 uniform 每帧都由 CPU 重写，等上一帧彻底画完再写。
    const double t_begin = NowSeconds();
    STEP("DrawShowFrame: 等待设备空闲");
    if (g_vkDeviceWaitIdle(device) != VK_SUCCESS) {
        *error = "等待设备空闲失败";
        LOG_STEP("[render] %s", error->c_str());
        return false;
    }
    const double t_idle = NowSeconds();

    STEP("DrawShowFrame: 采样相机");
    const ShowCameraPose pose = show.SampleCamera(seconds);
    const float radius = 0.5f * std::sqrt(
        (show_gpu.bounds_hi[0] - show_gpu.bounds_lo[0]) *
            (show_gpu.bounds_hi[0] - show_gpu.bounds_lo[0]) +
        (show_gpu.bounds_hi[1] - show_gpu.bounds_lo[1]) *
            (show_gpu.bounds_hi[1] - show_gpu.bounds_lo[1]) +
        (show_gpu.bounds_hi[2] - show_gpu.bounds_lo[2]) *
            (show_gpu.bounds_hi[2] - show_gpu.bounds_lo[2]));
    const float near_plane = std::max(radius * 0.002f, 0.05f);
    const float far_plane = radius * 30.0f + 100.0f;
    const float aspect = extent.height > 0
                             ? static_cast<float>(extent.width) / static_cast<float>(extent.height)
                             : 1.0f;

    const Mat4 view = LookAtVulkan(pose.position, pose.target);
    const Mat4 projection = PerspectiveVulkan(45.0f, aspect, near_plane, far_plane);
    const Mat4 view_projection = MultiplyMat4(projection, view);

    STEP("DrawShowFrame: 写 uniform");
    CameraUniform uniform{};
    std::memcpy(uniform.view_projection, view_projection.m, sizeof(uniform.view_projection));
    std::memcpy(uniform.view, view.m, sizeof(uniform.view));
    uniform.light_direction[0] = 0.25f;
    uniform.light_direction[1] = 0.55f;
    uniform.light_direction[2] = 0.80f;
    uniform.light_direction[3] =
        (surface_format.format == VK_FORMAT_B8G8R8A8_SRGB ||
         surface_format.format == VK_FORMAT_R8G8B8A8_SRGB)
            ? 1.0f
            : 0.0f;   // w = 目标格式是否自带 sRGB 编码
    uniform.eye_position[0] = pose.position[0];
    uniform.eye_position[1] = pose.position[1];
    uniform.eye_position[2] = pose.position[2];
    std::memcpy(camera_mapped, &uniform, sizeof(uniform));

    STEP("DrawShowFrame: CPU 蒙皮");
    show_gpu.Update(show, seconds);
    const double t_skin = NowSeconds();

    // 首帧把几何和取景的硬数据记进日志：画面若是空的，这两行能直接说明原因
    if (frames_drawn < 3) {
        float center[4] = {(show_gpu.bounds_lo[0] + show_gpu.bounds_hi[0]) * 0.5f,
                           (show_gpu.bounds_lo[1] + show_gpu.bounds_hi[1]) * 0.5f,
                           (show_gpu.bounds_lo[2] + show_gpu.bounds_hi[2]) * 0.5f, 1.0f};
        float clip[4];
        ApplyMat4(view_projection, center, clip);
        const float ndc_x = clip[3] != 0.0f ? clip[0] / clip[3] : 0.0f;
        const float ndc_y = clip[3] != 0.0f ? clip[1] / clip[3] : 0.0f;
        const float ndc_z = clip[3] != 0.0f ? clip[2] / clip[3] : 0.0f;
        size_t indices = 0;
        size_t draws = 0;
        for (int pass_index = 0; pass_index < 3; ++pass_index) {
            draws += show_gpu.passes[pass_index].size();
            for (const DrawRange& draw : show_gpu.passes[pass_index]) {
                indices += draw.index_count;
            }
        }
        Logf("[render] 第 %llu 帧：draw %zu 个 / 索引 %zu，场景中心 NDC (%.2f, %.2f, %.3f)，"
             "相机 (%.0f, %.0f, %.0f) -> (%.0f, %.0f, %.0f)",
             static_cast<unsigned long long>(frames_drawn), draws, indices,
             ndc_x, ndc_y, ndc_z, pose.position[0], pose.position[1], pose.position[2],
             pose.target[0], pose.target[1], pose.target[2]);
        Logf("[render] uniform: m00=%.4f m11=%.4f m22=%.4f m32=%.4f m23=%.4f 光照 (%.2f, %.2f, "
             "%.2f)",
             uniform.view_projection[0], uniform.view_projection[5],
             uniform.view_projection[10], uniform.view_projection[14],
             uniform.view_projection[11], uniform.light_direction[0],
             uniform.light_direction[1], uniform.light_direction[2]);
        const ShowVertex* stage_vertices = static_cast<const ShowVertex*>(show_gpu.stage_mapped);
        const ShowVertex* skin_vertices = static_cast<const ShowVertex*>(show_gpu.skin_mapped);
        if (stage_vertices != nullptr) {
            Logf("[render] 舞台首个顶点 (%.1f, %.1f, %.1f) UV (%.3f, %.3f)",
                 stage_vertices[0].position[0], stage_vertices[0].position[1],
                 stage_vertices[0].position[2], stage_vertices[0].uv[0],
                 stage_vertices[0].uv[1]);
        }
        if (skin_vertices != nullptr) {
            Logf("[render] 角色首个顶点 (%.1f, %.1f, %.1f) UV (%.3f, %.3f)",
                 skin_vertices[0].position[0], skin_vertices[0].position[1],
                 skin_vertices[0].position[2], skin_vertices[0].uv[0],
                 skin_vertices[0].uv[1]);
        }
    }

    STEP("DrawShowFrame: 等待帧栅栏");
    if (g_vkWaitForFences(device, 1, &in_flight[frame_index], VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        *error = "等待帧栅栏失败";
        LOG_STEP("[render] %s", error->c_str());
        return false;
    }

    STEP("DrawShowFrame: 获取交换链图像");
    uint32_t image_index = 0;
    const VkResult acquire_result = g_vkAcquireNextImageKHR(
        device, swapchain, UINT64_MAX, image_available[frame_index], VK_NULL_HANDLE, &image_index);
    if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
        return RecreateSwapchain(error);
    }
    if (acquire_result != VK_SUCCESS && acquire_result != VK_SUBOPTIMAL_KHR) {
        *error = "获取交换链图像失败（" + ResultText(acquire_result) + "）";
        LOG_STEP("[render] %s", error->c_str());
        return false;
    }
    const double t_acquire = NowSeconds();

    if (g_vkResetFences(device, 1, &in_flight[frame_index]) != VK_SUCCESS) {
        *error = "重置帧栅栏失败";
        LOG_STEP("[render] %s", error->c_str());
        return false;
    }
    STEP("DrawShowFrame: 录制命令");
    if (!RecordShowCommandBuffer(image_index)) {
        *error = "录制演出命令缓冲失败";
        LOG_STEP("[render] %s", error->c_str());
        return false;
    }
    const double t_record = NowSeconds();
    STEP("DrawShowFrame: 提交");

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

    if (g_vkQueueSubmit(queue, 1, &submit_info, in_flight[frame_index]) != VK_SUCCESS) {
        *error = "提交演出命令失败";
        LOG_STEP("[render] %s", error->c_str());
        return false;
    }
    const double t_submit = NowSeconds();

    STEP("DrawShowFrame: 呈现");

    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &render_finished[frame_index];
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain;
    present_info.pImageIndices = &image_index;

    const VkResult present_result = g_vkQueuePresentKHR(queue, &present_info);
    const double t_present = NowSeconds();
    frame_index = (frame_index + 1) % kFramesInFlight;

    // 诊断：把每帧各阶段耗时写进日志，能区分「GPU/呈现被卡住」与「CPU 太慢」
    ++frames_drawn;
    if (frames_drawn <= 3 || (t_present - t_begin) > 0.2 || frames_drawn % 120 == 0) {
        Logf("[render] 帧耗时 %.0f ms（等待 %.0f / 蒙皮 %.0f / 取图含栅栏 %.0f / 录制 %.0f / "
             "提交 %.0f / 呈现 %.0f）",
             (t_present - t_begin) * 1000.0, (t_idle - t_begin) * 1000.0,
             (t_skin - t_idle) * 1000.0, (t_acquire - t_skin) * 1000.0,
             (t_record - t_acquire) * 1000.0, (t_submit - t_record) * 1000.0,
             (t_present - t_submit) * 1000.0);
    }

    if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR) {
        return RecreateSwapchain(error);
    }
    if (present_result != VK_SUCCESS) {
        *error = "呈现演出画面失败（" + ResultText(present_result) + "）";
        LOG_STEP("[render] %s", error->c_str());
        return false;
    }
    return true;
}

}  // namespace

// 把 RGBA/BGRA 像素写成 32 位 BMP（自下而上），不依赖任何图像库。
namespace {

bool WriteBmp(const std::wstring& path, uint32_t width, uint32_t height,
              const std::vector<uint8_t>& pixels, bool bgra, std::string* error) {
    const uint32_t row_bytes = width * 4;
    BITMAPFILEHEADER file_header{};
    BITMAPINFOHEADER info_header{};
    file_header.bfType = 0x4D42;   // "BM"
    file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    file_header.bfSize = file_header.bfOffBits + row_bytes * height;
    info_header.biSize = sizeof(BITMAPINFOHEADER);
    info_header.biWidth = static_cast<LONG>(width);
    info_header.biHeight = static_cast<LONG>(height);
    info_header.biPlanes = 1;
    info_header.biBitCount = 32;
    info_header.biCompression = BI_RGB;
    info_header.biSizeImage = row_bytes * height;

    std::FILE* file = _wfopen(path.c_str(), L"wb");
    if (file == nullptr) {
        *error = "无法写入截图文件";
        return false;
    }
    std::fwrite(&file_header, sizeof(file_header), 1, file);
    std::fwrite(&info_header, sizeof(info_header), 1, file);
    std::vector<uint8_t> row(row_bytes);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* source = pixels.data() + static_cast<size_t>(height - 1 - y) * row_bytes;
        for (uint32_t x = 0; x < width; ++x) {
            row[x * 4 + 0] = bgra ? source[x * 4 + 0] : source[x * 4 + 2];
            row[x * 4 + 1] = source[x * 4 + 1];
            row[x * 4 + 2] = bgra ? source[x * 4 + 2] : source[x * 4 + 0];
            row[x * 4 + 3] = 255;
        }
        std::fwrite(row.data(), row.size(), 1, file);
    }
    std::fclose(file);
    return true;
}

}  // namespace

bool Renderer::RenderOffscreenFrame(const Show& show, double seconds, std::string* error) {
    if (offscreen_pipeline == VK_NULL_HANDLE || readback_mapped == nullptr) {
        *error = "离屏目标未就绪";
        return false;
    }
    STEP("离屏帧: 等待设备空闲");
    if (g_vkDeviceWaitIdle(device) != VK_SUCCESS) {
        *error = "等待设备空闲失败";
        return false;
    }

    const ShowCameraPose pose = show.SampleCamera(seconds);
    const float radius = 0.5f * std::sqrt(
        (show_gpu.bounds_hi[0] - show_gpu.bounds_lo[0]) *
            (show_gpu.bounds_hi[0] - show_gpu.bounds_lo[0]) +
        (show_gpu.bounds_hi[1] - show_gpu.bounds_lo[1]) *
            (show_gpu.bounds_hi[1] - show_gpu.bounds_lo[1]) +
        (show_gpu.bounds_hi[2] - show_gpu.bounds_lo[2]) *
            (show_gpu.bounds_hi[2] - show_gpu.bounds_lo[2]));
    const float aspect = extent.height > 0
                             ? static_cast<float>(extent.width) / static_cast<float>(extent.height)
                             : 1.0f;
    const Mat4 view = LookAtVulkan(pose.position, pose.target);
    const Mat4 projection = PerspectiveVulkan(45.0f, aspect,
                                              std::max(radius * 0.002f, 0.05f),
                                              radius * 30.0f + 100.0f);
    const Mat4 view_projection = MultiplyMat4(projection, view);
    CameraUniform uniform{};
    std::memcpy(uniform.view_projection, view_projection.m, sizeof(uniform.view_projection));
    std::memcpy(uniform.view, view.m, sizeof(uniform.view));
    uniform.light_direction[0] = 0.25f;
    uniform.light_direction[1] = 0.55f;
    uniform.light_direction[2] = 0.80f;
    uniform.light_direction[3] =
        (surface_format.format == VK_FORMAT_B8G8R8A8_SRGB ||
         surface_format.format == VK_FORMAT_R8G8B8A8_SRGB)
            ? 1.0f
            : 0.0f;   // w = 目标格式是否自带 sRGB 编码
    uniform.eye_position[0] = pose.position[0];
    uniform.eye_position[1] = pose.position[1];
    uniform.eye_position[2] = pose.position[2];
    std::memcpy(camera_mapped, &uniform, sizeof(uniform));

    show_gpu.Update(show, seconds);

    STEP("CaptureFrame: 录制");
    VkCommandBuffer command = command_buffers[0];
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (g_vkBeginCommandBuffer(command, &begin_info) != VK_SUCCESS) {
        *error = "录制截图命令失败";
        return false;
    }

    VkMemoryBarrier host_barrier{};
    host_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host_barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    host_barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                                 VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT;
    g_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT,
                           VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                               VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           0, 1, &host_barrier, 0, nullptr, 0, nullptr);

    VkClearValue clear_values[2]{};
    clear_values[0].color.float32[0] = 0.07f;
    clear_values[0].color.float32[1] = 0.09f;
    clear_values[0].color.float32[2] = 0.13f;
    clear_values[0].color.float32[3] = 1.0f;
    clear_values[1].depthStencil.depth = 1.0f;

    VkRenderPassBeginInfo pass_info{};
    pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass_info.renderPass = offscreen_pass;
    pass_info.framebuffer = offscreen_framebuffer;
    pass_info.renderArea.extent = extent;
    pass_info.clearValueCount = 2;
    pass_info.pClearValues = clear_values;
    g_vkCmdBeginRenderPass(command, &pass_info, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.maxDepth = 1.0f;
    g_vkCmdSetViewport(command, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = extent;
    g_vkCmdSetScissor(command, 0, 1, &scissor);

    g_vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, offscreen_pipeline);
    g_vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1,
                              &descriptor_set, 0, nullptr);
    const VkPipeline offscreen_pipelines[3] = {offscreen_pipeline, offscreen_blended,
                                               offscreen_outline};
    DrawShowGeometry(command, offscreen_pipelines);
    g_vkCmdEndRenderPass(command);

    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {extent.width, extent.height, 1};
    g_vkCmdCopyImageToBuffer(command, offscreen_color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             readback_buffer, 1, &copy);

    if (g_vkEndCommandBuffer(command) != VK_SUCCESS) {
        *error = "结束截图命令失败";
        return false;
    }

    STEP("离屏帧: 提交");
    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command;
    if (g_vkQueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE) != VK_SUCCESS) {
        *error = "提交离屏渲染命令失败";
        return false;
    }
    if (g_vkDeviceWaitIdle(device) != VK_SUCCESS) {
        *error = "等待离屏渲染完成失败";
        return false;
    }
    return true;
}

void Renderer::CopyReadbackToBgra(std::vector<uint8_t>* out) const {
    const size_t bytes = static_cast<size_t>(extent.width) * extent.height * 4;
    out->resize(bytes);
    std::memcpy(out->data(), readback_mapped, bytes);
    const bool bgra = surface_format.format == VK_FORMAT_B8G8R8A8_UNORM ||
                      surface_format.format == VK_FORMAT_B8G8R8A8_SRGB;
    if (!bgra) {   // RGBA -> BGRA
        for (size_t i = 0; i + 3 < bytes; i += 4) {
            std::swap((*out)[i], (*out)[i + 2]);
        }
    }
    for (size_t i = 3; i < bytes; i += 4) {
        (*out)[i] = 255;
    }
}

bool Renderer::BlitToWindow(const Show& show, double seconds, std::string* error) {
    if (!RenderOffscreenFrame(show, seconds, error)) {
        return false;
    }
    CopyReadbackToBgra(&pixel_buffer);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(extent.width);
    info.bmiHeader.biHeight = -static_cast<LONG>(extent.height);   // 自顶向下
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(window);
    if (dc == nullptr) {
        *error = "取窗口 DC 失败";
        return false;
    }
    SetDIBitsToDevice(dc, 0, 0, extent.width, extent.height, 0, 0, 0, extent.height,
                      pixel_buffer.data(), &info, DIB_RGB_COLORS);
    ReleaseDC(window, dc);
    return true;
}

bool Renderer::CaptureFrame(const Show& show, double seconds, const std::wstring& path,
                            std::string* error) {
    if (!RenderOffscreenFrame(show, seconds, error)) {
        return false;
    }
    STEP("CaptureFrame: 写文件");
    std::vector<uint8_t> pixels;
    CopyReadbackToBgra(&pixels);
    if (!WriteBmp(path, extent.width, extent.height, pixels, true, error)) {
        return false;
    }
    Logf("[render] 已保存截图 %ux%u -> %s", extent.width, extent.height,
         NarrowUtf8(path).c_str());
    return true;
}


int RunShow(const Show& show) {
    if (show.camera_keys.empty()) {
        LogText("[render] 演出包里没有相机轨，无法播放");
        std::fprintf(stderr, "演出包里没有相机轨，无法播放\n");
        return 1;
    }
    if (show.poses.empty()) {
        LogText("[render] 演出包里没有动画数据，无法播放");
        std::fprintf(stderr, "演出包里没有动画数据，无法播放\n");
        return 1;
    }
    double duration = show.duration;
    if (duration <= 0.0) {
        duration = static_cast<double>(show.poses.back().frame) / show.anim_fps;
    }
    if (duration <= 0.0) {
        LogText("[render] 演出时长为 0，无法播放");
        std::fprintf(stderr, "演出时长为 0，无法播放\n");
        return 1;
    }
    LOG_STEP("[render] 开始播放：时长 %.2fs，相机 %zu 帧，动画 %zu 骨骼 × %zu 帧，"
             "舞台 %zu 三角，角色 %zu 三角",
             duration, show.camera_keys.size(), show.anim_bones.size(), show.poses.size(),
             show.StageTriangleCount(), show.SkinTriangleCount());

    // 先扫一遍整场的根骨骼世界坐标范围：这行是"角色到底会不会动"的直接证据
    const RootProbe root_probe = MakeRootProbe(show);
    const int32_t root_bone = root_probe.bone;
    {
        float low[3] = {0.0f, 0.0f, 0.0f};
        float high[3] = {0.0f, 0.0f, 0.0f};
        bool found = false;
        for (double t = 0.0; t <= duration; t += 0.5) {
            float value[3];
            if (!CharacterRootWorld(show, root_probe, t, value)) {
                break;
            }
            if (!found) {
                for (int i = 0; i < 3; ++i) {
                    low[i] = high[i] = value[i];
                }
                found = true;
            } else {
                for (int i = 0; i < 3; ++i) {
                    low[i] = std::min(low[i], value[i]);
                    high[i] = std::max(high[i], value[i]);
                }
            }
        }
        if (found) {
            const float dx = high[0] - low[0];
            const float dy = high[1] - low[1];
            const float dz = high[2] - low[2];
            Logf("[render] 角色根骨骼 %s：x %.1f..%.1f  y %.1f..%.1f  z %.1f..%.1f，"
                 "最大位移 %.1f（%.2f 米）",
                 root_bone >= 0 ? show.skins[0].bones[static_cast<size_t>(root_bone)].name.c_str()
                                : "无",
                 low[0], high[0], low[1], high[1], low[2], high[2],
                 std::sqrt(dx * dx + dy * dy + dz * dz),
                 std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0f);
        } else {
            LogText("[render] 角色根骨骼：未找到 AO_Position/Root，无法统计位移");
        }
    }

    std::string error;
    STEP("RunShow: 创建窗口");
    if (!CreateRenderWindow(&error)) {
        LogText("[render] " + error);
        MessageBoxA(nullptr, error.c_str(), "liveview", MB_OK | MB_ICONERROR);
        return 1;
    }
    // 盖住参数窗口，避免被遮挡后合成器不再消费画面
    SetForegroundWindow(g_window);
    BringWindowToTop(g_window);

    Renderer renderer;
    STEP("RunShow: 初始化 Vulkan");
    if (!renderer.Init(g_window, &error)) {
        LogText("[render] " + error);
        MessageBoxA(g_window, error.c_str(), "liveview", MB_OK | MB_ICONERROR);
        renderer.Shutdown();
        DestroyWindow(g_window);
        return 1;
    }
    STEP("RunShow: 上传演出数据");
    if (!renderer.show_gpu.Upload(renderer.device, renderer.physical_device, show, &error)) {
        LogText("[render] " + error);
        MessageBoxA(g_window, error.c_str(), "liveview", MB_OK | MB_ICONERROR);
        renderer.Shutdown();
        DestroyWindow(g_window);
        return 1;
    }
    STEP("RunShow: 上传材质");
    if (!renderer.CreateMaterialResources(show, &error)) {
        LogText("[render] " + error);
        MessageBoxA(g_window, error.c_str(), "liveview", MB_OK | MB_ICONERROR);
        renderer.Shutdown();
        DestroyWindow(g_window);
        return 1;
    }
    STEP("RunShow: 进入渲染循环");
    LogText("[render] 进入渲染循环");

    // 先离屏渲一帧存成图片：窗口没画面时，这张图能区分「渲染空」还是「呈现空」
    {
        const std::wstring config_path = ConfigPath();
        const size_t slash = config_path.find_last_of(L"\\/");
        const std::wstring shot_dir =
            slash == std::wstring::npos ? std::wstring() : config_path.substr(0, slash + 1);
        const std::wstring shot_path = shot_dir + L"shot_0000.bmp";
        std::string shot_error;
        if (!renderer.CaptureFrame(show, 0.0, shot_path, &shot_error)) {
            LogText("[render] 截图失败：" + shot_error);
        }
    }

    std::printf("演出：%.2fs  相机 %zu 关键帧  舞台 %zu 三角  角色 %zu 三角  动画 %zu 骨骼\n",
                duration, show.camera_keys.size(), show.StageTriangleCount(),
                show.SkinTriangleCount(), show.anim_bones.size());
    std::printf("ESC 或关闭窗口结束\n");
    std::fflush(stdout);

    LARGE_INTEGER frequency{};
    LARGE_INTEGER start{};
    LARGE_INTEGER now{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);

    MSG message{};
    bool running = true;
    double title_time = -1.0;
    long long frame_logs = 0;
    long long frames_since_log = 0;
    double last_frame_log = 0.0;

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

        RECT client{};
        GetClientRect(g_window, &client);
        if (client.right - client.left <= 0 || client.bottom - client.top <= 0) {
            Sleep(10);
            continue;
        }

        QueryPerformanceCounter(&now);
        const double elapsed = static_cast<double>(now.QuadPart - start.QuadPart) /
                               static_cast<double>(frequency.QuadPart);
        double seconds = elapsed;
        while (seconds > duration) {
            seconds -= duration;
        }

        if (!renderer.DrawShowFrame(show, seconds, &error)) {
            Logf("[render] 帧 %.2fs 失败：%s", seconds, error.c_str());
            MessageBoxA(g_window, error.c_str(), "liveview", MB_OK | MB_ICONERROR);
            break;
        }

        // 前几帧与之后每 5 秒记一行，附带平均帧耗时，便于判断是渲染慢还是呈现被卡住
        ++frames_since_log;
        if (frame_logs < 5 || elapsed - last_frame_log >= 5.0) {
            const double average_ms =
                frames_since_log > 0 ? (elapsed - last_frame_log) * 1000.0 / frames_since_log : 0.0;
            Logf("[render] 已渲染 %.2fs / %.1fs（第 %lld 帧，平均 %.0f ms/帧）", seconds,
                 duration, static_cast<long long>(frame_logs), average_ms);
            float root[3];
            if (CharacterRootWorld(show, root_probe, seconds, root)) {
                Logf("[render] 角色根骨骼世界坐标 (%.1f, %.1f, %.1f)", root[0], root[1],
                     root[2]);
            }
            ++frame_logs;
            last_frame_log = elapsed;
            frames_since_log = 0;
        }

        if (elapsed - title_time >= 0.25) {
            title_time = elapsed;
            char title[200];
            float root[3];
            if (CharacterRootWorld(show, root_probe, seconds, root)) {
                std::snprintf(title, sizeof(title),
                              "liveview show  %.1f / %.1f s   根骨骼 (%.0f, %.0f, %.0f)",
                              seconds, duration, root[0], root[1], root[2]);
            } else {
                std::snprintf(title, sizeof(title), "liveview show  %.1f / %.1f s", seconds,
                              duration);
            }
            SetWindowTextA(g_window, title);
        }
    }

    renderer.Shutdown();
    LogText("[render] 播放结束");
    if (g_window != nullptr && IsWindow(g_window)) {
        DestroyWindow(g_window);
    }
    return 0;
}

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
    STEP("RunRender: 创建窗口");
    if (!CreateRenderWindow(&error)) {
        LogText("[render] " + error);
        MessageBoxA(nullptr, error.c_str(), "liveview", MB_OK | MB_ICONERROR);
        return 1;
    }

    Renderer renderer;
    STEP("RunRender: 初始化 Vulkan");
    if (!renderer.Init(g_window, &error)) {
        LogText("[render] " + error);
        MessageBoxA(g_window, error.c_str(), "liveview", MB_OK | MB_ICONERROR);
        renderer.Shutdown();
        DestroyWindow(g_window);
        return 1;
    }
    LOG_STEP("[render] 相机包播放：%.2fs（tick %.0f）", duration, bundle.tick_fps);

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
