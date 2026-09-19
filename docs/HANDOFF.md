# Nskry — 项目交接文档

> **最后更新**：2026-09-17
> **项目状态**：Stage A-G 已完成；Phase 4 截长图已实现为独立插件并可构建运行，OCR/翻译/录屏尚未开发
> **项目路径**：`d:\codes\Nskry`

---

## 1. 项目概述

Nskry 是一款轻量级 Windows 工具，融合了"QQ截图式的流畅框选交互"与"实时画中画（PiP）局部窗口动态监视"功能，并配备完整的矢量截图标注编辑器与置顶二次批注功能。

**核心卖点**：
- 极致轻量：完整包含标注引擎、实时 GPU PiP、插件系统的可执行文件仅 ~135KB，运行时 RAM < 20MB
- GPU 零拷贝：WGC 捕获 → `CopySubresourceRegion` 裁剪 → SwapChain 直出，全程不经过 CPU 内存
- 穿透遮挡：基于 Windows.Graphics.Capture (WGC)，即使目标窗口被遮挡也能实时捕获
- 完整标注系统：矩形、椭圆、箭头、自由画笔、马赛克、文字标注，带色板与粗细选择，支持撤销/重做
- 置顶截图二次批注：PinWindow 支持右键菜单随时进入编辑模式追加标注并烘焙保存
- 可组装架构：核心 + 插件 DLL 的模块化设计（PluginManager 已就绪，扫描 `plugins/` 动态挂载）

---

## 2. 已完成功能

| 功能 | 状态 | 说明 |
|------|------|------|
| 快捷键唤醒 (Ctrl+Alt+A) | ✅ | 全屏蒙版 + 智能吸附 |
| 手动拖拽框选 | ✅ | 自由矩形选区 |
| 框选后调整大小/位置 | ✅ | 8方向边角+内部拖拽移动 |
| 截图标注工具栏 | ✅ | 矩形/椭圆/箭头/画笔/马赛克/文字/撤销/重做 |
| 二级属性面板 | ✅ | 3档线宽 (2/4/8px) + 8色快捷色板 |
| 即时文字标注 | ✅ | 原地编辑框输入，支持阴影与平滑渲染 |
| 局部马赛克 | ✅ | 实时像素化隐私脱敏处理 |
| 编辑历史 | ✅ | Ctrl+Z 撤销，Ctrl+Y 重做 |
| 复制截图到剪贴板 | ✅ | Win32 Clipboard API (自动烘焙标注) |
| 保存截图为 PNG | ✅ | GDI+ 编码 + GetSaveFileNameW (自动烘焙标注) |
| 固定截图 (PinWindow) | ✅ | 置顶 GDI 窗口，可拖拽/缩放 (自动烘焙标注) |
| 固定截图二次批注 | ✅ | PinWindow 右键菜单「✎ 标注编辑」/双击，支持粗细(2/4/8)、8色色盘选择并烘焙保存 |
| PiP 实时监控 | ✅ | WGC + D3D11 SwapChain (支持标注浮动叠加) |
| PiP 动态二次批注 | ✅ | PipWindow 右键菜单「✎ 标注编辑」/双击，子窗口工具栏 + D3D11 动态 GPU 纹理叠加，支持在60FPS动态流上实时重批注/撤销/重做 |
| 窗口缩放锁定比例 | ✅ | 拖四角锁比例，拖边缘自由缩放 |
| 系统托盘图标 | ✅ | 右键菜单 + 双击截图 |
| 双屏适配 | ✅ | 虚拟屏幕坐标系 |
| 插件系统 (PluginManager) | ✅ | 动态扫描 `plugins/*.dll` 挂载扩展功能 |
| 快捷键退出 (Ctrl+Alt+Q) | ✅ | 完整资源清理 |

---

## 3. 技术栈

| 层面 | 选型 | 说明 |
|------|------|------|
| 语言 | C++20 | 极致性能，最低 RAM |
| WinRT 绑定 | C++/WinRT (SDK 内置 `cppwinrt`) | 用于 WGC API |
| GPU 渲染 | D3D11 (native `d3d11.h`) | 设备创建 + 纹理操作 |
| 屏幕捕获 | Windows.Graphics.Capture (WGC) | 事件驱动帧捕获 |
| 2D 绘图与标注 | GDI+ (`gdiplus.h`) | 矢量标注抗锯齿渲染 + PNG 编码 |
| UI 框架 | 纯 Win32 API | 零依赖 |
| 构建系统 | CMake 3.24+ | Ninja 生成器 + MSVC 编译器 |
| COM 管理 | `winrt::com_ptr<T>` | 统一管理 WinRT 和 native COM |
| 最低 OS | Windows 10 2004 (build 19041) | WGC 编程式捕获最低要求 |

---

## 4. 文件结构

```
d:\codes\Nskry\
├── CMakeLists.txt                          # CMake 构建配置
├── docs/
│   ├── HANDOFF.md                          # 本文档
│   └── implementation_plan.md              # 架构与规划文档
├── sdk/
│   └── nskry_plugin.h                      # 插件 SDK 公开头文件
├── build/
│   └── Nskry.exe                           # 已编译可执行文件 (Release)
└── src/
    ├── pch.h                               # 预编译头 (Windows/D3D/WinRT/STL)
    ├── main.cpp                            # WinMain + 热键 + 托盘 + 插件生命周期
    ├── core/
    │   ├── plugin_manager.h                # 插件管理器声明
    │   └── plugin_manager.cpp              # 扫描 plugins/*.dll 动态装载
    ├── interop/
    │   ├── window_info.h                   # WindowInfo 结构体 (HWND + bounds)
    │   ├── window_enumerator.h             # WindowEnumerator 类声明
    │   └── window_enumerator.cpp           # EnumWindows + DWM 精确边界
    ├── capture/
    │   ├── d3d_device.h                    # D3DDevice 类 (native + WinRT 双接口)
    │   ├── d3d_device.cpp                  # D3D11CreateDevice + WinRT 桥接
    │   ├── capture_session.h               # CaptureSession + CropRegion
    │   └── capture_session.cpp             # WGC 完整生命周期 + GPU 裁剪
    └── ui/
        ├── annotation/
        │   ├── annotation_shape.h          # 各矢量图元定义 (Rect/Ellipse/Arrow/Pen/Mosaic/Text)
        │   ├── annotation_engine.h         # 标注引擎声明 (Tool/Style/Undo/Redo/Bake)
        │   └── annotation_engine.cpp       # 标注引擎实现
        ├── selection_window.h              # 全屏蒙版 + 工具栏与标注状态
        ├── selection_window.cpp            # 智能吸附 + 框选 + 调整 + 标注与烘焙
        ├── pip_window.h                    # PiP 实时渲染窗口
        ├── pip_window.cpp                  # DXGI SwapChain + 比例锁定
        ├── pin_window.h                    # 固定截图置顶窗口
        └── pin_window.cpp                  # GDI StretchBlt + 右键菜单 + 二次标注编辑
```

---

## 5. 关键架构决策

### 5.1 标注系统设计 (AnnotationEngine)
- 矢量化图元设计（`AnnotationShape` 基类）：支持撤销重做历史栈。
- 坐标归一化：图元统一采用相对坐标系（`0` 到 `width`, `0` 到 `height`），在 `SelectionWindow` 中通过 `Graphics::TranslateTransform` 偏移，在 `PinWindow` 中通过 `Graphics::ScaleTransform` 缩放，保证缩放窗口时标注与底图绝对同步。
- 烘焙机制（Baking）：调用 `BakeToBitmap()` 时将底图与全部标注图层高品质合并成一张纯净的 `HBITMAP`，直接交由系统剪贴板、文件写入或新 Pin 窗口使用。

### 5.2 插件管理器 (PluginManager)
- 启动时自动扫描 `<exe_dir>/plugins/*.dll`。
- 按 C-ABI 标准校验 `nskry_plugin_info`, `nskry_plugin_init`, `nskry_plugin_execute`, `nskry_plugin_shutdown` 四个函数接口。
- 支持 `NSKRY_CAP_TOOLBAR_ACTION`、`NSKRY_CAP_POST_CAPTURE`、`NSKRY_CAP_STANDALONE`。

---

## 6. 构建方法

```powershell
# 1. 激活 MSVC 环境
$vsPath = "C:\Program Files\Microsoft Visual Studio\18\Community"
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"

# 2. 配置并构建（同时生成 installer/bundled/nskry-scroll.nskryplugin）
cmd /c "`"$vcvars`" > nul 2>&1 && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build"
```

构建安装器（需要 Inno Setup 6）：

```powershell
iscc installer/NskrySetup.iss
```

---

## 7. 常用快捷操作

| 场景 | 操作 | 说明 |
|------|------|------|
| 全局 | `Ctrl+Alt+A` | 唤醒截图 |
| 全局 | `Ctrl+Alt+Q` | 退出程序 |
| 截图时 | 点击标注工具 | 展开二级颜色/粗细面板并可绘制 |
| 截图时 | `Ctrl+Z` / `Ctrl+Y` | 撤销 / 重做上一步标注 |
| 截图时 | `Ctrl+C` | 快速复制当前截图（含标注） |
| 截图时 | `Ctrl+S` | 快速保存当前截图（含标注） |
| 截图时 | `ESC` / 右键 | 退出当前标注工具；若无工具则取消截图 |
| 固定置顶截图 | 右键点击 Pin 窗口 | 弹出菜单：标注编辑 / 复制 / 保存 / 关闭 |
| 固定置顶截图 | 双击 Pin 窗口 | 快速进入二次标注编辑模式 |
| Pin 编辑模式 | 切换工具/粗细/色盘 | 展开二级工具栏调整 3 档线宽 (2/4/8) 与 8 种高对比色 |
| Pin 编辑模式 | 点击 `✓` 完成 | 烘焙标注为新底图，恢复置顶查看 |
| Pin 编辑模式 | 点击 `✕` 取消 | 放弃本次追加的标注 |
| PiP 实时监控 | 右键点击 PiP 窗口 | 弹出菜单：标注编辑 / 关闭 |
| PiP 实时监控 | 双击 PiP 窗口 | 快速进入/退出二次批注编辑模式 |
| PiP 编辑模式 | 鼠标拖拽绘制 | 实时 GPU Alpha-Blend 在 60FPS 视频上叠加标注 |
| PiP 编辑模式 | `Ctrl+Z` / `Ctrl+Y` | 撤销 / 重做监控图层标注 |
| PiP 编辑模式 | 点击 `✓` / `✕` | 确认保留标注或撤销本次批注并返回拖拽模式 |

---

## 8. 核心踩坑与技术攻坚记录 (新 Agent 必读)

在开发与实测过程中解决过若干极为隐蔽的底层图形学与 Win32 机制陷阱，切勿回退或破坏以下机制：

### 8.1 DXGI Flip-Model SwapChain 遮挡子窗口陷阱
- **现象**：`PipWindow` 使用 `DXGI_SWAP_EFFECT_FLIP_DISCARD` 模式。DWM 会将 SwapChain 的呈现表面直接绑定至 HWND 客户区，**导致该 HWND 的任何 `WS_CHILD` 子窗口（如工具栏、文字输入框）全部被翻转链画面遮挡在最底层**。
- **解决方案**：二级工具栏 `m_hwndToolbar` 与文字输入框 `m_hTextEdit` 必须创建为 **Owned `WS_POPUP` 顶级窗口**（`WS_POPUP`, `WS_EX_TOPMOST | WS_EX_TOOLWINDOW`，其 Owner 为 `m_hwnd`）。在 DWM 桌面合成层中，Owned Popup 窗口天生位于 Owner 窗口之上，浮动在 DirectX 表面之上。

### 8.2 Win32 `CreateWindowExW` Popup 窗口的 `hMenu` 陷阱
- **现象**：对于 `WS_CHILD` 窗口，第 10 个参数 `hMenu` 用于传递子控件 ID（如 `(HMENU)104`）。但对于 `WS_POPUP` 窗口，Windows 严格将其解析为真正的菜单句柄指针！传入 `(HMENU)104` 会导致 `CreateWindowExW` 直接失败报错 `ERROR_INVALID_MENU_HANDLE` (1401) 并返回 NULL。
- **解决方案**：为 `WS_POPUP` 样式的工具栏或编辑框创建窗口时，第 10 个参数 `hMenu` 必须传 `nullptr`。

### 8.3 GDI+ 在透明 32位 ARGB 表面的文字 Alpha 通道丢失陷阱
- **现象**：在透明的 32bpp 标注缓存表面上绘制文字时，GDI+ 默认的 ClearType 亚像素抗锯齿只针对底色计算 RGB，**但在完全透明的画板上会将目标 Alpha 通道强制写为 0**！在 D3D11 使用 `SrcAlpha` 进行 Alpha 混合时，`Color * Alpha = 0`，导致文字标注 100% 隐形。
- **解决方案**：在 `AnnotationEngine::Draw` 绘制文字前，必须显式调用 `g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit)`。这强制 GDI+ 使用灰度抗锯齿并写入完整的 Alpha 通道（Alpha = 255），配合文字描边/投影即可清晰可见。

### 8.4 动态画中画的实时马赛克取样机制
- **现象**：马赛克图元 `MosaicShape::Draw` 需要从底图取样均值颜色。静态截图有固定 `HBITMAP`，而 PiP 监控每秒 60 帧由 GPU 渲染，没有常驻的 CPU 底图位图。
- **解决方案**：通过 `PipWindow::GetLastFrameHdc()` 利用缓存的 `D3D11_USAGE_STAGING` 纹理将最新捕获帧的显存映射至 Top-down GDI DIBSection (`guard.hdc`)，提供给标注引擎取样。所有 D3D Context 访问均受 `m_renderMutex` 线程锁保护。若取样失败，则回退为磨砂渐变色块保底。

### 8.5 `WM_WINDOWPOSCHANGED` 意外销毁文本框
- **现象**：当 Owned Popup 创建显示时，Windows 会向其父窗口分发 `WM_WINDOWPOSCHANGED`。若在此处无条件调用 `CommitTextEdit()`，会导致输入框刚被创建就立刻被销毁。
- **解决方案**：在 `WM_WINDOWPOSCHANGED` 中必须检查 `!(pos->flags & SWP_NOMOVE)`，仅当窗口真实发生移动时才提交文字，防止误杀。

---

## 9. 后续开发路线图 (Incoming Roadmap)

### Phase 4: 截长图 (Scrolling Screenshot) — 已实现
- **状态**：由 `plugins/nskry-scroll` 提供，打包为 `installer/bundled/nskry-scroll.nskryplugin`，通过统一插件包安装/加载链路接入。
- **当前能力**：框选目标区域、手动/自动滚动采集、连续帧位移匹配与增量拼接、实时预览、暂停/取消、长图裁剪及后续标注入口。
- **目标**：支持在滚动页面（长网页、终端、代码编辑器、聊天记录）中一键滚动合成超长截图。
- **关键设计**：
  1. 框选目标滚动区域，识别滚动条或目标 HWND。
  2. 向目标窗口发送滚轮或滚动指令（`WM_MOUSEWHEEL` / `WM_VSCROLL`）。
  3. 利用 WGC 连续提取每屏竖向图像切片。
  4. 基于特征行哈希匹配（Normalized Cross Correlation / Row Hash Matching）自动计算竖向位移 $\Delta y$。
  5. 拼接缝合为单一超高分辨率 PNG 画布并弹出标准标注/保存面板。

插件源码、manifest 与 CMake 打包配置位于 `plugins/nskry-scroll/`；发布时先构建插件包，再构建 Inno Setup 安装器。

### Phase 5: OCR 文字识别插件 (OCR Plugin)
- **目标**：实现为独立扩展 `plugins/nskry_ocr.dll`。
- **关键设计**：
  1. 基于 SDK 接口 `sdk/nskry_plugin.h`，导出标准 4 个 C-ABI 函数。
  2. 采用 Windows 10/11 内置的原生 `Windows.Media.Ocr.OcrEngine` API，零第三方模型依赖，无额外动态库体积负担，离线支持中英双语。
  3. 在截图工具栏上暴露 `[T]` OCR 图标；识别完成后弹出可复制/分段的半透明文本框覆盖层。

### Phase 6: 屏幕录制插件 (Screen Recording Plugin)
- **目标**：实现为独立扩展 `plugins/nskry_recorder.dll`。
- **关键设计**：
  1. 复用核心的 WGC + D3D11 零拷贝捕获管线。
  2. 使用 Windows Media Foundation (`IMFSinkWriter`) 支持 GPU 硬件编码输出标准 H.264 `.mp4`。
  3. 支持轻量级局部 GIF 动图导出（内置 NeuQuant / Octree 调色板量化算法）。
