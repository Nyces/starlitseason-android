# liveview —— live 演出验证程序的骨架
#
# 目标：在 PC 上以真实 LevelSequence 数据驱动一场 live（相机 / 角色 / 舞台 / 音轨）。
# 当前阶段只做「读数据 + 时间轴 + 探测」，渲染后端（Vulkan）在后续里程碑接入。
#
# 本地没有 C++ 工具链时，构建由 GitHub Actions 完成（见 .github/workflows/liveview.yml）。

cmake_minimum_required(VERSION 3.16)
project(liveview LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_executable(liveview
  src/main.cpp
  src/bundle.cpp
)

target_include_directories(liveview PRIVATE src)

if(MSVC)
  target_compile_options(liveview PRIVATE /W4 /permissive-)
else()
  target_compile_options(liveview PRIVATE -Wall -Wextra)
endif()

# 渲染后端（Vulkan）：第一里程碑只做窗口 + 交换链 + 清屏。
# 构建只拉 Vulkan-Headers（不装 Vulkan SDK、不链接 vulkan-1.lib），
# 运行时由程序自己 LoadLibrary("vulkan-1.dll") 取入口函数。
option(LIVEVIEW_RENDER "启用 Vulkan 渲染后端（窗口 + 交换链 + 清屏）" OFF)
if(LIVEVIEW_RENDER)
  if(NOT WIN32)
    message(FATAL_ERROR "渲染后端目前只支持 Windows（Win32 窗口 + VK_KHR_win32_surface）")
  endif()

  include(FetchContent)
  set(VULKAN_HEADERS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
  set(VULKAN_HEADERS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG v1.4.313
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(vulkan_headers)

  target_sources(liveview PRIVATE src/render_vulkan.cpp)
  target_link_libraries(liveview PRIVATE Vulkan::Headers)
  target_compile_definitions(liveview PRIVATE
    LIVEVIEW_RENDER=1
    VK_NO_PROTOTYPES
    VK_USE_PLATFORM_WIN32_KHR)
endif()
