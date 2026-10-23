# Translation MVP 实施报告

2026-10-23：轻量离线路线的本地开发 MVP 已打通；不是正式发行包。
未安装用户插件、未停止用户程序、未推送。模型、虚拟环境与产物均在 build/。

## 本轮实现与边界

核对原 Phase 4 翻译方向、API 2、输入扩展及 Core/插件规范后，确认当前没有跨插件 OCR 服务。
nskry-translate 因而独立按需加载，复制截图后使用 Windows OCR，不调用 OCR/Scroll 私有 DLL、模型目录或窗口。
不修改 API 2 布局或 reserved，不新增 Core 管理器。

已实现：

- 宿主通用截图/固定图/监控静态帧/编辑器选区动作；execute 返回前复制借用位图。
- 原文/译文双栏，可编辑原文，选择中英方向，主动翻译、取消、复制译文。
- 设置窗口与语言包路径保存；仅保存 HKCU ModelRoot，不保存截图文字。
- manifest 提供默认空的 capture 快捷键及 General 跳转，沿用宿主配置机制。
- 自含冻结 worker，不要求用户安装 Python；CPU INT8 OPUS-MT。
- 单任务无排队，后台不访问 UI/Registry/context/D3D；只有任务活动时有 50 ms 完成检查定时器。
- 取消、关闭、直接销毁、shutdown 回收线程、自建子进程和句柄；窗口支持最小化/调整大小。
- 普通初始化/打开设置不加载模型、联网或启动 worker；普通构建不下载模型、不自动冻结运行时。

未实现：在线 BYOK、免费接口、其他离线引擎、译文覆盖原图、语言自动检测、长段自动分句。
Windows OCR 不等于精确 OCR 插件的识别质量；可手动修订/粘贴原文。

## 引擎、来源与交付

CTranslate2 4.6.0 + SentencePiece 0.2.0，CPU INT8，intra_threads=2 / inter_threads=1，beam=2。
转换环境另含 CPU torch 2.9.0 / transformers 4.49.0，不进入冻结运行时。
开发探针显式下载公开模型，固定 revision 并记录源/转换文件 SHA-256；不把旧转换结果冒充新版来源。
语言包暂为外部目录，不提高宿主通用插件包限额。

| 方向 | 模型 | 固定 revision | 转换后 model.bin SHA-256 |
| --- | --- | --- | --- |
| 英→中 | Helsinki-NLP/opus-mt-en-zh | 408d9bc410a388e1d9aef112a2daba955b945255 | 327584c20bb83c7e89d595bcfa30b6ef3771c10816f707e892c4bbb1f808a8fb |
| 中→英 | Helsinki-NLP/opus-mt-zh-en | cf109095479db38d6df799875e34039d4938aaa6 | e4955858cae9542bef37424a9b79720e3db2f32501fe62264c0cd3eac6319777 |

原模型卡分别标注 Apache-2.0 / CC-BY-4.0。量化属于修改，应保留来源、署名及修改说明。
只生成附 DEVELOPMENT_ONLY.md 的本地试用包，不进 installer/bundled，不公开发布。
完整第三方许可文本/对应源码义务核对、Intel OpenMP 等本地运行库及可选 CUDA/cuDNN 依赖审计仍是发行门禁；
不能用项目 GPL 替代依赖许可。打包脚本不自动安装，开发包不含外部语言包。

## 安全与资源上限

- 输入最多 16000 UTF-16 单元，拒绝空白、NUL、损坏代理项；超限不静默截断。
- 每段源/目标最多 256 token；超限失败，不把截断输出当成功；保留空行和换行。
- 截图最多 4096 单边 / 4 MP，另受 Windows OCR MaxImageDimension 限制；长图需先选范围。
- 翻译期限 60 秒；这只是失败上限，不是耗时承诺。Windows OCR 阶段也有独立 60 秒上限。
- worker 执行前原子加入 kill-on-close Job，私有提交量硬上限 1 GiB；原 512 MiB 限额小于实测加载峰值，已修正。
- 只结束自建 worker，不扫描/结束用户 Python、模型服务或其他程序。
- 唯一任务目录位于 LocalAppData/Nskry/cache/translation；原文/响应均以当前用户 DPAPI 加密。
  成功/失败/取消后尝试删除明确的两个文件及空目录。崩溃或删除失败可能留下加密文件。
  不启动历史扫描；DPAPI 不是针对同用户恶意代码的沙箱。

## 实测与质量

开发机：Windows 11 build 26200，i7-12650H（10 核/16 逻辑处理器），约 15.8 GiB 物理内存。
不是低配机。以下使用公开合成短句，没有上传用户文本。

| 项目 | 实测 |
| --- | --- |
| 单方向 INT8 模型及词表配置 | 80,936,756 / 80,936,867 bytes |
| 单方向完整语言包 | 82,551,757 / 82,552,544 bytes，约 78.7 MiB |
| 冻结运行时展开 | 120,508,162 bytes，约 114.9 MiB |
| 本地开发插件 ZIP（无外部语言包） | 约 41.2 MiB；最终重打包大小见产物 |
| 每方向 3 次新进程，每进程 4 短句 | wall 1.05–1.17 秒；模型/依赖加载 570–675 ms |
| 冻结 worker 英→中短句 | wall 679.5 ms；累计 CPU 718.75 ms |
| 冻结 worker 中→英短句 | wall 723.4 ms；累计 CPU 687.5 ms |
| 两次私有提交峰值采样 | 682,614,784 / 683,560,960 bytes，约 651–652 MiB |
| 两次工作集峰值采样 | 156,811,264 / 156,876,800 bytes，约 149.6 MiB |

提交量与物理驻留工作集不能混称 RAM 占用。测试按自建子进程每 10 ms 采样，可能漏采峰值。
CPU 是进程累计时间，不是整机百分比。worker 退出后不保留模型。
未测宿主加载插件后的常驻内存增量、长段峰值、内存压力下分页或低配机耗时。

低配验收建议使用 4 核低功耗 x64、8 GiB RAM、SSD、无独立 GPU 的实机，
记录具体 CPU、可用内存、电源模式和文本规模，不按 i3/i7 品牌简单划分。
应测冷加载、短段、中等段、取消及同时运行监控；不能由本机结果承诺所有机器 1–2 秒。

质量示例：screenshot 可能译成“抓图”，screen recording 语境可能译成“录音”；
“录制已暂停，点击继续恢复录制。”的英文存在不自然表达。
无词典修补或硬编码译文；只证明基本可用，不是大规模质量评估。

## 验证与缺口

- 完整 MSVC Release 构建通过；既有 OpenCV 版本检测警告不是本轮新增失败。
- 最终完整 CTest 35/35 通过，44.59 秒（含显式开启的真实离线模型测试）。
- NskryTranslationPolicyTests：空/超长/UTF-16 边界。
- Python worker 纯测试 3/3：DPAPI 往返/损坏、输入约束、换行、段落及输出 cap，不加载模型。
- NskryTranslationPluginTests：无需模型的输入错误、缺引擎/模型、独立 Windows OCR、借用图立即释放、
  重复开始、取消、关闭、重新打开、shutdown、直接 DestroyWindow 后卸载。
- NskryTranslationOfflineTests：配置 NSKRY_TRANSLATE_TEST_MODELS 后追加真实冻结 worker 翻译/生命周期。
- translation_worker_smoke.py：双向真实翻译、加密文件协议、进程退出和资源采样，不安装插件。

本地试用产物：build/translate-local/nskry-translate-development-0.1.0-final.nskryplugin（43,214,481 bytes）。
模型根目录：build/translate-languages。包只完成 ZIP 打包及直接 DLL/worker 测试，未以用户 Registry 安装验证。
用户自行安装试用时需在插件设置中将语言包路径改为该目录的绝对路径；不只复制入口 DLL。

仍需实机：手动安装后工具栏/快捷键/Pin/PiP 全流程，中文/空格安装路径、缺 Windows OCR 语言、
旧 CPU、受限 Job、磁盘满/删除失败、长任务 60 秒超时、大量文本质量及低配性能。
现有完整回归通过不等于这些新插件场景已人工验收。

## 未来引擎兼容方向

保留用户配置在线（BYOK 或其他授权）、明确来源的免费在线、小型离线、用户可选较大离线。
当前边界为 OCR/可编辑原文 → 受限翻译任务 → 译文；私有依赖自含，不加空 manager 或新插件前置。
后续先新增版本化 provider 配置，再实现在线 adapter，要求主动上传确认、HTTPS/重定向限制、
DPAPI 凭证保护、取消/超时/大小限制和脱敏错误。不拿语言包路径存在线凭证，不暗中 online fallback。
