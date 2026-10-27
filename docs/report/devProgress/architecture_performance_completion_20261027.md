# 代码、架构与性能审核收口（2026-10-27）

对应 `docs/Proposal/code_architecture_performance_audit_20261027.md`。前两批的细节与历史验证见
`architecture_performance_20261027.md`；本报告记录本轮剩余项的实施及评估结论。
用户已授权本地 commit，禁止 push。没有安装用户插件、关闭用户程序、删除旧证据或访问无关的 `Paper Work/`。

## 1. 清单逐项结论

| 项目 | 收口结果 | 保留边界 |
| --- | --- | --- |
| A1 图像识别/翻译关闭响应 | 前批已修复直接销毁 UI 的同步等待，延迟提供者与真实 OCR 联动回归保留 | v1 release 和 DLL shutdown 仍必须 join，不强杀原生线程 |
| A2 更新/录屏生命周期 | 更新已是后台任务，新增延迟清理响应测试；录屏直接销毁 UI 后的采集/预览/导出孤儿任务已修复 | 程序退出仍等待安全清理；第三方 native DLL 非沙箱 |
| B1 共享构建归属 | 共享布局/进程静态目标及统一资产清单已落实，各包自含资产，不递归扫描另一个插件包 | 源码物理位置仍在 OCR 目录，worker 仍由同一构建 target 生产；没有引入服务前置或依赖安装器 |
| B2 跨业务验证类型 | 前批已抽通用 UTF-16 验证，识别不再构造翻译请求 | 旧 SDK 验证入口、版本/长度/错误行为保留 |
| B3 大入口凝聚性 | 本轮将翻译的离线进程/DPAPI/临时文件清理移入 offline_process.*，入口只负责 UI 与任务接续 | 其他大窗口并非仅因行数构成缺陷；不为改名/减行数拆碎状态或重写窗口 |
| B4 产品 ID 与能力边界 | 核对仍符合规范：客户端可明确选择 OCR；Core 调度重查启用、兼容、能力；占位不提升官方身份 | 保留既有产品快捷键入口；未实现/冒充通用依赖解析器 |
| C1 识别/翻译冷启动与内存 | 实测分段；移除截图翻译 BGRA→临时 DIB→读回往返，直接复制到有界 IPC 输入 | 不改模型、beam、INT8、质量规则，不用常驻模型换冷启动；MKL 禁用方案经测试不采用 |
| C2 动态录屏 | 新增按 CPU/OS 能力选择的精确 AVX2 转换；消除帧调度迟到累积；实际 30/60fps、暂停/收尾/解码验证 | GPU NV12/跨进程纹理/MF sample 池未实现，评估不纳入本次：需要额外所有权/驱动验证，现有证据不支持为省复制直接改协议或盲复用 sample |
| C3 诊断构建 | 前批已将手动图片审计设显式目标，仍能单独构建 | 保留工具、冻结标准、失败 trace |
| C4 重复编译与声明 | JSON、metadata、runtime 分别按职责成为内部静态库；各测试/插件只链接所需层 | 官方插件仅复用 JSON，不链接宿主 Registry/Runtime；不是新增管理器或运行服务 |

本次清单中的可直接实施改动已完成；高成本候选作出明确不采用决定，不标成已实现。
未来若要做 GPU 编码全链路或替换翻译 runtime，应另用跨设备收益和质量证据立项。

## 2. 实际依赖与所有权

### 源码职责

| 层 | 拥有的内容 | 不应承担的内容 |
| --- | --- | --- |
| Core UI / settings | 框选、通用编辑、Pin/PiP、动作布局、设置生命周期 | OCR/翻译模型、录屏业务纠错 |
| Core metadata/runtime | manifest/registry、兼容判断、延迟加载、SDK 调度、主线程边界 | 识别/编码算法或另一个后台模型管理器 |
| OCR / Scroll | 各自算法、结果/采集 UI、任务清理 | 修改宿主窗口控件、提供通用基础编辑前置 |
| Record | 录制控制、会话时间线、编码/导出/预览 worker | 借用共享 immediate D3D context 在后台编码 |
| Translate | 翻译 UI、输入接续、在线适配、独立离线进程模块 | 借其他包私有目录或让 OCR 成为隐式必装项 |

### 构建依赖

| 消费者 | 明确链接/生成依赖 | 交付效果 |
| --- | --- | --- |
| Core 与宿主集成测试 | nskry_plugin_runtime → nskry_plugin_metadata → nskry_json | 同职责源码编译一次，仍是静态代码 |
| JSON/manifest/UI 等测试 | 所需 JSON 或 metadata 层 | 不为 JSON 测试拉入生命周期实现 |
| Translation DLL | 纯 JSON、私有 recognition、独立 offline_process | 不链接宿主 runtime/registry |
| OCR / Translation native recognition | 共用 layout/process/wire 静态实现和 runtime asset 声明 | 两个独立包自含各自模型/许可，安装不依赖对方 |
| Record | 私有 codec/engine/pixels 与 worker target | AVX2 仅单独翻译单元启用，普通代码保留基线指令集 |

原 CMake 声明中 JSON/manifest/registry/runtime/image-dispatch/SemVer 源引用分别为
16/9/7/6/6/9 次；当前各为 1。包含显式诊断目标，不能把这 47 个声明差直接当成默认构建减少 47 次。
Ninja 图核对 runtime/json 各只有一条编译规则。没有测量冷构建墙钟加速比例，不宣称安装体积因此大幅缩小。

新增配置阶段的插件链接边界检查：四个官方插件的传递链接不得包含宿主 runtime/metadata，JSON 可用。
覆盖合法纯库、传递违规、alias、条件链接四种手动 CMake 检查（`build/boundary-check-c67086bc6f/results.json`）。
这是本仓库防止误链接的门禁，不是对第三方 DLL 的安全隔离。

### 运行依赖

| 路径 | 所有权 / 生命周期 |
| --- | --- |
| Core → 插件 | metadata 不加载 DLL；用户执行才 init/execute；退出先停止任务再卸载 |
| OCR → 翻译 | 已有文字经公开版本化交接复制，不再识别 |
| 翻译 → 可选 OCR | 用户主动勾选；宿主复制借用图、限定一个任务；主线程领取结果 |
| 默认截图翻译 | UI 已拥有的紧密 BGRA → 私有同步 RunModelPixels → IPC 自有副本 → 一次 native worker |
| 离线翻译 | 一个 DPAPI 临时请求与有界子进程；结束/取消/失败后清理；不驻留模型 |
| 录屏 | 会话线程持有私有采集/编码器；控制 UI 消失后临时收尾窗口等终态，不再继续无 UI 采集 |

SDK API 2 结构及 reserved 没有变化；新增 raw-pixel 函数是包内静态实现，不是跨 DLL 的 STL 接口。
设置、未知配置、旧 configure v1/v2、standalone 预留、staging/rollback、模型来源与许可均保留。

## 3. 性能改动与实测

### 截图翻译输入

原路径已有 BGRA vector，却重建一张 DIB、memcpy，再 GetDIBits 到 IPC mapping。
当前直接将借用 span 复制到同一有界 mapping，输入始终由任务拥有，worker 退出后释放。
移除一个 width×height×4 的临时 DIB 与往返；4MP 上限对应约 16MiB 像素存储。
这是被删除的分配/复制规模，不是已经测得总提交量必降固定 16MiB。

真实模型测试分别比较 bitmap/raw 的识别文字、行框、glyph 框与偏移；覆盖两张既有 fixture。
另补 raw 短缓冲、预取消、超时和 worker 回收。没有使用测试期望文本修正识别。

### 录屏像素转换

新 AVX2 实现沿用完全相同的 BT.709 整数系数、floor 平均和饱和规则。
只在 CPU 支持 AVX2、OS 支持 YMM 状态且宽度适合时调用；其他情况保留 SSE2/原基线。
AVX2 只给 pixel_avx2.cpp 编译，不把整个插件编译成要求 AVX2；可以配置 `NSKRY_RECORD_AVX2=OFF`。
没有新增像素缓冲/队列。随机像素、2..32 偶数宽度、非对齐指针、前后哨兵均与独立标量公式精确一致。

1080p，100 次转换，5 轮交替执行 SSE2/选择路径：

| 轮 | SSE2 ms | AVX2 选择路径 ms | 字节一致 |
| --- | ---: | ---: | --- |
| 1 | 127.601 | 73.3027 | 是 |
| 2 | 131.727 | 77.2139 | 是 |
| 3 | 132.865 | 71.6547 | 是 |
| 4 | 126.295 | 77.5240 | 是 |
| 5 | 129.219 | 82.4774 | 是 |

中位数 129.219→77.2139ms，转换阶段约减少 40.2%；不等于整机或完整录屏 CPU 下降 40.2%。
原始输出 `build/pixel-conversion-20261027.txt`。本机支持 AVX2；另一构建关闭 AVX2 验证基线路径，不能代替旧 CPU 实机。

### 帧调度与资源

旧 next=本次采样时间+间隔会积累每次读帧/定时器小延迟。
现在按录制有效时间的采样网格前进，跳过错过时隙，至少留半个间隔，不追补积压、不忙等。
时间戳仍来自实际有效时间，暂停时长不写入视频；补迟到不积累、大停顿不追赶、溢出饱和纯逻辑测试。

合成动态画面 + 真实 MF 硬件编码，1080p、每轮约 2 秒有效录制、暂停 1 秒、5 轮：

| 配置 | 原调度帧数 | 新调度帧数 | 解码与时长 |
| --- | --- | --- | --- |
| 30fps / V2 | 58–59 | 61 | 所有文件全量解码，帧数匹配会话计数、总时长匹配 |
| 60fps / V2 | 110–113 | 121–122 | 同上 |

首尾部分帧使帧数/时长之比可略高于名义帧率，不能把边界差当作无限补帧或固定 CFR 证明。
两批为顺序测量，没有随机交替整个编码流程；观察支持消除累积漂移，不能保证任何硬件都恒定达到目标帧率。
没有测 WGC 采集速度，不能把合成源结果当成桌面/游戏录制数据。

原/新 30fps CSV：`record-benchmark-43272-350968140` / `record-benchmark-33332-351360312`；
原/新 60fps CSV：`record-benchmark-15240-350752031` / `record-benchmark-17112-351214250`，均在 `build/verify-translation/`。
最终两组会话均在确认编码进程退出后发布终态；停止后 5/30 秒宿主句柄稳定 176，私有提交约 1.58→1.49MiB，无逐轮增长。
settle 行传入空 Snapshot，子进程 PID/内存字段为占位 0，不能当成额外进程枚举测量；退出确认来自会话 Close/终态协议。
这是基准进程，不是完整 Nskry 的空闲内存。30 秒期间测试泵仍工作，不能据 0ms CPU 计数宣称零开销。

测量修正也保留证据：最初 60fps 基准误用至多 30fps 的 V1，明确失败后改用真实 V2 契约；
最初在解码后取 5/30 秒样本会混入 MF decoder 缓存，已将取样放在解码前。失败数据未删除。

### 翻译冷启动：不采用无效优化

显式 `tools/profile_runtime.py`，每方向 5 个新进程，仅固定公开短句，开发解释器 CTranslate2 4.6.0 / SentencePiece 0.2.0：

| 中位数 ms | 英→中 | 中→英 |
| --- | ---: | ---: |
| 导入 | 332.33 | 319.68 |
| 两个 tokenizer | 73.48 | 73.27 |
| 模型 | 91.34 | 90.70 |
| 推理 | 86.78 | 63.68 |
| 整进程 | 744.43 | 708.43 |

私有提交从入口约 10MiB 到导入后约 513MiB、模型后约 617–619MiB、完成约 647–649MiB。
每次进程实际退出，无留驻模型。这是开发解释器分段，不是冻结包完整 UI 的时延或绝对峰值保证。
结果 `build/translation-runtime-profile-20261027.json`。

按 [4.6.0 官方环境变量说明](https://raw.githubusercontent.com/OpenNMT/CTranslate2/v4.6.0/docs/environment_variables.md)
试验导入前禁用 MKL：峰值仅降低约 18MiB，整进程中位数却为 792.32/749.31ms，推理也更慢；固定文本输出 hash 一致。
因此不改变用户默认后端、不删运行库、不绕过包初始化，也不修改模型精度/beam。
对照 `build/translation-runtime-profile-nomkl-20261027.json`。这是 5 次顺序对照，不称统计显著性或 P95。
若将来要大幅缩小 native import 的成本，需要独立评估/构建 runtime 与许可/多 CPU 回归，本轮没有假装解决其全部内存。

## 4. 测试、质量与交付

基准机：i7-12650H（10 核/16 线程）、系统报告物理内存总量约 15.78GiB（标称 16GB）、RTX 4060 Laptop GPU；MF 运行时确认硬件编码，但未据 GPU 名称猜测具体编码器。
没有低资源组 4 核/8GiB/核显、多驱动/断屏/热降频实机；Core 的低空闲策略保留，不能保证所有机器 1–2 秒完成 OCR。

最终 Release 构建成功，`git diff --check` 通过。模型、原人工标准、错误样例没有降低或重写。
版本为 OCR 0.6.6、Record 0.4.1、Translation 0.3.3；安装兼容边界沿用现有公开扩展，旧插件保持兼容。

| 验证 | 结果 |
| --- | --- |
| 最终 native-model / AVX2 配置完整 CTest | **46/46，75.16 秒**（此前同内容轮次与另一构建并行时 131.91 秒，不作性能比较） |
| model OFF / AVX2 OFF 全部配置测试 | **41/41，55.70 秒**；构建所有测试目标及依赖，避免 OFF 的 ALL 打包覆盖源树 native bundle |
| SSE2 回退显式执行 | `NskryRecordWireTests` 输出 avx2=0，独立整数参考与非对齐哨兵通过 |
| bitmap/raw 模型输入 | 两张 fixture 文本/glyph 坐标一致，既有区域/内容断言通过，进程确认退出 |
| 生命周期 | 普通取消、直接销毁、预览拖动、导出、退出、延迟清理、重复开始等仍在完整回归中 |
| 构建资产 | 前批 4 项 staging 生成/增量/重建通过，两个包 15 个运行文件 hash 一致 |
| 链接边界 | 合法纯库通过，传递宿主库、alias、条件宿主库均拒绝，四项手动配置检查通过 |

OFF 首轮 40/41：原因是按测试 target 构建时没有隐式生成 `NskryOcrFakeWorker`，不是产品原始像素失败。
已为 `NskryOcrProcessFailureTests` 添加明确依赖，再跑全部 41 项通过，未删除断言。

最终实际离线包解压到 `build/translation-optimized-0.3.3-package/`；CRC、manifest 版本、安全路径、DLL 与构建产物一致性通过。
使用既有两张公开图、原人工标准各 3 次截图 + 各 1 次独立正确文本控制，共 **8/8 工作流完成**。
英文三轮 CER=0；中文三轮 CER=1/36，原来的个别难字错误仍存在，严格 exact-OCR 质量条件仍不满足。
不将工作流完成当作质量合格，也没有把优化宣称为识别准确率提升。
所有观察到的 worker 均退出，同时活跃子进程峰值为 1，没有在线上传。

最终包端到端（ms）：英文原文出现 1343/1344/1297、总 2906/2797/2812；中文原文出现 937/1172/1015、总 2187/2797/2312。
这是本次包的实测，不与历史不同负载/缓存条件的更慢轮次比较加速比例；也没有达到所有输入总耗时 1–2 秒的保证。
明细 `build/translation-final-package-audit-20261027.json`、`translation-final-package-evaluation-20261027.json`。

### 未安装的本地产物

| 路径 | 字节 | SHA-256 |
| --- | ---: | --- |
| `build/translate-local/nskry-translate-offline-development-0.3.3-optimized.nskryplugin` | 64894715 | `7180126f85a396c2cb43f9901e1b996dae87b84328e6ebcb433987036672d31d` |
| `build/translate-local/nskry-translate-online-development-0.3.3-optimized.nskryplugin` | 21740723 | `bb726372d1a68ca287822df8f1aa729a6c7afb2b274310505ff2b573dc54f6b8` |
| `installer/bundled/nskry-ocr.nskryplugin`（0.6.6/native） | 21674737 | `1671db6e51db30d4d7a3f8fac06d34c1e3de966fda7019cfc31cf0d92c1e95b4` |
| `installer/bundled/nskry-record.nskryplugin`（0.4.1） | 120806 | `ac93abe34b10091a40f0e1f6e4f759052f0135748731830812b16045e92972a6` |

被审计的 Translation DLL SHA-256 为 `1644ff006f6f61b2c46619645780dd269e8d073175b9c208320124f23cf03ae9`。
Core 位于 `build/verify-translation/Nskry.exe`。语言包继续使用外部 `build/translate-languages`。
Translation 仍为本地开发包，没有加入公开 installer；正式 runtime/模型授权与对应源码发行门禁未取消。
旧 0.3.2 包保留，它们不包含本轮代码。build 证据为本地临时材料；源码中的诊断入口可重新生成，不能承诺这些临时路径永久存在。

## 5. 代码和测试取舍

删除的是重复编译声明、跨业务验证耦合及多余像素往返，不删除已承诺功能。
独立标量参考、SSE2 回退、原生故障 worker、旧扩展测试、质量失败 trace 各自验证不同风险，不能当成重复代码清掉。
大文件按变化原因拆分，离线进程具有明确资源所有权所以拆出；Pin/PiP/编辑器没有因行数就引入新管理器或插件前置。
没有发现可直接删除且满足“无调用、非动态入口、非兼容/规划”的剩余生产函数，不为了凑删减数量移除未来契约。
