# Screen recording 0.1.1

独立按需插件；无需 OCR、Scroll 或外置运行时。Windows WGC + Media Foundation，离线、无声音。
许可 GPL-3.0-or-later，随包附 LICENSE/COPYING/PLUGIN_EXCEPTION。源码与构建资料在此仓库。

## 使用

构建后在 Settings → Plugins → Browse 安装
`installer/bundled/nskry-record.nskryplugin`，按设置页提示重启宿主。
本轮开发不会自动安装到用户环境。使用同轮构建的 Core，不能只把 DLL 放进旧宿主。

框选后点击 Screen recording，选择一个**不存在的 MP4 文件名**即开始录制。
也可在 Hotkeys 的插件分区设置 Start/Stop（均默认空）；Start 框选后直接录制，Stop 停止保存。
悬浮控制窗显示状态/有效时长/编码路径，提供 Pause/Resume、Stop/save、Cancel/discard。
关闭录制窗会询问保存/丢弃/继续；应用退出丢弃未完成录制。
控制窗通过系统 capture-exclusion 排除，失败则不开始；保留 Windows 捕获指示。

## 范围与资源

录的是单个显示器内**固定物理坐标**，不是跟随窗口。跨屏拒绝；源窗口最小化/关闭后录该位置实际桌面。
选区最多 3840×2160 总像素，单边最多 3840；输出近似等比缩小，最长边≤1920、短边≤1080、偶数对齐。
最高目标 20fps，目标平均码率不超过 8Mbps（不保证瞬时码率上界）；实际吞吐受机器、驱动和磁盘影响。
首版没有 GIF/音频/摄像头/剪辑。

插件 init 不预热。按需一个 worker、两张 BGRA 缓冲、一帧 NV12 sample，WGC 双缓冲，保留系统 sink writer 背压。
慢编码不追帧排队；用真实有效时间给前一帧设置 duration，可能降低动态流畅度，但不加速视频。
录制时使用会话私有的高精度等待计时器；不修改全局计时精度，不忙等，不新增常驻轮询。
暂停关闭采集、取消计时并等待命令事件；编码器/输出保留以便继续，不把暂停时间写进视频。
硬件配置优先，仅当查询到实际 encoder 的硬件标识才显示 confirmed；否则明确 hardware not confirmed。
初始化失败会重试软件编码；编码中失败不热切换时间轴，保留错误与部分文件。
停止后关闭本插件拥有的采集/编码/线程/COM 引用；进程总资源不一定回到冷启动基线。
本机硬件编码连续三轮停止后的句柄数仍有增长，原因未定位，不能归因于正常缓存或宣称回收完全通过。
软件路径两轮停止后的句柄数稳定；这些不是低配或长录制结论，见实测报告。

## 文件与关闭边界

输出目录内创建随机 `.nskry-record-{GUID}.partial.mp4`，不覆盖现有文件。
Stop：写最后一帧 duration → Finalize MP4 → 释放编码器/文件 → 再次核对取消 → 发布为目标路径。
发布阶段短暂进入不可丢弃状态，避免保存/取消竞争；文件 I/O 不持有 UI 状态锁。
Cancel/退出：协作取消、释放资源后只删除自己创建的临时文件；删除失败会保留错误路径。
目标文件在录制中被其它程序创建时不覆盖，保留已封装的 partial，可自行重命名恢复。
采集故障会尝试对已成功编码的片段收尾一次，成功时明确提示“不完整但已封装”的 partial 路径，
仍显示失败，不冒充完整保存。恢复收尾前仍可取消丢弃，提交保留决定后不能再取消。
已失败的 Finalize 不重试；编码/磁盘故障可能留下不可播放的 partial，并明确提示路径。
这不是文件修复；崩溃/强制结束/断电不保证可恢复，无自动修复。

显示器布局广播、休眠广播触发异步 Stop/save；读回发现显示器/DPI/设备变化则失败关闭。
MF/显卡驱动的同步调用本身没有硬取消保障：正常 UI 不等待它们，但宿主退出必须 join，
若驱动永久挂起，退出可能等待。硬超时隔离 worker 进程属于后续可靠性增强，不宣称本版已实现。

测试与未验收场景见 [首版报告](../../docs/report/devProgress/recording_phase5_20261015.md) 和
[0.1.1 可靠性与资源复测](../../docs/report/devProgress/recording_hardening_20261015.md)。

可选基准不会随普通构建或 CTest 自动运行。需在开发环境显式构建并运行：

```powershell
cmake --build build/verify --config Release --target NskryRecordBenchmark
& build/verify/NskryRecordBenchmark.exe build/verify 1920 1080 5 hardware 3
& build/verify/NskryRecordBenchmark.exe build/verify 1280 720 5 software 2
```

参数依次为已有输出目录、宽、高、有效录制秒数、编码偏好、轮数；宽高须偶数，总像素≤1920×1080。
基准使用合成帧与真实 MF 编码，不采集用户屏幕；每轮暂停 1 秒，结束后观察 1 秒，再统一解码输出。
视频与 CSV 留在新建的独立子目录。不能将其吞吐或内存当成真实 WGC/完整宿主的测量。
