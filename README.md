# Nskry

轻量级 Windows 截图 & 画中画监控工具。

## 功能

- **智能框选截图** — QQ截图风格的窗口自动吸附 + 手动拖拽
- **框选后可调整** — 拖拽边缘/四角调整选区大小，拖拽内部移动选区
- **复制 / 保存** — 一键复制到剪贴板或保存为 PNG
- **固定截图** — 将截图钉在桌面最上层（可拖拽、可缩放）
- **PiP 实时监控** — 框选任意窗口区域，生成画中画实时镜像
- **比例锁定** — 拖四角锁定原始比例，拖边缘自由缩放
- **系统托盘** — 后台运行，托盘右键菜单快捷操作
- **插件架构** — SDK 与插件管理链路已就绪，当前包含截长图插件，并为 OCR/翻译/录屏保留扩展位

## 快捷键

| 快捷键 | 功能 |
|--------|------|
| `Ctrl+Alt+A` | 截图 |
| `Ctrl+Alt+Q` | 退出 |
| `ESC` / 右键 | 取消当前截图 |

## 构建

```powershell
# 需要 Visual Studio (MSVC) + CMake + Ninja
$vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" > nul 2>&1 && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build"
```

如需构建安装器，先完成上述 CMake 构建。CMake 会同时生成
`installer/bundled/nskry-scroll.nskryplugin`，再运行 Inno Setup：

```powershell
iscc installer/NskrySetup.iss
```

OCR、翻译和录屏仍属于后续插件规划，当前版本尚未实现。

## 技术栈

C++20 · Win32 API · D3D11 · Windows.Graphics.Capture · GDI+

## 文档

- [项目交接文档](docs/HANDOFF.md) — 完整架构、决策、构建说明
- [插件 SDK](sdk/nskry_plugin.h) — 插件开发接口
- [安装器构建说明](installer/README.md) — per-user Setup 与可选官方插件打包

## License

Nskry is licensed under the GNU General Public License, version 3 or later
(GPL-3.0-or-later). See [LICENSE](LICENSE) for the license notice and the
official license text.
