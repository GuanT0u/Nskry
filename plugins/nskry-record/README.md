# Screen recording 0.2.0

独立按需插件；无需 OCR、Scroll 或外置运行时。Windows WGC + Media Foundation，离线、无声音。
许可 GPL-3.0-or-later，随包附 LICENSE/COPYING/PLUGIN_EXCEPTION。源码与构建资料在此仓库。

## 使用

构建后在 Settings → Plugins → Browse 安装
`installer/bundled/nskry-record.nskryplugin`，按设置页提示重启宿主。
本轮开发不会自动安装到用户环境。使用同轮构建的 Core，不能只把 DLL 放进旧宿主。

框选后点击 Screen recording，选择一个**不存在的 MP4 文件名**即开始录制。
也可在 Hotkeys 的插件分区设置 Start、Stop 和 Pause/resume（均默认空）；Start 框选后直接录制，Stop 停止保存。
悬浮控制窗显示状态/有效时长/编码路径，提供 Pause/Resume、Stop/save、Cancel/discard。
关闭录制窗会在该控制窗内显示保存/丢弃/返回确认，不新增可能入镜的独立弹窗；应用退出丢弃未完成录制。
控制窗通过系统 capture-exclusion 排除，失败则不开始；保留 Windows 捕获指示。

## 范围与资源

录的是单个显示器内**固定物理坐标**，不是跟随窗口。跨屏拒绝；源窗口最小化/关闭后录该位置实际桌面。
选区最多 3840×2160 总像素，单边最多 3840；输出近似等比缩小，最长边≤1920、短边≤1080、偶数对齐。
最高目标 20fps，目标平均码率不超过 8Mbps（不保证瞬时码率上界）；实际吞吐受机器、驱动和磁盘影响。
首版没有 GIF/音频/摄像头/剪辑。

插件 init 不预热。按需一个采集/调度线程、一个独立编码进程；不用时没有编码进程。
两张 BGRA 缓冲、一个 NV12 共享槽（最多 3,110,400 bytes），WGC 双缓冲，保留系统 sink writer 背压。
先在非 UI 线程转换 NV12，再通过事件交付唯一槽；编码进程复制至自己的 sample 后应答，才允许复用槽。
转换在 x64 使用基础 SSE2，不要求 AVX/高端 CPU；与独立 BT.709 标量参考逐字节一致。
同时写入完整 SDR 色彩标记，修复原 BT.601 像素与高清默认解码矩阵不匹配时的偏色。
慢编码不追帧排队；用真实有效时间给前一帧设置 duration，可能降低动态流畅度，但不加速视频。
录制时使用会话私有的高精度等待计时器；不修改全局计时精度，不忙等，不新增常驻轮询。
暂停关闭采集、取消计时并等待命令事件；编码器/输出保留以便继续，不把暂停时间写进视频。
硬件配置优先，仅当查询到实际 encoder 的硬件标识才显示 confirmed；否则明确 hardware not confirmed。
初始化失败会重试软件编码；编码中失败不热切换时间轴，保留错误与部分文件。
编码器和 MF/硬件驱动引用留在一次性子进程，停止或取消后该进程退出，不驻留复用。
子进程只继承三个匿名通信句柄，限制为单进程 Job、512MiB 进程提交量，不是安全沙箱或 GPU 显存上限。
0.1.1 的硬件句柄增长根因未定位；隔离后十轮短测未再在宿主累积，不等于修复了厂商驱动。
资源统计必须同时计入宿主和子进程；不能仅凭宿主录制中内存降低宣称总开销下降。

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
编码通信单次等待最长 15 秒，取消事件可提前打断；错误/超时后不再复用未完成的共享槽。
退出通常先关闭编码进程，失败时只终止本次创建的 Job，等待有时限，不结束用户其它程序。
父进程异常退出时 Kill-on-close Job 会清理编码进程；被迫终止时不保证 partial 可播放。
MF 已隔离，但 WGC 服务仍在宿主自有采集线程：D3D/系统采集关闭若永久阻塞，宿主退出仍可能等待。
这不是完整采集进程隔离，也不保证内核/磁盘永久卡死时资源立即消失。

测试与未验收场景见 [首版报告](../../docs/report/devProgress/recording_phase5_20261015.md) 和
[0.1.1 可靠性与资源复测](../../docs/report/devProgress/recording_hardening_20261015.md)，
最新实现见 [0.2.0 编码隔离与性能报告](../../docs/report/devProgress/recording_isolation_20261019.md)。

安装时须包含同包 `nskry-record-worker.exe`，不能只复制 DLL；缺文件会明确报错，不静默退回进程内编码。
该 EXE 和私有通信协议不是 SDK 或其它插件的可复用服务，第三方插件不得依赖它的文件/命令行。

可选基准不会随普通构建或 CTest 自动运行。需在开发环境显式构建并运行：

```powershell
cmake --build build/verify --config Release --target NskryRecordBenchmark
& build/verify/NskryRecordBenchmark.exe build/verify 1920 1080 5 hardware 3
& build/verify/NskryRecordBenchmark.exe build/verify 1280 720 5 software 2
& build/verify/NskryRecordWireTests.exe --benchmark
```

参数依次为已有输出目录、宽、高、有效录制秒数、编码偏好、轮数；宽高须偶数，总像素≤1920×1080。
基准使用合成帧与真实 MF 编码，不采集用户屏幕；每轮暂停 1 秒，结束后观察 1 秒，再统一解码输出。
视频与 CSV 留在新建的独立子目录；CSV 同时包含宿主及编码进程的 CPU/内存。
轮数范围 1..20，有效录制秒数 1..1800。转换微基准不随普通 CTest 运行。
不能将这些吞吐或内存当成真实 WGC/完整宿主/低配机器的测量。
