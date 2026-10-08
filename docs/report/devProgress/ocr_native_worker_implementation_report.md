# OCR 原生按需引擎实施报告

日期：2026-10-08。状态：已接入插件 0.6.0、构建及回归验证；未安装到用户目录，未提交或推送。
承接 `ocr_engine_evaluation_20261004.md`，采用原生一次性进程，不将 Python 加入产品。

## 1. Changed files

- `plugins/nskry-ocr/src/model/`：版本化输入/输出协议、CTC 解码、检测识别、进程资源管理、worker。
- `nskry_ocr.cpp`：精确/快速引擎切换、进度/取消、原图及结果所有权、关闭时回收结果窗口。
- `ocr_arbitration.h`：模型字符片段的连接标志，避免复制中文时人为插空格；原 Windows 仲裁未重写。
- 插件/根 CMake、manifest：0.6.0 构建、可选 Windows-only 构建、完整包及测试。
- `tools/prepare_ocr_dependencies.py`：固定 URL/SHA-256、安全解压、缓存复用；不安装 Python 包。
- `tests/ocr_{wire_ctc,process_failure,model}_tests.cpp`、fake worker、smoke workflow、说明与第三方声明。

## 2. Architecture changes

默认 Accurate OCR 直接启动 `engine/nskry-ocr-worker.exe`：共享内存 BGRA → PP-OCRv4 DB 检测 →
透视裁剪/批量 CTC 识别 → UTF-16 行与字符坐标 → 进程退出。Fast OCR 保留 Windows 引擎。
两种结果不混合、不按词典纠错。截图、坐标、编辑/复制始终基于原图，不展示识别预处理图。

worker 链接 ORT 1.29.0 CPU 与静态 OpenCV 4.10.0 core/imgproc、Clipper 6.4.2；
插件 DLL 不导入 ORT/OpenCV。模型来自固定 RapidOCR 1.4.4 wheel；方向分类模型不加载/不打包。
没有 GPU、Python、在线 OCR、LLM 或后台模型预热。保持现有插件包/加载/重启更新结构，不改 SDK。

## 3. Candidate arbitration algorithm

Windows 分支继续使用已实现的空间对齐/候选仲裁。模型分支使用检测框内的 CTC 原始输出，
不把神经网络置信度与 Windows 启发式分数相加。字符标签表只是解码表，不是词典纠错。
行框及近似字符框映射回原图，支持原结果页高亮与选择；字符边界不是像素级分割。

## 4. Fast / Enhanced / Third pass behavior

Windows 最大三轮、增强按条件触发，未变。模型一次任务执行一套检测/识别管线：
`model_pass_count=1` 不等于一次神经网络调用，识别网络按文字框长宽比排序后每批最多六个。
不先执行三轮 Windows 再启动模型，也不固定多轮模型重试。引擎切换明确由用户操作。

输入上限 16×1024×1024 像素、边长 8192；大图识别最长边限 2000。
检测栅格最短边目标 320、最长边上限 2048；识别高 48、宽上限 8192。
上述限制是资源保护，大图缩小、小字可能影响准确率，不能声称解决所有漏字。

## 5. Regression fixture results

`explorer_dark_zh.png`（1408×793）：五个指定空间区域十轮均 contains 5/5，exact 3/5。
路径识别为 `此电脑〉`，工具栏为 `三查看`：正确目标存在，但图标产生额外字符；
`类型`、`build`、`文件夹` 在对应区域完整准确。本图实际列名是“类型”，不能用“类别”验收。

`paragraph_dark_zh.png`（375×283）：十轮中文、字母和数字内容一致；195 个非空白预期字符中
六处弯引号识别为直引号，标点敏感 CER 为 6/195＝3.08%，exact=false，Han exact=true。
此前 Windows 对此固定段落也识别出“需/理/胜”，不能把本次说成在该图找回了这三个漏字。
Windows 旧评测最终 CER 5.13%；两张图不足以推断一般场景的准确率。

质量文件：`build/verify/ocr_native_{explorer,paragraph}_quality_20261008.json`。
预期文本仅被测试程序读取，生产代码不包含这些修正规则。

## 6. 10-run stability result

最终使用 NORMAL 调度优先级，每轮新建并等待 worker 退出；两张图各连续十次，准确性门禁全部通过。
下面为端到端毫秒（含图像传输、进程启动、模型加载、推理、解码和退出，不含结果窗绘制）：

| Run | Explorer | Paragraph | 管线数 / worker 退出 |
| --- | ---: | ---: | --- |
| 1 | 2131.74 | 1253.66 | 各 1 / 是 |
| 2 | 2186.86 | 1509.57 | 各 1 / 是 |
| 3 | 2112.54 | 1368.78 | 各 1 / 是 |
| 4 | 1989.96 | 1334.23 | 各 1 / 是 |
| 5 | 1967.08 | 1340.08 | 各 1 / 是 |
| 6 | 2128.80 | 1303.41 | 各 1 / 是 |
| 7 | 2524.70 | 1491.69 | 各 1 / 是 |
| 8 | 1972.71 | 1244.64 | 各 1 / 是 |
| 9 | 2044.85 | 1194.37 | 各 1 / 是 |
| 10 | 1985.37 | 1230.23 | 各 1 / 是 |

完整每轮文本/行框/字符框/置信度、load/inference/CPU/峰值内存：
`build/verify/ocr_native_model_{explorer,paragraph}_normal_10runs_20261008.json`。
同一样本十次输出稳定。早期 BELOW_NORMAL 试验有 12.76 s / 7.34 s 延迟，保留在
`ocr_native_model_{explorer,paragraph}_10runs_20261008.json`，未删除或覆盖；已撤回降低优先级。
NORMAL 十次未复现这些长延迟，但不足以证明系统负载不会影响耗时。

## 7. Performance

参考机为此前记录的 i7-12650H；未取得低配设备，未固定热状态/功耗模式，不是冷 OS 缓存测试。

| 样本 | 平均端到端 | P95（nearest rank） | 最大工作集 | 最大进程提交量 | 最大 CPU 时间 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Explorer / 4 ORT 线程 | 2.104 s | 2.525 s | 334.113 MiB | 377.180 MiB | 7.297 s |
| Paragraph / 2 ORT 线程 | 1.327 s | 1.510 s | 170.984 MiB | 210.031 MiB | 2.313 s |

CPU 时间是所有线程累计，不是经过时间或 CPU 百分比。识别结束 worker 退出，地址空间由 OS 回收；
结果窗原图/文字与系统文件缓存不应称为模型常驻，也不能宣称整个 Nskry 内存归零。
本轮进程存活检查通过，未做长时间真实监控占用曲线或低配置验收。

Job Object 限制 1 个子进程、1 GiB 进程提交量、关闭即终止；只继承输入 mapping、输出 pipe、NUL。
默认超时 20 s，取消/替代/shutdown 终止并等待自己拥有的 PID，不按名称杀其他进程。
ORT 限 1–4 线程、关闭 spinning；OpenCV 单线程、SSE2 基线与按硬件分发的 SSE4.1/AVX2。

后续验收分组（项目测试分组，不是行业定义）：低功耗 4 核/4 线程、8 GB RAM、SSD；
普通 4 核/8 线程、8–16 GB、SSD；高性能参考 6 个以上性能级核心、16 GB 以上。
混合架构/功耗差异不能只看线程数；每组记录具体 CPU、内存、功耗、后台负载及冷/热启动。
**当前未满足“低配置也保证 1–2 秒”，高性能参考机大图 P95 也超过 2 秒。**

## 8. Build / tests

- 完整 Release 构建通过；原生代码/纯逻辑/进程失败/fixture/UI 共 10 项 CTest 通过。
- 评测工具六项 Python 单元测试通过；两样本各十次生产 worker 连续验证通过。
- UI 覆盖取消、请求替代、两种引擎切换、旋转、窄窗口控件、原尺寸复制/编辑、shutdown 释放窗口。
- 失败测试覆盖缺 EXE、尺寸不匹配、畸形返回、超时、取消后的 PID 退出、预热后重复调用句柄稳定。
- 完整 `.nskryplugin` 解压至中文/空格目录加载并操作通过；仅 DLL 的缺引擎恢复到 Fast OCR 通过。
- `NSKRY_OCR_MODEL_ENGINE=OFF` 单独构建插件和 smoke 通过，不生成/覆盖当前完整模型包。
- 插件包约 20.6 MiB，含两模型/运行库/声明，不含 Python、PDB、分类模型或下载缓存。

安装器二进制及真实 Settings 导入未在本次自动化中执行，用户目录未改动。
OpenCV 的旧 CMake/MSVC 版本提示是第三方配置警告；不影响本轮成功构建。

## 9. Remaining known issues

1. 图标被误作文字、引号变体仍存在；没有通过硬编码消除。更丰富明暗/缩放/中文小字 fixture 尚待补充。
2. 1–2 s 低配目标尚未验收；单次模型内存峰值比 Windows OCR 高，但任务后退出。
3. 不带方向分类模型，倒置文字需要用户 Rotate 90°；近似字符框对倾斜/竖排不是精准分割。
4. Windows Fast 已开始的 `RecognizeAsync(...).get()` 仍不能中途终止；代次只丢弃过期结果。
5. 第三方声明已带齐代码/运行库许可及模型来源，但 1.4.4 具体权重许可归属链发布前仍需核定。
   本地开发包通过测试不代表公开发行审核通过。
6. 引擎选择只在当前会话保留；不改全局设置 schema，未来持久化需遵循既有插件规划。

## 10. Recommended next step

先由用户安装完整包测试真实漏字截图，将原图与模型 trace 作为可复现样本，
再按低功耗/普通机器测试冷启动及后台负载。不要基于少数测试词做补字规则，
不要因行级置信度高就宣称无漏字。确定薄弱点后再评估更小/更强模型或有界局部重识别，
保持一次性 worker 生命周期；公开发布前完成准确权重许可和安装器端到端核验。
