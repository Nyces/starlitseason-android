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

# 渲染后端（Vulkan）在下一里程碑启用；这里先把第三阶段需要的东西留好接口位。
option(LIVEVIEW_RENDER "启用 Vulkan 渲染后端（后续里程碑）" OFF)
if(LIVEVIEW_RENDER)
  find_package(Vulkan REQUIRED)
  target_sources(liveview PRIVATE src/render_vulkan.cpp)
  target_link_libraries(liveview PRIVATE Vulkan::Vulkan)
  target_compile_definitions(liveview PRIVATE LIVEVIEW_RENDER=1)
endif()
