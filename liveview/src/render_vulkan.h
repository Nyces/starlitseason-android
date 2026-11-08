// Vulkan 渲染后端（第一阶段：窗口 + 交换链 + 清屏循环）。
//
// 只有在 CMake 里打开 LIVEVIEW_RENDER 时才会编译这个实现。

#pragma once

#include "bundle.h"
#include "show.h"

namespace liveview {

// 打开一个窗口并按时间轴播放相机。
// seconds < 0 表示播放导出范围的完整时长。
// 返回进程退出码（0 表示正常播放结束）。
int RunRender(const Bundle& bundle, double seconds);

// 打开一个窗口播放完整演出（相机 + 舞台 + 角色蒙皮动画），循环播放直到 ESC。
int RunShow(const Show& show);

}  // namespace liveview
