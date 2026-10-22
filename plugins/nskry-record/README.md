# Screen recording 0.4.0

独立按需插件，Desktop Duplication / WGC + Media Foundation / WIC，离线、无声音。不依赖 OCR、Scroll、FFmpeg 或外置运行时。
随包提供 GPL-3.0-or-later、COPYING 和 PLUGIN_EXCEPTION。

## 使用

安装 `installer/bundled/nskry-record.nskryplugin` 后按设置页提示重启。必须配合同轮构建的 Core（区域采集 v2）；
不能仅把 DLL 拷入旧宿主。开发验证不自动安装插件。

- 框选 → Screen recording → 30/60 fps、高清/高/中、可选 Follow selected window → Start。
- GIF 是工具栏上独立的 Record GIF，不在视频入口增加格式选择。
- 开始前显示实际选区红框，选项水平排列在选区下方；空间不足时放到上方或屏幕内。
- Stop 后可 Save、Copy file 或 Trim。Trim 可拖动时间轴首尾手柄，也保留起止秒数输入，Apply 后仍可保存/复制。
- 拖动手柄时按需显示对应帧预览，不自动播放。预览最多8次/秒、640×360，松手后回收解码任务。
- 裁剪是重新编码，不是无损关键帧剪切；有再次编码的耗时/质量损失。GIF 时间以百分之一秒量化。
- Settings → Plugins → 选中本插件 → Plugin settings，可设置默认质量和帧率；录前 Save defaults 同样有效。
- 默认 30 fps / High，私有配置位于 HKCU/Software/Nskry/Plugins/nskry-record，使用单个版本化 DWORD 原子写入。
- Hotkeys 中 Start、Stop、Pause/resume 均默认空；Start 框选后进入录前选项。闲置 Stop 不加载插件。

关闭录制窗时，保存/丢弃/返回的确认在 capture-excluded 控制窗内部显示。
应用退出取消活动录制/导出；已复制文件保留。

## 复制和文件生命周期

Copy file 发布标准 CF_HDROP 文件引用，不是图片像素。仅支持接收文件粘贴的目标软件，不能保证每个聊天软件都支持。
已复制文件保留在 `%LOCALAPPDATA%/Nskry/cache/recordings`，关闭/退出不删除，避免破坏后续粘贴及剪贴板历史。
未复制的当前录制在丢弃/关闭/退出时删除；保存复制到新的用户文件，不覆盖已有文件。

缓存超过 1 GiB 或 2048 项时拒绝开始下一次录制，提示先保存所需文件再手动清理。
这是开始前的保留量检查，不是单条 MP4 文件大小限制；没有后台清理/轮询。
录制、保存和裁剪使用随机自有临时文件，成功收尾后才发布。保存/裁剪在后台执行，可取消；
失败保留原录制，删除失败报告临时路径。崩溃/断电不保证 MP4 partial 可恢复。

## 来源与范围

默认是单个显示器内固定物理坐标；该模式下原窗口移动/最小化后仍录该位置桌面。
Follow 用窗口 WGC 源，保留相对 DWM 可见边界的选区，移动和暂停后移动会跟随。
改变窗口尺寸、最小化、关闭、跨显示器或 DPI 变化明确结束并尝试收尾，绝不静默换成桌面。
选区必须完整落在源窗口和同一显示器内，支持负屏幕坐标，跨屏初始区域拒绝。

输入总像素≤3840×2160、单边≤3840；视频输出最长边≤1920、短边≤1080，近似等比、偶数对齐。
控制窗及红色实际录制范围框都排除在采集中；红框穿透鼠标。
固定区域 v2 改用 Desktop Duplication，不创建显示器 WGC 会话，因此不出现它的全屏黄色边框。
窗口跟随仍使用窗口 WGC，Windows 可显示该窗口源的系统提示。没有绕过系统权限或全局关闭采集指示。
Desktop Duplication 不可用时明确失败，不悄悄改成其他来源。旧 v1 客户端维持原 WGC 行为。

## 参数与资源

视频目标 30/60 fps；实际帧率受机器、驱动、区域和磁盘影响，不保证每台机器达到目标。
高清/高/中目标码率为 `像素数 * fps / (4/7/12)`，限制 0.5–24 Mbps，不是瞬时硬上限。
优先硬件编码，失败退回软件；实际后端显示 confirmed / not confirmed，不承诺普遍硬编。
颜色保持 BT.709 limited NV12，x64 基础 SSE2 路径，无需 AVX。

GIF 固定 10 fps、最长边960/短边540、每帧最多256色、约两分钟；写帧前检查128 MiB文件上限。
最多256种颜色的帧使用精确调色板，否则采用采样面积最多256×256的自适应调色板并修正中性灰映射。
GIF 不提供帧率/码率配置；复杂场景仍存在GIF量化损失，不缓存全部动画。

init/设置页不预热采集或编码；活动录制只有一个采集线程、一个私有编码进程。
窗口WGC双缓冲；固定区域使用一个DXGI获取帧和选区staging纹理。两张BGRA、单个传输槽；视频NV12槽最多3,110,400字节，GIF BGRA槽最多2,073,600字节。
慢编码不补帧排队，真实有效时间写入 duration；暂停关闭采集并事件等待，不把暂停时间录成空白。
编码进程限制512 MiB提交量、单进程Job、只继承三个匿名通信句柄。不是恶意插件沙箱或显存上限。

Trim 在同类一次性编码进程中解码/重新编码，单帧缓冲，最长等待5分钟，可取消；
普通编码IPC单次15秒，取消可以打断。停止、取消、失败后回收线程、进程、COM/GPU/文件资源。
仅终止本次自建Job，不停止用户程序。WGC仍在宿主采集线程，驱动/内核永久卡死不保证及时退出。
不含音频、摄像头或多轨剪辑。

## 验证和历史

最新范围、兼容性、实测与未验收项见 [0.4.0报告](../../docs/report/devProgress/recording_ux_20261022.md)。
[0.2.0编码隔离实测](../../docs/report/devProgress/recording_isolation_20261019.md)是旧20fps配置，不能当作本版60fps/GIF资源数据。
私有 worker EXE 必须随包提供，不是公开 SDK 或跨插件前置。

可选合成帧基准（默认仍使用兼容的20fps测试配置，不代表完整UI/WGC）：

```powershell
cmake --build build/verify --config Release --target NskryRecordBenchmark
& build/verify/NskryRecordBenchmark.exe build/verify 1920 1080 5 hardware 3
& build/verify/NskryRecordWireTests.exe --benchmark
```
