# liveview —— live 演出验证程序
#
# 里程碑 1（原生）：双击启动、选游戏目录、输入 AES 密钥、原生解析 pak 索引。
# 本地没有 C++ 工具链时，构建由 GitHub Actions 完成（见 .github/workflows/liveview.yml）。

cmake_minimum_required(VERSION 3.16)
# 需要 C：FetchContent 拉进来的 zlib 是 C 项目
project(liveview LANGUAGES C CXX)

if(NOT WIN32)
  message(FATAL_ERROR "liveview 目前只支持 Windows（Win32 对话框 + pak 校验）")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# zlib：pak 条目按 64KB 分块压缩，解压要用 inflate。
# 用 FetchContent 拉官方源码（Windows 上没有系统 zlib，也不装 vcpkg）。
include(FetchContent)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zlib
  GIT_REPOSITORY https://github.com/madler/zlib.git
  GIT_TAG v1.3.1
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(zlib)

add_executable(liveview
  src/main.cpp
  src/app_window.cpp
  src/commands.cpp
  src/bundle.cpp
  src/show.cpp
  src/aes256.cpp
  src/pak.cpp
  src/config.cpp
  src/dialogs.cpp
  src/ue4package.cpp
)

target_include_directories(liveview PRIVATE src)
# zlib 不同版本的目标名不一样（zlibstatic / zlib），用生成表达式兜底
target_link_libraries(liveview PRIVATE
  $<IF:$<TARGET_EXISTS:zlibstatic>,zlibstatic,zlib>
  comdlg32 shell32 ole32 user32 gdi32 advapi32)

if(MSVC)
  # /utf-8：源码和字符串字面量按 UTF-8 解释，否则中文对话框会乱码
  target_compile_options(liveview PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(liveview PRIVATE -Wall -Wextra)
endif()

# 渲染后端（Vulkan）：第一里程碑只做窗口 + 交换链 + 清屏。
# 构建只拉 Vulkan-Headers（不装 Vulkan SDK、不链接 vulkan-1.lib），
# 运行时由程序自己 LoadLibrary("vulkan-1.dll") 取入口函数。
option(LIVEVIEW_RENDER "启用 Vulkan 渲染后端（窗口 + 交换链 + 清屏）" OFF)
if(LIVEVIEW_RENDER)
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

  # 着色器：构建期用 glslang 把 GLSL 编成 SPIR-V，再嵌进可执行文件。
  # 这样运行时不需要外部 .spv 文件，也不依赖 Vulkan SDK。
  set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
  set(ENABLE_SPIRV ON CACHE BOOL "" FORCE)
  set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
  set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG vulkan-sdk-1.3.296.0
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(glslang)

  set(LIVEVIEW_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
  set(LIVEVIEW_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
  file(MAKE_DIRECTORY ${LIVEVIEW_GENERATED_DIR})
  set(LIVEVIEW_VERT_HEADER ${LIVEVIEW_GENERATED_DIR}/scene_vert_spirv.h)
  set(LIVEVIEW_FRAG_HEADER ${LIVEVIEW_GENERATED_DIR}/scene_frag_spirv.h)
  set(LIVEVIEW_EMBED ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake)

  add_custom_command(
    OUTPUT ${LIVEVIEW_VERT_HEADER}
    COMMAND glslang-standalone -V --target-env vulkan1.0
            -o ${LIVEVIEW_GENERATED_DIR}/scene.vert.spv ${LIVEVIEW_SHADER_DIR}/scene.vert
    COMMAND ${CMAKE_COMMAND}
            -DINPUT=${LIVEVIEW_GENERATED_DIR}/scene.vert.spv
            -DOUTPUT=${LIVEVIEW_VERT_HEADER}
            -DNAME=kSceneVertexSpirv
            -P ${LIVEVIEW_EMBED}
    DEPENDS ${LIVEVIEW_SHADER_DIR}/scene.vert ${LIVEVIEW_EMBED}
    COMMENT "编译 scene.vert -> SPIR-V")

  add_custom_command(
    OUTPUT ${LIVEVIEW_FRAG_HEADER}
    COMMAND glslang-standalone -V --target-env vulkan1.0
            -o ${LIVEVIEW_GENERATED_DIR}/scene.frag.spv ${LIVEVIEW_SHADER_DIR}/scene.frag
    COMMAND ${CMAKE_COMMAND}
            -DINPUT=${LIVEVIEW_GENERATED_DIR}/scene.frag.spv
            -DOUTPUT=${LIVEVIEW_FRAG_HEADER}
            -DNAME=kSceneFragmentSpirv
            -P ${LIVEVIEW_EMBED}
    DEPENDS ${LIVEVIEW_SHADER_DIR}/scene.frag ${LIVEVIEW_EMBED}
    COMMENT "编译 scene.frag -> SPIR-V")

  add_custom_target(liveview_shaders DEPENDS ${LIVEVIEW_VERT_HEADER} ${LIVEVIEW_FRAG_HEADER})
  add_dependencies(liveview_shaders glslang-standalone)
  add_dependencies(liveview liveview_shaders)
  target_include_directories(liveview PRIVATE ${LIVEVIEW_GENERATED_DIR})
endif()
