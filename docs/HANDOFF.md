# Nskry — 项目交接文档

> **最后更新**：2026-09-14
> **项目状态**：Phase 1 + Phase 2 完成，核心截图功能可用
> **项目路径**：`d:\codes\Nskry`

---

## 1. 项目概述

Nskry 是一款轻量级 Windows 工具，融合了"QQ截图式的流畅框选交互"与"实时画中画（PiP）局部窗口动态监视"功能。

**核心卖点**：
- 极致轻量：可执行文件 ~78KB，运行时 RAM < 20MB
- GPU 零拷贝：WGC 捕获 → `CopySubresourceRegion` 裁剪 → SwapChain 直出，全程不经过 CPU 内存
- 穿透遮挡：基于 Windows.Graphics.Capture (WGC)，即使目标窗口被遮挡也能实时捕获
- 可组装架构：核心 + 插件 DLL 的模块化设计（SDK 已定义，PluginManager 待实现）

---

## 2. 已完成功能

| 功能 | 状态 | 说明 |
|------|------|------|
| 快捷键唤醒 (Ctrl+Alt+A) | ✅ | 全屏蒙版 + 智能吸附 |
| 手动拖拽框选 | ✅ | 自由矩形选区 |
| 框选后调整大小/位置 | ✅ | 8方向边角+内部拖拽移动 |
| 自绘工具栏 | ✅ | 复制/保存/固定/监控 4个按钮 |
| 复制截图到剪贴板 | ✅ | Win32 Clipboard API |
| 保存截图为 PNG | ✅ | GDI+ 编码 + GetSaveFileNameW |
| 固定截图 (PinWindow) | ✅ | 置顶 GDI 窗口，可拖拽/缩放 |
| PiP 实时监控 | ✅ | WGC + D3D11 SwapChain |
| 窗口缩放锁定比例 | ✅ | 拖四角锁比例，拖边缘自由缩放 |
| 系统托盘图标 | ✅ | 右键菜单 + 双击截图 |
| 双屏适配 | ✅ | 虚拟屏幕坐标系 |
| 快捷键退出 (Ctrl+Alt+Q) | ✅ | 完整资源清理 |

---

## 3. 技术栈

| 层面 | 选型 | 说明 |
|------|------|------|
| 语言 | C++20 | 极致性能，最低 RAM |
| WinRT 绑定 | C++/WinRT (SDK 内置 `cppwinrt`) | 用于 WGC API |
| GPU 渲染 | D3D11 (native `d3d11.h`) | 设备创建 + 纹理操作 |
| 屏幕捕获 | Windows.Graphics.Capture (WGC) | 事件驱动帧捕获 |
| 截图编码 | GDI+ (`gdiplus.h`) | PNG 保存 |
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
│   └── HANDOFF.md                          # 本文档
├── sdk/
│   └── nskry_plugin.h                      # 插件 SDK 公开头文件
├── build/
│   └── Nskry.exe                           # 已编译可执行文件 (Release, ~78KB)
└── src/
    ├── pch.h                               # 预编译头 (Windows/D3D/WinRT/STL)
    ├── main.cpp                            # WinMain + 热键 + 系统托盘 + 编排逻辑
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
        ├── selection_window.h              # 全屏蒙版 + SelectionAction/Result
        ├── selection_window.cpp            # 智能吸附 + 框选 + 调整 + 工具栏
        ├── pip_window.h                    # PiP 实时渲染窗口
        ├── pip_window.cpp                  # DXGI SwapChain + 比例锁定
        ├── pin_window.h                    # 固定截图置顶窗口
        └── pin_window.cpp                  # GDI StretchBlt + 比例锁定
```

---

## 5. 关键架构决策

### 5.1 WGC 编程式捕获 (不使用 Picker)
```cpp
auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
interop->CreateForWindow(hwnd, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item));
```

### 5.2 D3D11 → WinRT 设备桥接
```cpp
auto dxgi = m_device.as<IDXGIDevice>();
::CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put());
m_winrtDevice = inspectable.as<IDirect3DDevice>();
```

### 5.3 从 WGC 帧获取 D3D11 纹理
```cpp
auto surface = frame.Surface();
winrt::com_ptr<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
surface.as<::IUnknown>()->QueryInterface(__uuidof(...), access.put_void());
access->GetInterface(__uuidof(ID3D11Texture2D), srcTex.put_void());
```
> **⚠️ 关键坑点**：`IDirect3DDxgiInterfaceAccess` **必须使用完全限定名**
> `::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess`

### 5.4 GPU 零拷贝渲染管线
```
WGC FrameArrived
  → 从帧表面获取 ID3D11Texture2D (帧纹理)
  → CopySubresourceRegion → 中间裁剪纹理 (GPU crop)
  → CaptureSession 回调传出裁剪纹理
  → PipWindow.RenderFrame: CopyResource → SwapChain 后缓冲
  → Present(1, 0) — DWM 通过 DXGI_SCALING_STRETCH 处理缩放
```

### 5.5 SelectionWindow 状态机
```
Hovering → (左键按下) → Dragging → (左键松开) → Selected ⇄ Adjusting
                                                        ↓ (点击按钮)
                                                  FinishWithAction → 回调
```
- `Selected` 状态展示工具栏，支持进入 `Adjusting` 调整选区
- 点击选区内部/边缘/四角进入调整，点击工具栏按钮确认动作
- 点击选区外部重置回 `Hovering` 重新选择

### 5.6 GDI+ 与 NOMINMAX 兼容
```cpp
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
```
GDI+ 头文件需要 `min`/`max` 宏，但我们用 `NOMINMAX`。上面的 workaround 注入 `std::min`/`max` 到 `Gdiplus` 命名空间。

### 5.7 窗口缩放比例锁定 (WM_SIZING)
- **拖四角**：锁定宽高比。对比"以宽为准"和"以高为准"两个候选尺寸，选面积更小的那个，使窗口边缘精确跟手
- **拖边缘**：自由缩放，不锁定比例
- PipWindow 和 PinWindow 使用相同逻辑

### 5.8 PiP 关闭生命周期
```
用户点击 X → PipWindow::WndProc WM_CLOSE → 调用 m_onClose 回调
  → 回调中 PostMessage(g_mainHwnd, WM_CLEANUP) (延迟到主线程)
  → MainWndProc 处理 WM_CLEANUP: g_capture.reset() → g_pip.reset()
```

### 5.9 系统托盘
- `Shell_NotifyIconW` 注册托盘图标
- 双击触发截图，右键弹出上下文菜单 (Capture / Quit)
- `WM_USER_TRAY = WM_USER + 1`

---

## 6. 构建方法

### 前置条件
- Visual Studio 2022/2025 或 Build Tools (需要 MSVC C++ 编译器和 Windows SDK)
- CMake 3.24+
- Ninja (通常随 VS 安装)

### 编译命令
```powershell
# 1. 激活 MSVC 环境 (根据实际 VS 安装路径调整)
$vsPath = "C:\Program Files\Microsoft Visual Studio\18\Community"
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"

# 2. 配置 (仅首次，或删除 build/ 目录后)
cmd /c "`"$vcvars`" > nul 2>&1 && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release"

# 3. 编译
cmd /c "`"$vcvars`" > nul 2>&1 && cmake --build build"
```

> **提示**：如果编译时报 `LNK1104: cannot open file 'Nskry.exe'`，先 `taskkill /F /IM Nskry.exe` 后重试。

### 运行
```
d:\codes\Nskry\build\Nskry.exe
```
- `Ctrl+Alt+A` — 进入截图框选模式
- `Ctrl+Alt+Q` — 退出程序
- 系统托盘右键 — Capture / Quit

---

## 7. 已知问题与限制

### 7.1 Chrome/Electron 遮挡时画面冻结
Chromium 的 "Native Window Occlusion" 优化。所有 WGC 软件（包括 OBS）的共同限制。

### 7.2 LoadCursorW / LoadIconW 宏问题
`IDC_ARROW` 等宏展开类型不匹配，使用 `MAKEINTRESOURCEW(32512)` 替代。

### 7.3 std::min/max 类型不匹配
Win32 的 `LONG` 与 `int` 混用时需显式 `static_cast<int>`。

### 7.4 C4819 编码警告
MSVC codepage 936 对 UTF-8 文件发出警告，不影响编译。

---

## 8. 插件 SDK 设计

插件头文件：`sdk/nskry_plugin.h`

每个插件 DLL 需导出 4 个函数：
- `nskry_plugin_info()` → 返回 `NskryPluginInfo` (名称/ID/版本/能力)
- `nskry_plugin_init(ctx)` → 初始化
- `nskry_plugin_execute(ctx)` → 执行动作
- `nskry_plugin_shutdown()` → 清理

能力标志：
- `NSKRY_CAP_TOOLBAR_ACTION` — 在截图工具栏添加按钮
- `NSKRY_CAP_POST_CAPTURE` — 截图后处理（如 OCR）
- `NSKRY_CAP_STANDALONE` — 独立功能（如录屏）

PluginManager（扫描 `plugins/` 目录加载 DLL）尚未实现。

---

## 9. 下一步开发计划

1. **Phase 3** — 截图编辑器插件 (标注/马赛克/箭头)
2. **Phase 4** — 截长图 + OCR 文字识别
3. **Phase 5** — 录屏 (MP4/GIF)
4. **PluginManager** — 实现 DLL 加载和管理

---

## 10. 开发约定

- 所有类和命名空间使用 `nskry::` 前缀
- COM 指针统一使用 `winrt::com_ptr<T>`
- Win32 窗口类名统一以 `Nskry` 为前缀
- 插件 DLL 命名：`nskry-{功能名}.dll`
- 构建：CMake + Ninja + MSVC
- 编译定义：`WIN32_LEAN_AND_MEAN`, `NOMINMAX`, `WINRT_LEAN_AND_MEAN`, `UNICODE`, `_UNICODE`
