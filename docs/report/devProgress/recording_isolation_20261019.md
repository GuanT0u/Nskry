# nskry-record 0.2.0：编码隔离、快捷键与性能验证

日期：2026-10-19。接续 [0.1.1 报告](recording_hardening_20261015.md)。

结论：独立编码、超时/取消/异常父进程清理、暂停快捷键、关闭确认与颜色修复已实施。
最终完整 Release 构建成功，CTest **29/29 通过，35.02 秒**。不代表全部硬件、低配和系统故障均已验收。

## 1. 范围及核对结论

本轮针对已确认的 MF 硬件编码反复启停后宿主句柄增长、原生编码调用不可取消、转换成本和回归可靠性继续实施。
Core/插件规范、原 Phase 5、采集/命令 v1 和上轮代码/测试均已核对；没有把 standalone 预留位当成现有能力。
实现按编码最小链路 → 进程故障回归 → 快捷键/帧格式保护 → 性能优化 → 完整回归分阶段构建。

交付为独立 `nskry-record` 0.2.0，包内新增 `nskry-record-worker.exe`。
不依赖 OCR/Scroll，不增加音频、GIF、摄像头、剪辑、联网、后台服务或额外管理器。
Core 和公开 SDK 均未修改；API 2 布局/reserved、capture/commands v1 版本与语义保持不变。
暂停/继续新增为 manifest 声明的 command，默认空，仍由宿主统一注册和配置快捷键。
旧的 Start/Stop ID 及用户配置保持兼容。第三方插件不能依赖私有 worker/头文件作为前置服务。

构建使用 `build/verify`，不覆盖用户可能运行中的 `build/Nskry.exe`，不自动安装用户插件、不结束用户程序。
本轮允许本地 commit，不 push；生成包在 `installer/bundled/nskry-record.nskryplugin`，二进制保持忽略，不加入源码提交。

## 2. 关键实现与自行审查

### 2.1 职责、线程和帧所有权

| 位置 | 职责 / 生命周期 |
| --- | --- |
| 主 UI 线程 | 按钮/快捷键/状态；不调用编码或等待文件收尾；插件生命周期不跨线程 |
| RecordSession 的按需线程 | 同线程调用宿主采集 open/read/close，拥有 CPU BGRA 两缓冲、有效时间轴和帧调度 |
| IsolatedEncoder | 创建一次性 Job/编码进程，转换 NV12，单请求/应答；负责取消、超时和关闭确认 |
| 包内 worker | 自有 MTA、MF、编码器和文件；没有宿主回调、GPU texture 或 immediate context |
| MfEncoder | 既有硬件优先/软件初始化回退、实际 backend 查询、NV12 sample、MP4 收尾 |

仍是单屏固定物理矩形，不追踪 HWND，不悄悄改成窗口捕获。多屏/DPI/尺寸改变的拒绝和失败策略不变。
共享内存仅有一个 NV12 槽，最大 3,110,400 bytes，外加固定头；没有编码帧队列或管道无界积压。
采集线程持有该槽直到请求发出，子进程复制到自己拥有的 MF sample 后应答，父线程才复用槽。
等待出错后标记 channel 不可复用，不能覆盖子进程仍可能读取的数据。
宿主仍保留 sink writer 默认背压；没有宣称能精确限制所有 MF/驱动内部队列。

### 2.2 进程边界与关闭

- init 不创建 worker、采集、模型或编码器；每次录制创建新编码进程，停止/取消后退出，不常驻复用。
- EXE 从包含代码的 DLL 所在目录解析，不搜索 PATH、当前工作目录或环境变量覆盖路径。
- 仅继承匿名 mapping/request/response 三个句柄，不传宿主函数指针或借用对象。
- 创建时使用明确 handle list 和原子的 Job list，进程在 Job 内才恢复执行；Job 不继承给子进程。
- Job 使用 kill-on-close、最多一个进程、512MiB 进程提交量限制；不代表 GPU 显存上限、低配预算或安全沙箱。
- 私有协议核对 magic/version/header size、帧尺寸/长度、请求序号和返回序号；格式限制共用领域纯逻辑。
- 单个编码请求最多等待 15 秒，取消事件优先于应答；等待只发生在录制线程，不阻塞主消息循环。
- 正常关闭给 worker 协作退出机会；失败时仅终止自己创建的 Job，退出等待有时限。
  只有观察到进程已退出才允许正常发布；退出未确认时报错，不假报资源已释放或取消成功。
- 失败/取消 Open 时，待 child 关闭后再次核对已创建文件标记，防止创建/取消竞争漏掉自己的临时文件。
- File I/O 不持 UI 状态锁。仅操作自己创建的 GUID partial，不覆盖已有目标；取消/恢复提交点规则保持不变。
- MF 正常 Finalize 才封装和发布；故障恢复仍至多尝试一次，失败可能留下不可播放的 partial。
  被迫结束 worker 不是文件修复；崩溃、断电和不完整索引不保证可恢复。

这是**编码隔离**，不是完整采集隔离：WGC/D3D/设备关闭仍在宿主采集线程，永久原生采集阻塞可能拖延退出。
操作系统内核或磁盘永久阻塞时，也不能保证 Terminate 后所有系统资源立即消失；代码检查退出结果并如实失败。
硬件资源增长根因没有被证明已修复，隔离只是防止其随每次录制留在长期运行的宿主中。
没有用脱离线程、强制卸载 DLL 或新常驻看门狗来掩盖问题。

### 2.3 性能与功能

- NV12 传输量是同尺寸 BGRA 的 3/8；共享槽直接作为转换输出，避免额外 BGRA 中间复制。
- 转换按两行四像素使用 x64 基础 SSE2，不要求 AVX/现代 CPU；尾部两列和非 SSE2 构建有标量路径。
- 使用 BT.709 limited-range 整数公式与匹配的完整 SDR 标记；2×2 色度平均使用 floor(sum/4)，不使用改变舍入的 pavg。
- 保留 20fps 目标、真实时间戳、暂停不计时、单 pending sample、慢帧不追赶的背压策略。
- 暂停关闭采集，父线程等待命令事件，worker 等待请求事件；没有新增暂停或 idle 轮询。
- 新增可配置 Pause/resume 快捷键，默认空；空闲时调用不启动进程/采集，也不会冒充执行成功。
- 关闭确认改为原 capture-excluded 控制窗内的 Save/discard/back，避免独立 MessageBox 的排除效果未经验证。
  录制期间不再为关闭确认创建顶层弹窗；出错提示只在采集结束后显示，并防止提示期间重入启动录制。
- 恢复采集时重新验证 version/structSize/尺寸/stride；变化先失败，不向旧缓冲读入不兼容格式。
- 跨进程前规范化输出绝对路径，避免子进程工作目录不同导致相对路径写错位置；包含中文/空格的相对路径有回归。
- 状态快照保存采集/编码的最大墙钟跨度，不记录像素或文字；CSV 可用于定位掉帧区段，但跨度不是 CPU 或根因归属。

自行审查后，未加入重复任务管理器、额外插件依赖、跨进程共享 D3D 或无用途的预留抽象。
MF 格式校验从私有 IPC 协议下移到 `record_policy.h`，避免底层编码器反过来依赖通信布局。
采集 v1/公共 ABI 和未来接口不因这次优化删除或借用 reserved。
MSVC `/W4 /analyze` 检查了 isolated encoder、session、worker、MF encoder 和控制入口五个模块，
修复未检查 StringFromGUID2 返回值、约 64KiB 的 worker 入口栈复制和保存对话框路径栈缓冲后，复查无警告。
硬件回退重建 partial 时同步更新创建标记；取消清理把已经不存在的自有临时文件当作已清理，而非误报失败。

## 3. 构建及测试

```powershell
cmake --build build/verify --config Release --target all NskryRecordBenchmark
ctest --test-dir build/verify --output-on-failure
ctest --test-dir build/verify --output-on-failure -R NskryRecordStorageFailureTests --repeat until-fail:10
& build/verify/NskryRecordWireTests.exe --benchmark
```

新增三个正式 CTest 项，原有测试保留：

- `NskryRecordWireTests`：协议格式/大小/版本/长度/极值/句柄解析；随机像素和不满 SIMD 宽度与独立标量参考逐字节对照，输出尾部 canary 不变。
- `NskryRecordColorTests`：高清及小尺寸、硬件/软件偏好路径；MP4 色彩矩阵标记和实际 RGB 解码色块误差均验证。
- `NskryRecordProcessTests`：真实 child 正常编码、中文/空格及相对输出路径、错误时间戳、缺 worker、异常退出、Frame 卡住、Finalize 卡住、取消；检查所创建 child 退出。
  故障 seam 只编入专用测试 helper，不编入交付 worker，也不进入 SDK。
- 父程序异常退出回归：外层测试保留自己的安全 Job，明确结束所创建的测试父进程，
  再确认编码进程由内部私有 Job 清理，而不是靠最后关闭外层 Job 才消失。独立连续三轮通过。
- `RecordSessionTests` 补恢复时错误 stride 被拒绝；原有取消、背压、S_FALSE、恢复片段实际解码和文件冲突仍保留。
- `RecordPluginTests/RecordControlTests` 补 Pause/resume 元数据、空闲失败与实际命令派发；原有关闭重入与采集排除保留。
- 真实 WGC 自有 320×180 窗口仍进行暂停换色、MP4 实际 RGB 解码、时间匹配及控制窗颜色不泄漏验证。

测试过程发现并处理一项旧测试竞态：原“收尾写入失败”在提交帧后直接让所有写入失败，
可能误伤 MF 尚未处理的异步帧，导致原生同步 Finalize 卡住。首次完整测试及一次定位复跑均出现 20 秒超时，未忽略。
现在收尾场景准确拦截真实 MP4 `moov` 索引写入，配额耗尽场景仍覆盖普通写入失败；原时限没有放宽。
修正后独立连续十轮通过，均实际发生注入、未标记封装成功、测试流归零。
原生调用可能挂起的行为没有被“修复测试”消除，产品另外通过独立进程和超时/取消边界处理，并专测 Finalize 挂起。

完整测试需要普通桌面权限（真实 WGC、快捷键和本机网络 peer），不以沙箱拒绝当采集通过。
没有据这些测试宣称手动用户工作流或全部设备均通过。
最终包内版本为 0.2.0，共六项（manifest、DLL、私有 worker 和三份许可文件），
归档 DLL/worker SHA-256 均与独立验证构建一致；差异格式检查通过。

颜色复核先复现而后修复：原 1080p 输入 BGR=(20,30,180)，MF 解码硬件偏好得到 (16,44,192)，
软件得到 (18,44,190)。仅补 BT.601 tag 不足以修复本机 MF RGB 路径；独立 FFmpeg 解码能正确使用该 tag，
说明不能把该问题简单归因为像素转换或只看元数据就宣布解决。
最终统一 BT.709 像素与完整 matrix/range/primaries/transfer 标记，1920×1080 和 320×180 均实测：
硬件偏好 (19,31,180)，软件 (22,31,178)，均在每通道 ±8 的有损编码验收范围内。
没有通过给测试解码器硬塞相反矩阵来掩盖偏色；无效的 codec 色彩属性尝试已撤回。

## 4. 性能数据与可复跑方法

测试机与历史报告相同：i7-12650H、16 logical CPUs、约 15.78GiB RAM、RTX 4060 Laptop，Windows build 26200。
不是低配实机；软件路径不代表在低配硬件上运行。SSE2 不抬高现有 x64 最低指令集要求，但不能据此宣称低配已验收。

```powershell
& build/verify/NskryRecordBenchmark.exe build/verify 1920 1080 5 hardware 10
& build/verify/NskryRecordBenchmark.exe build/verify 1280 720 5 software 5
& build/verify/NskryRecordBenchmark.exe build/verify 1920 1080 1800 hardware 1
```

以上资源基准是**合成 CPU 帧 + 真实 MF 编码/MP4 解码**，不是 1080p WGC 吞吐或全宿主总资源测试。
CSV 分别保存宿主与编码进程 CPU-ms/private/working bytes/handles；总成本不能只看宿主。
worker CPU 完成值包括子进程退出前的 CPU 时间。进程工作集相加可能重复计算共享页，不能当物理 RAM 精确总量。
各轮统一完成录制后再解码，避免解码器缓存污染后续录制基线。

### 转换微基准

最终 BT.709 的 1080p、100 次转换，独立三次：

| 轮次 | 标量参考 ms | SSE2 ms |
| --- | ---: | ---: |
| 1 | 543.186 | 150.772 |
| 2 | 512.444 | 144.149 |
| 3 | 515.731 | 146.437 |

全部输出逐字节相等。
这是同进程转换墙钟微基准，不是端到端录屏加速比例。该模式仅显式 `--benchmark` 运行，不增加应用或普通 CTest 的工作。

### 30 分钟持续测试

该长测使用的是完成编码隔离、但尚未加入最终颜色修复及退出诊断/UI 收尾的候选构建；正常采集/单槽/编码架构相同。
期间存在短暂构建、静态分析与父进程故障回归负载，不是独占静默机器的吞吐基准。
最终构建另对保留视频全帧重验；最终色彩与诊断实现另外进行短测，不冒称又完整跑了一次最终版本 30 分钟。

| 项目 | 结果 |
| --- | --- |
| 有效录制 / MP4 时长 | 1800.0196203 / 1800.0196000 秒 |
| 完整解码帧 | 34,750（约 19.3fps，非全程 20fps） |
| 宿主 / 编码 CPU 累计 | 89,000 / 62,203 ms，包含启动及收尾，不含后续解码 |
| 宿主录制中最大采样 private / working | 18,288,640 / 28,401,664 bytes |
| 编码进程最大采样 private / working | 109,195,264 / 81,956,864 bytes |
| 宿主停止后 private / working / handles | 1,507,328 / 8,634,368 bytes / 144 |
| 编码进程停止后 | 已退出；不保留常驻编码进程 |
| 文件大小 | 72,368,028 bytes |

宿主录制期间 handles 保持 157，编码进程末段 private 约 97.7MB；不能将小幅增长直接归因于正常缓存，
更不能据此保证数小时录制/GPU 显存归零。停止后 1 秒安静段 CPU 增量为 0ms（计数粒度内）。
慢间隔计数 77，说明机器有真实吞吐波动；视频仍保留真实时长，不通过加速视频隐藏掉帧。

### 最终构建短测

颜色、关闭确认、退出检查、路径及耗时诊断全部进入最终构建后，再独立运行硬件十轮和软件五轮。
各轮中途暂停 1 秒，停止后观察 1 秒；所有视频检查 EOS、完整帧数、严格递增时间戳、首帧时间为零及有效时长。
最终工具也重新完整验证长测视频：34,750 帧，duration100ns=18,000,196,000，与原有效时间匹配。

| 指标 | 1080p 硬件十轮 | 720p 软件五轮 |
| --- | --- | --- |
| 每轮约 5 秒的完整解码帧 | 98–99 | 99 |
| 启动至停止的宿主+child CPU-ms / 轮 | 734–1,000 | 453–671 |
| 录制期间宿主最大采样 private bytes | 18,362,368 | 9,121,792 |
| 录制期间 child 最大采样 private bytes | 95,981,568 | 81,137,664 |
| 所有轮次停止后宿主 handles | 全为 144 | 全为 144 |
| 最后一轮停止后宿主 private / working bytes | 1,536,000 / 8,818,688 | 1,576,960 / 8,753,152 |
| 最大采集调用墙钟跨度 ms（合成帧） | 4.8200 | 2.7119 |
| 最大转换+编码应答墙钟跨度 ms | 20.2624 | 17.8559 |

暂停及停止后的安静段，两条路径所有轮次均测得宿主/child CPU 增量 0ms（计数粒度内）。
编码进程均退出；最终十轮没有复现前期 37 帧的极低吞吐轮次，不能因此否定前期异常或保证永不掉帧。
前期候选确实出现过 37/约5 秒的轮次，其时长正确但流畅度不足；缺少当轮调用跨度数据，根因未确定。
新增诊断只提供墙钟定位，不把未知原因武断归于硬件、Windows 或用户机器。
硬件/软件分辨率不同，不据表格推断哪一种编码一定更省资源；低配和长时真实 WGC 吞吐仍未验收。

### 原始证据

- `build/verify/record-benchmark-8304-301736515/metrics.csv`：隔离后、SIMD 前的硬件十轮，停止后 handles 全为 144。
- `build/verify/record-benchmark-13020-302566453/metrics.csv`：SIMD 后硬件十轮。
- `build/verify/record-benchmark-30976-302492812/metrics.csv`：SIMD 后软件五轮。
- `build/verify/record-benchmark-4116-302646500/metrics.csv`：30 分钟合成帧持续测试。
- `build/verify/record-benchmark-33292-304778281/metrics.csv`：最终完整构建硬件十轮，包含调用跨度。
- `build/verify/record-benchmark-32336-304858484/metrics.csv`：最终完整构建软件五轮，包含调用跨度。
- `build/verify/record-full-suite-20261019.log`：最终完整 29 项 CTest 日志留档。

这些生成数据和视频留在忽略的验证目录；源码提交包含复跑工具，不包含本机捕获数据或二进制。

## 5. 仍需保留的边界

- 编码隔离不等于修复 GPU 厂商驱动、完全隔离 WGC 或保证全系统内核状态；未测 GPU 显存归零。
- 真实低配 2C/4T、4GB RAM、核显的硬件/软件路径仍需设备验收；不以绑核或本机软件编码替代。
- 真实拔屏、多 DPI 切换、锁屏/安全桌面、休眠、HDR、驱动 reset、物理磁盘满/网络盘卡死仍需实机。
- 原生插件仍不是安全沙箱；Job/继承限额是生命周期与资源约束，不是抵御恶意插件。
- 当前不引入 GIF/音频/摄像头/剪辑；后续新增来源/编码需独立定义时间轴和资源预算，不能悄悄更改 capture v1。

参考 Microsoft 官方 [进程创建与继承](https://learn.microsoft.com/en-us/windows/win32/procthread/creating-processes)、
[属性列表](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattribute)、
[等待多个对象](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects)。
颜色边界参考 [YUV 矩阵](https://learn.microsoft.com/en-us/windows/win32/medfound/mf-mt-yuv-matrix-attribute) 与
[标称范围](https://learn.microsoft.com/en-us/windows/win32/medfound/mf-mt-video-nominal-range-attribute)；
本机另用已安装的 FFprobe 确认最终 MP4 为 tv range、BT.709 matrix/transfer/primaries。FFmpeg 工具只用于本机验证，不成为插件依赖。
实现事实和性能数字以本仓库代码及本轮实测为依据，不把系统 API 的存在当作验收结果。
