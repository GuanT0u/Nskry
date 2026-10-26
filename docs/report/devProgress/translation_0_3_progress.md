# Translation 0.3.0：可选在线引擎与离线长段落（2026-10-25）

状态：本地开发试用版，已实施、构建并验证；不是“正式发行门禁全部完成”。
未自动安装、未停止用户程序、未 push。沿用 API 2、文字翻译交接 v1、原有语言包。

后续真实图片验收发现：流程完成但原文/译文质量不合格；下面的功能测试通过不能当作质量通过。
详见 [公开图片自动化验收](translation_public_image_audit_20261025.md)，保留原始包及失败证据，未以修正基准掩盖问题。

下一轮已实施 [0.3.1 识别/翻译优化及同标准复测](translation_0_3_1_quality_progress.md)：英文 OCR 恢复准确，中文明显改善但数字分组与译文质量仍未整体放行。

再下一轮：[0.3.2 空间间距恢复与可选 OCR 联动](translation_0_3_2_recognition_progress.md)。门牌分组已恢复；仍有字形/专名错误，严格质量门禁未全部通过。保留轻量默认路径，不靠多轮、精度或硬编码修词。

## 1. 先核对的实际缺口

- 0.2.1 仅离线；已具备框选自动识别/翻译、脚本判断中英方向和 OCR 已有文字交接。
- worker 每个原文行超过 256 token 就拒绝，OCR 段落合并后更容易触发。
- worker 有失败回复，但宿主先看到非零退出码就抛错，丢弃回复，用户只能看到笼统报错。
- 本轮不重写 OCR、采集或 SDK，不引入 OCR/Scroll 前置、模型服务或额外宿主管理器。

## 2. 实施顺序与完成项

先补离线分段、私有回复协议与测试并构建成功，再增加在线适配/配置/UI，最后加强网络退出栅栏并完整回归。

### 离线

- 原始段落按句末/空白优先切片，每片重新分词、校验，绝不依赖引擎静默截断。
  只有无合适边界的超长词/无标点文本才回退为字符边界；保留空行及 CRLF/LF/CR。
- 每次推理仍最多 256 token；输出触顶时重试更小原文片段，不发布已截断片段。
  整个任务最多 128 次推理，仍为 16000 UTF-16、2 个推理线程、60 秒、Job 私有提交硬上限 1 GiB。
- 新 worker 回复 protocol=1、parts、白名单 error_code；缺语言包/运行时、加载失败、输入/输出/工作量上限、内存等有不同提示。
  退出码 1 会读取结构化失败；崩溃、损坏、不完整回复不发布部分文字，不显示原始异常/路径/原文。
- 旧 worker 的成功回复仍可读；旧 worker 自身的段落限制不会因只换 DLL 就消失，需更新完整包。

### 在线

- OpenAI 兼容 Chat Completions：用户填完整 HTTPS endpoint、模型 ID、可选 Key。
  默认 max_completion_tokens=4096，可显式切换 legacy max_tokens。不选默认付费模型，不读取/获取用户账户凭证。
- LibreTranslate：用户填完整 HTTPS /translate 和可选 Key；没有擅自把其商业托管站点当免费服务。
- 可选免费 MyMemory 官方 /get：无需账号/Key/email；目前匿名额度 5000 字符/日。
  项目额外限制单任务 2000 UTF-8 bytes、最多 16 个顺序请求、每段 480 bytes、整体 60 秒。
  保留段落，不同段落不混在一个请求。不调用 /set、贡献、注册或取 Key 接口，不规避配额。
- 第一次打开默认离线；显式选择引擎可记住。**每次在线翻译仍单独确认上传**，OCR 交接不构成授权。
  提示目标地址、文字长度、可能收费/保存/共享，以及取消不能撤回已发内容。
  MyMemory 额外提示长期存储/第三方处理和 GET 查询参数风险，敏感文字应留在离线。
- 同一个任务只走选定引擎，无应用层自动重试或联网 fallback。服务失败、配额、截断、过滤/拒绝、工具调用不伪装成功。

协议来源由 OpenAI Docs 核对官方请求/响应格式；其他服务同样仅依据官方资料：
[Chat Completions](https://developers.openai.com/api/reference/resources/chat/subresources/completions/methods/create)、
[LibreTranslate](https://docs.libretranslate.com/guides/api_usage/)、
[MyMemory API](https://mymemory.translated.net/doc/spec.php)、
[额度](https://mymemory.translated.net/doc/usagelimits.php)、
[隐私与条款](https://mymemory.translated.net/terms-and-conditions)。
服务条款/额度可能变化；软件开源许可不等于第三方服务转售许可，未作法律授权保证。

### 配置与 UI

- 原生、按需设置窗口，无模型预热、端点探测、下载或保存即联网。
- 两类自配引擎各用一个 schema=1 的当前用户 DPAPI REG_BINARY 值；未知字段保留，解析失败/更新 schema 不静默覆盖。
  可明确 Forget；原 ModelRoot 不变，PreferredProviderV1 只记录用户显式引擎选择。
- Key 密码显示；UI 线程读取配置、复制快照给任务。DPAPI 不是同用户恶意代码的沙箱，不保证抹除所有分配器/OS 副本。
- 增加 Save text（UTF-8），Copy/Save 仅完成后有结果可用。手动修改原文立即清掉旧译文，避免误复制过期结果。
- 在线上传确认期间防重入；拒绝返回 SDK CANCELED，没有把拒绝当成功或开始网络。
  最小化后再调用会恢复窗口；设置窗口随所属结果窗口销毁，不留孤立 UI。

## 3. 线程、安全与资源边界

在线路径是原生 WinHTTP，不启 Python/离线模型。一个任务一个可取消 worker，只有任务期间的 UI 完成检查；
无启动请求、空闲定时器、固定连接、推理池、常驻自建线程或后台 OCR 上传。

HTTPS 必须，保留证书验证；禁 URL 用户名/密码、配置地址查询参数、重定向、cookie、自动 Windows 认证和跨会话连接池。
响应上限 2 MiB；不向用户展示可能回显原文/Key 的服务错误 body，不记录原文/凭证。

请求 HANDLE_CLOSING 栅栏保护 request 数据；额外使用 session 的 WINHTTP_OPTION_UNLOAD_NOTIFY_EVENT，
等到**最后一个回调真正返回**再退出任务，避免“回调里 SetEvent 后 DLL 已卸载但回调尾部还执行”的竞争。
状态/读缓冲的栈生命周期覆盖整个 session；主线程不直接操作 worker 的请求。
依据 [Microsoft WinHTTP 选项](https://learn.microsoft.com/en-us/windows/win32/winhttp/option-flags)
与 [关闭语义](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpclosehandle)。

布局拆分仅限本插件：online_translation（协议/编排）、translation_http（传输）、online_config（持久化）、
online_settings（配置 UI）、worker_reply（离线协议）。未添加宿主管理器、SDK reserved 用法或一次性采集重构。

## 4. 构建与测试

MSVC Release、build/verify-translation 完整构建成功。正常构建不下载模型/引擎；本轮显式重新冻结并暂存 worker。
最终完整 CTest **42/42 通过，57.19 秒**（此前完整一轮 42/42、59.72 秒也通过）。
包含截图/监控/固定图、OCR/长图相关 UI、录屏、更新、输入扩展和插件生命周期的既有回归。

新增/扩充：

- Python 纯逻辑 6 项：无模型分段、长度/Unicode、空行、变动 token 计数、输出触顶重分、总工作量上限、DPAPI。
  可用 Python 时加入 CTest；模型库使用替身，不下载/导入真实推理依赖。
- 私有 worker 回复：新/旧协议、退出码一致性、损坏/未知 schema、NUL/无效 Unicode、错误信息不泄露子进程文本。
- 在线纯逻辑：OpenAI/legacy 请求、Libre 可选 Key、免费 UTF-8 编码与边界、段落保留、配额/截断/拒绝/工具结果。
- 独立进程内重定向 HKCU：合成 Key 加密保存/读取、未知字段、新 schema/损坏配置拒绝、Forget；不读取/修改用户真实 Key。
- 自建 loopback TLS 停滞端点：真实异步超时/取消与回调回收；不安装证书，不绕过 TLS，不用外网。
- 窗口：每次确认、拒绝后无任务、确认期间重入 BUSY、拥有窗口关闭、无离线引擎包也能打开/配置/安全卸载、引擎显式偏好。
- 真实离线 2275 字符段落成功分 2 次推理；截图自动翻译、已有 OCR 文字交接、取消/重复开始/关闭/卸载均保持。

首轮窗口回归失败是新增“拒绝上传”场景留下非空原文，后续“空输入”场景错误启动任务。
已在场景边界清空原文后重测；没有放宽空输入保护或删掉用例。
一次手动测试使用相对 DLL 路径，未满足 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 的绝对路径要求；
改绝对路径后通过，不是插件安装/识别失败。

标准 CTest **没有外网调用**。显式 live-free 测试只发固定公开合成短句：
MyMemory 中→英、英→中成功；实际窗口从确认到显示结果也通过（约 1094 ms，4 个 UTF-16 输出单位）。
未上传用户截图、剪贴板、账号 Key 或历史内容。

## 5. 实测性能与空间（不是低配保证）

开发机沿用 Windows 11 / i7-12650H（10 核 16 逻辑处理器）/约 16 GiB RAM。
以下是单次公开合成 fixture 的观测，不是准确率/延迟 SLA，也不是低配硬件结论。

| 路径 | 墙钟 | CPU 累计 | 私有提交观测峰值 | 物理工作集观测峰值 |
| --- | --- | --- | --- | --- |
| 免费在线 英→中 | 938 ms | 15.625 ms | 2,768,896 B | 13,803,520 B |
| 免费在线 中→英 | 937 ms | 低于本次计时粒度 | 2,834,432 B | 14,024,704 B |
| 离线 英→中，37 UTF-16 | 922 ms | 953 ms | 682,745,856 B | 156,753,920 B |
| 离线 中→英，10 UTF-16 | 911 ms | 859 ms | 683,245,568 B | 156,377,088 B |
| 离线长段落，2275 UTF-16，2 parts | 5572 ms | 7172 ms | 746,409,984 B | 213,037,056 B |

在线是**独立原生测试进程**的当前采样峰值，包含测试自身，不是宿主新增量，也不含服务器/OS 服务开销。
采样 5 ms，CPU 计时存在粒度，显示 0 不表示完全零成本；离线是新启动 worker 的 10 ms 采样。
CPU 累计可大于墙钟，因有两个推理线程。短句不代表长文也能在两秒内完成；长文质量/上下文仍受实验模型限制。
任务后子进程退出，临时加密请求/回复按既有 RAII 删除；异常退出/文件锁仍可能留加密文件，未新增启动扫描。

低配（如低功耗 4 核/8 GiB/SSD、无独显）和内存压力尚未实测，不声称低配流畅或所有机器 1–2 秒。

## 6. 产物（仅生成，未安装）

两个是**同一插件 ID/version 的可选构建形态**，不要同时当两个插件安装：

- 仅在线：build/translate-local/nskry-translate-online-development-0.3.0-final.nskryplugin
  - 177,491 B；8 项，展开 406,828 B；不含 engine/Python/模型。
  - SHA-256：96346e846d836c767d7e3104bc0e1e112de78dce9cfe47cccc7ef7ee7081a97e。
- 离线增强（也有在线）：build/translate-local/nskry-translate-offline-development-0.3.0-final.nskryplugin
  - 43,330,001 B；144 项，展开 121,227,716 B；保留完整冻结 engine。
  - SHA-256：0cd5cebfdf602d12e9f6bf798d72b6937da0be385c75bbec469033a3f851aba7。
- 外部语言包仍为 build/translate-languages；无需重新转换。

核对 ZIP 根目录、manifest 0.3.0、GPL/例外和在线隐私指南均在包内，均低于现有宿主包限额。
实际解包仅在线包运行独立窗口回归通过；没有安装进 Registry，也没有替换运行中的用户 DLL。
不把离线增强包放入 installer/bundled；完整原生运行时依赖/第三方许可审核仍是正式发行门禁。

## 7. 未声称完成的内容

- 无真实用户 BYOK Key、可用 LibreTranslate 服务器，故没有这两者真实账户成功/扣费验收；协议、配置和传输生命周期已测试。
- 第三方可能有不同 token 参数、system role/认证/语言规则；未实现 Azure 专项、Responses/SSE、多模态/工具或全部兼容变体。
- 只有中英脚本判断，不是通用语言识别；其他拉丁语言可能需用户调整原文/方向。没有词典硬修、假数据补译或准确率保证。
- Save text 的交互式文件选择、不同磁盘/磁盘满，以及高 DPI/多屏、企业代理、休眠/断网压力仍需实机体验验收。
- 更大离线引擎、跨请求上下文、更多语言、完整公开发行许可审计属于后续范围；没有用空接口/额外前置冒充完成。

使用说明与官方来源见 plugins/nskry-translate/ONLINE_ENGINES.md。
