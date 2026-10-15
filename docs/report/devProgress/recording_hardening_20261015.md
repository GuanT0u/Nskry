# nskry-record 0.1.1：可靠性增强与资源复测

日期：2026-10-15。接续 [Phase 5 首版报告](recording_phase5_20261015.md)。

## 结论

本轮完成失败片段收尾、取消竞争与存储故障回归、录制节奏修正和可复跑的资源基准。
Release 构建通过，完整 CTest **26/26 通过，25.35 秒**。
产物为 `installer/bundled/nskry-record.nskryplugin`，版本 0.1.1；没有自动安装、停止用户程序、提交或 push。

仍不能声称录屏已达到完整资源/可靠性发行门禁：本机硬件编码多轮停止后的句柄数连续增长，
归属尚未定位；没有证明它是正常缓存。驱动同步调用永久阻塞时的有界退出仍未实现。
这两项应优先进入下一轮，而不是继续添加音频/GIF 等功能掩盖风险。

## 1. 核对与边界

复核 Core/插件契约、开发规范、现有录屏报告、capture/commands v1、插件与测试后继续小范围实现。
Core 采集语义、框选/快捷键、API 2 布局及 reserved、本次已存在的可选 v1 接口均未修改。
仍是单屏固定物理区域；插件负责编码/录制控制，不依赖 OCR、Scroll 或新增管理器。
没有启动时编码预热、联网、后台常驻采样或全局计时精度调整。

本轮改动集中在 `plugins/nskry-record/`、录屏测试、交接及开发文档。
内部 `MfEncoder::OpenStream` 用于独立存储流及故障注入，不是公开 SDK，不引入插件前置。
`RecordSession` 的显式软件编码参数仅用于内部基准；用户默认仍优先尝试硬件。

## 2. 实施内容

### 2.1 故障恢复与所有权

- 失败时立刻冻结有效时长，进入 Recovering；不把关闭设备/收尾耗时计入录制。
- 关闭采集后，对已有成功编码样本执行至多一次 best-effort Finalize。
- 已失败的 Finalize 不重试，也不允许后续 WriteSample；不是文件修复或强行补索引。
- 封装成功仍报告录制失败，提示不完整片段和随机 partial 路径，不发布成完整目标。
- 已 finalize 后目标被占用/rename 失败，正确标记为可恢复片段，不覆盖目标。
- 恢复期间取消可覆盖保留决定；在主线程可见的不可取消提交点后才保留文件，文件 I/O 不持状态锁。
- 取消成功删除自己拥有的 partial 时不再误报失败；删除失败仍保留路径。
- Snapshot 记录成功编码末尾、恢复路径与是否已封装，UI 显示恢复状态。

错误恢复仅包含已经成功写入的前缀，可能少于失败前最后看到的画面。
未编码的尾帧不冒充已保存；强制结束、断电和原生驱动永久挂起仍不保证文件可播放。

### 2.2 帧与时序

- encoder 入口验证非负、正 duration、时间戳不重叠及 64 位加法不溢出。
- 去掉每次采样将 pending BGRA 全帧复制回 scratch 的操作；有效新帧交换两张缓冲，
  S_FALSE 直接重复已确认的 pending，不读无效 scratch 内容。
- 1080p 每次少一次 8,294,400-byte 拷贝，按 20fps 理论少约 158MiB/s 内存搬运；
  这只是复制量，不是实测 CPU 提升百分比。
- 测量发现原等待方式在本机目标 20fps 时约产生 81 帧/5 秒（约 16fps）。
  改用录制 worker 私有高精度 waitable timer，系统不支持则普通 timer 回退；命令通过独立事件唤醒。
- 暂停关闭采集、取消 timer，等待命令事件；无暂停轮询、忙等或全局 timeBeginPeriod。
  继续不累积暂停时间、不追帧突发、不放宽有界缓冲。
- timer 在 worker 内关闭，命令事件在线程 join 后关闭；完成前不卸载 DLL。

[Windows 高精度等待计时器](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw)
和 [MF Finalize 契约](https://learn.microsoft.com/en-us/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsinkwriter-finalize)
为实现参考；实际吞吐/错误结果以下文测试为准。

## 3. 测试与构建

分阶段构建：恢复/存储注入构建 → 聚焦回归 → 基准与缓冲优化构建 → 等待机制构建与聚焦回归。
最终完整构建包含交付插件包；可选基准单独显式构建，不属于普通 CTest。

```powershell
cmake --build build/verify --config Release --target all NskryRecordBenchmark
ctest --test-dir build/verify --output-on-failure
```

完整 **26/26 通过，25.35s**，包括既有 OCR、Core/插件、更新、编辑/监控与录屏回归。
真实 WGC、系统快捷键及本机网络测试按批准普通桌面权限运行；没有安装用户插件或停止程序。
测试进程使用自有窗口/临时输出，不能替代真实用户完整工作流或低配验收。

新增/补充验证：

| 测试 | 实际覆盖 |
| --- | --- |
| RecordPolicy | Recovering 冻结时长、禁止恢复录制、取消覆盖、保留提交点、时间戳溢出/重叠 |
| RecordSession | 模拟采集错误→真实 MF 封装片段→实际 RGB 解码；时长匹配成功编码前缀 |
| RecordSession | 中文/空格目录；录制中创建同名目标，目标字节不变、partial 可解码 |
| RecordSession | 故障关闭期间取消，完成后无输出/恢复路径、采集数归零 |
| RecordSession | S_FALSE 时 scratch 被覆盖为白色，重复帧解码仍是原有红色，不混入无效像素 |
| RecordControl | 自有窗口接收显示变化/休眠消息后异步保存，采集关闭；原暂停/保存/取消/嵌套关闭仍通过 |
| RecordStorageFailure（新增） | 真实 MF MP4 sink + 有界测试 IStream，注入 quota-full 与 Finalize 写失败 |

存储注入不是填满真实磁盘，也不把模拟广播当成真实拔屏/休眠验收。
测试流的配额不超过 2MiB；通过系统 byte-stream wrapper 对接真实 sink。
本次观察 quota-full 的 MF 错误为 `0x8000FFFF`、收尾写失败为 `0xC00D36BB`；
底层均实际注入 `STG_E_MEDIUMFULL`，不假定所有 MF/驱动都保留同一 HRESULT。
两例均有写失败计数、未被标记 Finalized，重复收尾/写入被拒绝，流对象计数归零。

最终完整测试日志在 `build/verify/Testing/Temporary/LastTest.log`。
`git diff --check` 通过；包内 manifest 版本为 0.1.1，DLL SHA-256 与独立验证构建一致，
包含 manifest、DLL、LICENSE、COPYING、PLUGIN_EXCEPTION 五项。没有安装到用户 Registry。
故障片段保留在测试独立 `record-session-*` 目录，便于复查，未清理用户文件。

## 4. 可复跑资源基准

`NskryRecordBenchmark` 为 EXCLUDE_FROM_ALL 的开发工具；不随应用启动、普通构建或测试自动运行。
输入合成 BGRA 帧，输出真实 H.264/MP4；每轮在有效录制中途暂停 1 秒，停止/join 后再静置 1 秒。
统一在所有录制轮次结束后解码，避免解码器缓存污染下一轮录制基线。
CSV 含进程 CPU-ms、private/working bytes、handles、帧数、慢间隔及有效时长。

```powershell
& build/verify/NskryRecordBenchmark.exe build/verify 1920 1080 5 hardware 3
& build/verify/NskryRecordBenchmark.exe build/verify 1280 720 5 software 2
```

开发机仍为 Windows build 26200、i7-12650H、16 logical CPUs、约 15.78GiB RAM、RTX 4060 Laptop。
不是低配，未通过绑核模拟来宣称低配达标；这是合成输入+真实编码，不含 WGC/GPU 读回成本。
真实 WGC→MP4→解码仍由原有 320×180 自有窗口回归覆盖，没有新增 1080p WGC 吞吐结论。

### 最终短测结果

| 路径/轮次 | 有效时长 s | MP4 时长 s | 实际解码帧 | 进程 CPU-ms（本轮基线至停止） | 停止后 private bytes | 停止后 working bytes | 停止后 handles |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1080p hardware 1 | 5.0260209 | 5.0260000 | 99 | 1,296 | 48,979,968 | 35,258,368 | 403 |
| 1080p hardware 2 | 5.0231024 | 5.0231000 | 99 | 1,188 | 50,749,440 | 37,523,456 | 436 |
| 1080p hardware 3 | 5.0085667 | 5.0085499 | 99 | 1,094 | 53,497,856 | 39,088,128 | 469 |
| 720p software 1 | 5.0069155 | 5.0069000 | 99 | 734 | 3,239,936 | 17,850,368 | 281 |
| 720p software 2 | 5.0095573 | 5.0095500 | 99 | 531 | 3,801,088 | 17,965,056 | 281 |

Hardware 为实际 encoder 硬件标识确认，不只是请求开启标志。
CPU 数据为独立基准进程总累计，不是插件分配量，不应跨不同分辨率比较硬件/软件优劣。
三轮硬件与两轮软件，暂停安静段及停止后安静段均测得 0ms CPU 增量（1 秒、计数粒度内）。
本机平均解码吞吐约 19.7–19.8fps，不承诺每帧固定 50ms 或低配一定达到目标。
各 MP4 时长与有效时间偏差均小于 0.1ms；暂停不形成 1 秒空白。
硬件路径最高采样 private=116,686,848、working=87,388,160 bytes；
软件路径最高采样 private=87,277,568、working=60,293,120 bytes。
这是离散采样最大值，不是 GPU 显存/瞬时峰值或整个 Nskry 的内存预算。

原始数据与视频：

- `build/verify/record-benchmark-3024-324099000/metrics.csv`（最终硬件三轮）。
- `build/verify/record-benchmark-19428-324231906/metrics.csv`（最终软件两轮）。
- `build/verify/record-benchmark-26632-323785296/metrics.csv`（等待优化前硬件三轮，81 帧/轮）。
- `build/verify/record-benchmark-6784-323855531/metrics.csv`（等待优化前软件两轮，81 帧/轮）。

这些本机生成文件在忽略的验证目录内，不随源码交付；工具与命令可供其他机器重跑。

## 5. 未解决问题及下一步优先级

1. **硬件编码多轮启停后 handles 403→436→469，约每次增加 33。**
   多次独立短测均观察到相近趋势，软件两轮为 281→281。
   不能简单归因为 Windows 缓存，也不能因无持续 CPU 就宣称资源释放验收通过。
   额外显式调用 encoder IMFShutdown 的试验未改善趋势，已撤回，不保留无效 workaround。
   下一步先定位对象/驱动归属，并设计一次性录制进程隔离；即使根因在驱动，也需验证宿主不会持续积累。
2. **原生 WGC/MF 同步调用没有硬取消保证。** UI 普通录制控制不等待这些调用，但退出必须 join，
   永久驱动挂起仍可能拖延退出。进程隔离需要明确跨进程帧所有权/采集交付，不开放共享 immediate context，
   不改 API 2 布局或复用 reserved。本轮没有假装通过看门狗线程解决硬取消。
3. **设备与存储实机**：真实拔屏、DPI 切换、休眠/锁屏、安全桌面、驱动 reset、真实磁盘满/网络盘阻塞、
   权限中途撤回尚未验收。现有故障注入只覆盖其可控分支。
4. **持续/低配**：尚未进行 30min/数小时/大文件测试和低配实机测量。建议 2C/4T 低压 x64、4GB RAM、核显，
   分别测有/无硬件编码的 720p/1080p 20fps、内存峰值/停止后、UI 延迟和收尾时长；
   不把这台机器的短测当成门槛已通过。
5. **手动体验**：完整用户框选、另存为、全局键、恢复路径提示、多功能同时运行仍需人工压力测试。

不因上述限制自动安装测试插件、结束用户程序或扩大到音频/GIF/剪辑；先修可靠性再扩功能。
