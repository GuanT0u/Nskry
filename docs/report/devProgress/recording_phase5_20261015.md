# Phase 5：nskry-record 首版实施与验证（2026-10-15）

## 结论与范围

独立、按需 `nskry-record` 0.1.0 已实现，包位于 `installer/bundled/nskry-record.nskryplugin`。
Core 与插件须使用本轮同套构建；没有安装到用户 Registry，没有停止用户进程，没有提交或 push。
验证目录为 `build/verify`，不覆盖用户可能正在运行的 `build/Nskry.exe`。

支持单屏固定物理区域 MP4/H.264、开始、暂停、继续、停止保存、取消丢弃，状态/有效时长/控制窗，
设置页自定义 Start/Stop（默认均空）。不依赖 OCR/Scroll，不含 GIF、声音、摄像头、剪辑或联网。
这是功能首版，不是所有硬件/故障/低配/长录制均已验收的发行声明。

## 1. 核对及分阶段构建

- 原 Phase 5 是 WGC → Media Foundation H.264 → MP4 的预选；历史 SDK/standalone 描述不是已实现帧流。
- 现有 SelectionWindow、metadata 工具栏、capture/launch 路由、Hotkeys/General、Registry/lazy runtime 可复用。
- CaptureSession 的 HWND 来源、callback 借用纹理与监控策略不适合直接当插件录屏 API。
  保留其原行为，单独提供通用 monitor region stream，使用新私有 device/context，不复用共享 immediate context。
- 第一阶段：可选 capture/commands v1、状态/时间轴/帧调度与纯测试，完整构建通过。
- 第二阶段：独立 MF encoder、录制 worker、控制窗与包，构建通过。
- 第三阶段：暂停重建、故障/取消/发布竞争、运行时/解码/真实 WGC 回归、规范与报告，构建并跑完整测试。
- 新代码按职责放于 `plugins/nskry-record/src/{record_policy,mf_encoder,record_session,nskry_record}.*`。
  主入口只接通服务与命令路由，没有把编码算法塞进 main 或增加重复管理器。

## 2. SDK / 兼容边界

`sdk/nskry_plugin.h` API 2 布局及 reserved 未改，standalone 仍预留。
新增独立 `sdk/nskry_capture.h` 与 `sdk/nskry_commands.h`，协商说明见
[扩展契约](../../plugins/recording_extensions_v1.md)。

- manifest `extensions.region_capture=1`：runtime 在 init 前调用可选 connect 导出，插件复制服务表。
  init 不创建编码器/设备/线程；缺服务/不支持版本拒绝加载。旧插件完全不需要导出这些接口。
- `extensions.commands=1` + shortcut `scope:command`：宿主只调已加载 DLL 的显式控制命令，未使用 Stop 不加载插件。
  全局键仍由宿主注册与配置；没有私装 hook。禁用后允许已有任务收到停止命令，不允许新的 execute。
- 局部 configure_input_v1/v2 兼容链不变；全局 capture/command 不作为插件局部绑定传递。
- Registry、设置与 DLL 生命周期只在主线程；capture/encoder/file 都在独立 worker。
  插件线程不调用宿主 UI 回调，不持有 execute context/位图，不访问共享 immediate context。

## 3. 来源、显示器与 DPI

录制含义是同一 monitor 内固定物理屏幕矩形，不是 sourceHwnd 隐藏窗口镜像。
窗口移动/缩小/关闭后，该位置的实际桌面会被录下；不会跟随窗口，也不会因 sourceHwnd 关闭而偷偷换采集语义。
跨屏直接拒绝；允许负坐标副屏。显示器身份/边界/DPI/设备状态改变时报错或在广播时停止；暂停恢复前再次核对。
不自动搬迁选区、不自动接受中途源尺寸变化。尚未实际拔屏或改变系统 DPI 测试。

限制：输入每边≤3840、总像素≤3840×2160，monitor≤16,777,216 像素。
输出近似等比缩小、偶数对齐，最长边≤1920、短边≤1080；缩小后不足 2px 的极细区域拒绝。
控制窗采用 WDA_EXCLUDEFROMCAPTURE，失败就不开始；真实录制样本已验证没有测试控制窗颜色。
保留系统捕获指示，不绕过操作系统权限；受保护内容不保证可录。

## 4. 时间戳、背压与资源

- 默认最高 20fps，输出码率范围 0.5..8Mbps（目标平均码率，不承诺瞬时码率上界）。
- 一个按需 worker，两张有界 BGRA 缓冲，一个输入 NV12 sample；WGC pool=2，CPU staging 只含选区。
  插件不建立生产者无界队列，不补发追帧 burst；保留 sink writer 默认背压。
- 使用 steady_clock 的有效 100ns 时间轴；首帧为零，前一帧 duration 延伸到下一次实际提交。
  慢采集/编码会降动态帧率，不缩短视频时长；暂停区间不写视频空白。
- 暂停关闭 WGC/私有 capture device，worker 条件变量等待；保留编码状态和文件便于继续。
  新系统尝试 WGC MinUpdateInterval；旧系统仅限消费，生产频率可能高于 20fps。
- 优先 hardware transforms，通过实际 encoder 硬件 URL 属性确认；未知时明确 not confirmed。
  初始化失败重试软件路径，录制中失败不热切换编码器。已分别测试本机硬件路径及显式软件路径。
  支持 ICodecAPI 时请求 2 个编码 worker；不宣称能限制所有厂商/Windows 辅助线程池。
- 停止/取消后释放本插件拥有的线程、COM、GPU、帧池、文件；Done 只在完成清理后发布。
  UI 在任务完成前不 join，不持有文件发布锁等待磁盘，不常驻启动采集或联网。

## 5. 文件、取消与异常

随机 GUID partial 位于目标目录，FAIL_IF_EXIST 创建，不覆盖其它文件。
Stop：写最后一帧 → sink Finalize → 释放文件/编码器 → 核对取消 → Publishing → 无覆盖 rename。
Publishing 是明确不可取消的提交点，之前的取消优先；文件 I/O 不在 UI 状态锁内。
首帧前停止清理空文件并提示未保存。Cancel/进程正常退出会协作停止、join，再删除自己拥有的 partial。
完成后 timer 停止、worker join；只保留无采集/编码的状态窗，关闭或 shutdown 释放窗口类。

显示布局/休眠广播请求异步停止；读回错误、设备丢失、编码/输出/Finalize 失败进入失败并保留路径。
最终路径被其它程序占用时不覆盖，已 finalize 的 partial 可人工改名；失败 partial 不一定可播放。
没有崩溃恢复索引、碎片化 MP4 或断电修复，异常终止不能保证文件可恢复。

重要限制：WGC Map、MF WriteSample/Finalize 和驱动同步调用没有可靠硬取消 API。
正常操作不阻塞 UI，但退出必须 join；驱动永久挂起时可能等待。首版不是进程隔离方案，
不能宣称已通过永久驱动挂起/有界退出验收。需要硬截止退出时，应后续单独实现一次性工作进程隔离。

## 6. 自动验证

完整 Release 构建成功；最终完整 CTest **25/25 通过，28.06s**（原有 19 项与新增 6 项）。
最终命令：`cmake --build build/verify --config Release`、`ctest --test-dir build/verify --output-on-failure`。
`git diff --check` 通过；最终包包含 manifest、DLL 和三份许可文件，未安装。
完整测试含系统快捷键、WinHTTP 本机 peer 和 WGC，须普通桌面权限；沙箱下 WGC 返回 E_ACCESSDENIED，
已按批准权限运行，不把该拒绝误认为采集引擎通过。

新增：

1. `NskryRecordPolicyTests`：重复开始、有效时间、1000 次快速暂停/继续、停止/取消/发布、调度及输入边界。
2. `NskryRecordEncoderTests`：40 帧、2.000 秒 H.264 MP4；SourceReader 真正解码 RGB，40 帧且时间戳递增；
   拒绝覆盖已有文件；显式软件编码另一路通过。
3. `NskryRecordSessionTests`：模拟有所有权的帧源，但使用真实编码器。
   覆盖暂停无采集/时长冻结、恢复、快切换、取消、退出、首帧前停止、停止/关闭竞争、120ms 慢帧背压、
   设备错误注入、输出目录失败、capture 同线程与资源归零、UI 消息继续派发。
4. `NskryRecordCaptureTests`：真实 monitor WGC→crop→H.264→MP4→RGB 解码，测试自有小窗口，
   暂停 650ms 后换颜色继续；两阶段颜色均存在、控制窗颜色为零、MP4 时长匹配有效时间。
5. `NskryRecordPluginTests`：实际 DLL 元数据发现不加载、未安装服务拒绝、按需 init 不调用采集、空 Stop、卸载清理。
6. `NskryRecordControlTests`：静态链接实际控制 UI，模拟自有帧源，实测按钮暂停/恢复/保存/丢弃、窗口排除属性、
   关闭确认期间 Stop 完成时仍持有 session，离开嵌套消息循环后才回收，递归关闭不重入。
   测试 seam 仅编入测试程序，不进入交付 DLL 或公开 SDK。

既有 manifest/runtime 测试补充 command scope/version、未声明命令、未加载不执行与禁用后停止路径。
没有以这些测试冒充真实用户逐项操作或低配验收。测试视频与故障 trace 保留在忽略的 build/verify 内。
曾发现并修复：Finalize 后重复刷新已关闭 byte stream 导致误报失败；测试等待瞬时 inRead 标志易错，
改为等待持久读取计数，避免把测试竞态当产品错误。

## 7. 开发机实测（短样本，不是低配结论）

Windows build 26200，i7-12650H、16 logical CPUs、约 15.78GiB RAM，DXGI 报告 RTX 4060 Laptop GPU。
真实样本为 320×180，nominal 20fps，实际确认 Hardware H.264；不能外推 1080p/4K/低配。
一轮代表性短测：总墙钟 2532ms（含初始化/暂停/收尾），进程 CPU 累计 390ms；
650ms 暂停段 CPU 计数为 0ms，停止并 join 后安静 500ms CPU 计数为 0ms（均是采样粒度内，不是绝对零）。
有效时间 1.4127882s，MP4 1.4127500s；22 帧完整解码，前后颜色 12/10 帧，控制色 0 帧，UI tick=53。

| 测量点 | private bytes | working set | process handles |
| --- | ---: | ---: | ---: |
| 冷启动测试基线 | 2,215,936 | 11,894,784 | 213 |
| 录制中 | 109,494,272 | 72,339,456 | 750 |
| 暂停 | 106,414,080 | 70,193,152 | 652 |
| 停止并 join | 63,295,488 | 45,936,640 | 460 |

这是独立测试进程总量，包含系统 DLL/线程池/驱动缓存，不是插件分配量或 Nskry 全应用预算。
峰值 working set 74,694,656 bytes。停止后没有回到冷启动基线，必须如实保留该限制。
已释放所有本轮拥有的会话/编码器/线程并观察无持续 CPU；没有据此证明长期缓存或 GPU 显存完全归零。

低配验收尚未进行。建议真实基准而非品牌分档：2C/4T 低压 x64 CPU、4GB RAM、核显、无可用硬件编码和有硬件编码
分别测 720p/20fps、1080p/20fps，记录 CPU-ms/墙钟、有效 fps、RAM/commit 峰值与停止后、暂停功耗、
开始/停止延迟、30min 样本/播放时长及 UI 延迟；不得用高配绑核代替低配实机。

## 8. 仍需实机 / 发行门禁

- 真实用户完整框选→另存为→控制按钮/全局键→关闭确认体验；多屏负坐标、不同 DPI 的切换。
- 屏幕断开、实际休眠恢复、锁屏/安全桌面、显卡驱动 reset/HDR/受保护内容。
- 真正磁盘满、网络盘阻塞/权限中途撤回、MF Finalize 失败；已覆盖普通输出失败，不等于覆盖这些设备故障。
- 30min/数小时/大文件的索引内存、句柄/GPU 增长趋势，低配持续吞吐与软件 fallback 实机。
- 硬件初始化失败后自动重试路径、未知编码器属性的设备矩阵；本机只验证正常硬件及显式软件。
- 长时间同时截图/Settings/PiP/OCR/Scroll 的体验；现有回归均通过，但未做全部功能同时开录制的人工压力测试。
- Setup 脚本新增存在包才显示的 record 组件，未编译/执行安装器。官方签名与官网仍未上线。

## 9. 依据

实现参考 Microsoft 官方文档：
[Sink writer](https://learn.microsoft.com/en-us/windows/win32/api/mfreadwrite/nf-mfreadwrite-mfcreatesinkwriterfromurl)、
[硬件 transforms 开关](https://learn.microsoft.com/en-us/windows/win32/medfound/mf-readwrite-enable-hardware-transforms)、
[实际 transform 查询](https://learn.microsoft.com/en-us/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsinkwriterex-gettransformforstream)、
[默认背压](https://learn.microsoft.com/en-us/windows/win32/medfound/mf-sink-writer-disable-throttling)、
[H.264 worker 设置](https://learn.microsoft.com/en-us/windows/win32/medfound/codecapi-avencnumworkerthreads)、
[WGC monitor source](https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createformonitor)、
[窗口排除](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowdisplayaffinity)。
公开数据和代码实测各自注明，不将“允许硬件”或“能编译”等同实际硬件编码/可播放。
