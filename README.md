# Nskry

轻量级 Windows 截图、长截图与画中画监控工具，支持离线 OCR 文字识别和截图标注。

## 功能

- **智能框选截图** — QQ截图风格的窗口自动吸附 + 手动拖拽
- **框选后可调整** — 拖拽边缘/四角调整选区大小，拖拽内部移动选区
- **复制 / 保存** — 一键复制到剪贴板或保存为 PNG
- **固定截图** — 将截图钉在桌面最上层；支持拖动、缩放、复制、保存，以及二次标注
- **PiP 实时监控** — 框选任意窗口区域，生成画中画实时镜像
- **窗口缩放** — 固定截图和 PiP 窗口拖四角保持原始比例，拖四边自由缩放
- **长截图** — 手动或自动滚动采集页面，带实时预览；可调整裁剪范围并编辑结果
- **OCR 文字识别** — 使用 Windows 内置 OCR 离线识别截图，可查看识别文字或在图片上选择识别结果
- **截图标注** — 矩形、椭圆、箭头、画笔、马赛克和文字工具，支持撤销/重做
- **系统托盘** — 后台运行，托盘右键菜单快捷操作
- **插件管理** — 插件按需加载，可在设置中管理；官方长截图和 OCR 插件随安装器提供

## 快捷键

| 快捷键 | 功能 |
|--------|------|
| `Ctrl+Alt+A` | 截图 |
| `Ctrl+Alt+Q` | 退出 |
| `ESC` / 右键 | 取消当前截图或关闭当前操作 |

快捷键可在设置中调整。OCR 使用 Windows 提供的 OCR 语言包；可识别的语言取决于系统中已安装的语言组件。长截图和 OCR 插件可在设置的插件页面启用或管理。

## 构建

```powershell
# 需要 Visual Studio (MSVC) + CMake + Ninja
$vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" > nul 2>&1 && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build"
```

如需构建安装器，先完成上述 CMake 构建。构建过程会生成官方插件包：
`installer/bundled/nskry-scroll.nskryplugin` 和
`installer/bundled/nskry-ocr.nskryplugin`。再运行 Inno Setup：

```powershell
iscc installer/NskrySetup.iss
```

安装器会显示当前 `installer/bundled/` 中实际存在的官方插件作为可选组件，并通过 Nskry 自己的插件安装流程完成校验和安装。翻译与录屏仍属于后续规划，尚未实现。

## 技术栈

C++20 · Win32 API · D3D11 · Windows.Graphics.Capture · Windows OCR · GDI+

## 文档

- [项目交接文档](docs/HANDOFF.md) — 完整架构、决策、构建说明
- [插件 SDK](sdk/nskry_plugin.h) — 插件开发接口
- [安装器构建说明](installer/README.md) — per-user Setup 与可选官方插件打包
- [插件与发行计划](docs/nskry_distribution_plugin_plan.md) — 插件生命周期、包格式和发行规划

## License

Nskry is licensed under the GNU General Public License, version 3 or later
(GPL-3.0-or-later). See [LICENSE](LICENSE) for the license notice and the
official license text.
