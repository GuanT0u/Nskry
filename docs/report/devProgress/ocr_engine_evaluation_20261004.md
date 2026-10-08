# OCR 引擎与瞬时资源取舍实测

> 2026-10-08：本报告保留原型阶段结果。原生按需引擎现已接入正式插件代码，
> 后续构建、准确性、资源和已知限制见 [实施报告](ocr_native_worker_implementation_report.md)。

日期：2026-10-04。状态：**完成对照评测工具和原型验证，未将新引擎接入正式插件，未安装、提交或推送。**

## 1. Changed files

- `tests/ocr_native_probe.cpp`：独立的 Windows OCR 裁剪、留白、缩放、灰度/反色实验；每个组合是独立识别，不能称为生产中的一轮。
- 根 `CMakeLists.txt`：添加手动构建的 `NskryOcrNativeProbe`，不加入默认构建/CTest。
- `tests/ocr/benchmark_rapidocr.py`：使用本机已安装的 RapidOCR/ONNX Runtime，单次任务单独进程，记录原始文字/位置/置信分数、启动/加载/推理/端到端时间、CPU 时间、峰值工作集/提交量、模型版本/大小/哈希。没有上传截图、下载模型或修改全局 Python 包。
- `tests/ocr/evaluate_results.py`、`test_evaluation.py`：空间区域验收、字符编辑距离、原生 trace 读取和 6 项纯逻辑测试。
- 新增用户此前提供的 `paragraph_dark_zh.png` 及人工核对正文；新增 Explorer 局部区域预期，修正列名“类别”为“类型”。正文预期不含列表圆点，标点和大小写参与比较，空白不参与；`han_exact` 仅是额外汉字序列指标，不能替代完整准确率。
- `tests/ocr/.gitignore`、`tests/ocr/README.md` 和进度报告。

## 2. Architecture changes

正式插件无架构变化，仍是 Windows OCR；本次不把 Python 运行时加入产品依赖。原型为本地 PP-OCRv4 检测+识别模型，经 `rapidocr-onnxruntime 1.4.4` / `onnxruntime 1.29.0` 执行，CPU-only。正文均正向，实验关闭方向分类的推理；此包装库初始化仍创建分类 session，这也计入加载时间和内存。

依据实测，建议下一阶段采用**独立、按需启动的一次性原生 worker**：插件保留 UI 和原图，worker 加载模型、完成任务、返回文字/坐标后退出。线程限制、任务内内存复用与退出回收分开处理。独立进程退出可回收其地址空间，但 OS 文件缓存不应伪装成泄漏，也不能宣称整个 Nskry 的内存降为零。

准确模式应直接调用经过验证的模型，而不是每次先跑三轮 Windows OCR 再补一个模型：识别成另一有效汉字时，原有评分未必触发回退。Windows OCR 可保留为轻量选项/模型包缺失时的明确回退，不能静默降低准确模式质量。

## 3. Candidate arbitration algorithm

本次不改仲裁、不拼两种引擎输出、不硬编码“类型/查看”等文字。神经模型的分数也不能直接与原生启发式评分当成同一种置信度。

补充空间验收：同样的“此电脑”出现在左侧导航不能抵消上方路径里的漏字。分别记录 contains 和 exact：`三查看` 是包含正确文字但多了一个字，不是完全正确。

## 4. Fast / Enhanced / Third pass behavior

生产最大 3 pass、条件触发、原图展示和插件生命周期全部保持原样。

Windows 独立裁剪实验针对上方路径、工具栏、列名及合并区域，各测试 1/2/3/4 倍、0/12 px 留白、原色/灰度/反色：路径在部分组合恢复“此电脑”，加留白后也可能恶化；“查看”未被这些组合找回，“类型”局部仍有“举型”错误。**这些有限实验不证明所有原生优化都无效，但不足以支持“简单调整就能解决三类问题”。**

原生 probe 用 GDI+ bicubic，与生产 GDI HALFTONE 不同，不能当成生产路径性能或精确等价复现。原始 trace 在 `build/verify/ocr_native_crop_{path,view,type,toolbar,group}.trace`。

## 5. Regression fixture results

### explorer_dark_zh（1408×793）

原生列来自 2026-10-01 的留存 10 次 raw pass trace，按本次校正后的坐标重新评估；不是本次重新采集的 10 次原生测量。

| 指定位置 | 原生 A / B / C | 离线模型（本次 10 次均一致） |
| --- | --- | --- |
| 路径“此电脑” | `>I比申，月囟` / `>I比申，脑` / `>I比申，脑` | `此电脑〉`，字正确但带箭头 |
| 工具栏“查看” | 三轮均缺失 | `三查看`，图标被误认成“三” |
| 列名“类型” | 三轮均缺失 | `类型`，完全匹配 |
| 路径“build” | 三轮正确 | `build`，完全匹配 |
| 第一行“文件夹” | 正确 / `文亻牛夹` / 正确 | `文件夹`，完全匹配 |

模型为 5/5 局部 contains、3/5 局部 exact，**不是 100% 准确**，更不是全图所有文本都经过人工验收。其他位置仍有图标噪声。

### paragraph_dark_zh（375×283）

本次 Windows 生产管线和模型各跑 10 次：

- Windows 最终结果：195 个非空白参考字符，编辑距离 10，CER 5.13%。
- 模型结果：编辑距离 6，CER 3.08%，六处弯引号被识别为直引号；当前参考正文的汉字/拉丁字母内容完整。
- 两者本次的汉字序列都与参考一致，包含“需、理、胜”。**这张固定文件没有复现用户曾报告的随机漏这几个字，不能声称模型在这个对照里修复了原生漏字。**
- UI 高亮框映射与实际返回文本需要分别验收，本轮没有以“高亮看起来完整”替代文字比较。

## 6. 10-run stability result

以下均为每轮新建进程，包含启动、导入、模型加载、图片读取、检测、识别、结果序列化和退出。OS 文件缓存可能已热，不代表开机后首次运行；不含最终产品 UI 绘制/进程间传图。未同时启动其他本任务的 OCR 测试。

| Run | Explorer 端到端 ms | 段落端到端 ms |
| ---: | ---: | ---: |
| 1 | 1990.89 | 1453.88 |
| 2 | 1987.09 | 1409.12 |
| 3 | 1999.88 | 1420.98 |
| 4 | 2058.20 | 1393.52 |
| 5 | 2047.46 | 1413.17 |
| 6 | 2060.25 | 1393.44 |
| 7 | 2104.39 | 1432.36 |
| 8 | 2076.72 | 1397.59 |
| 9 | 2097.25 | 1427.24 |
| 10 | 2105.97 | 1383.04 |

每次 Explorer 94 行，段落 10 行。局部验收和段落完整文本在各自 10 次中稳定；每个 worker 正常退出。

完整模型输出与逐轮 timing：

- [Explorer](../../../build/verify/ocr_rapidocr_explorer_arena_10runs_20261004.json)
- [段落](../../../build/verify/ocr_rapidocr_paragraph_arena_10runs_20261004.json)
- [Explorer 空间验收](../../../build/verify/ocr_rapidocr_explorer_arena_quality_20261004.json)
- [段落验收](../../../build/verify/ocr_rapidocr_paragraph_arena_quality_20261004.json)
- [原生段落 A/B/C + 仲裁 trace](../../../build/verify/ocr_native_paragraph_10runs_20261004.trace)
- [原生段落验收](../../../build/verify/ocr_native_paragraph_quality_20261004.json)

这些完整日志在忽略提交的 `build/verify` 中，重建/清理时需要自行保留。本报告保存关键结果，不将用户 OCR 大量日志默认写入运行中的产品。

## 7. Performance

机器：Intel Core i7-12650H，高性能参考机，不是低配验收机。本轮未记录功耗模式/温度，尚无低配硬件实测。

| 原型配置 | 平均端到端 | P95（10 次 nearest rank） | 峰值工作集最大值 | 峰值进程提交量最大值 |
| --- | ---: | ---: | ---: | ---: |
| Explorer，4 线程，检测最短边下限 736，arena 开 | 2052.81 ms | 2105.97 ms | 380.41 MiB | 894.48 MiB |
| 段落，2 线程，检测最短边下限 320，arena 开 | 1412.43 ms | 1453.88 ms | 231.61 MiB | 731.00 MiB |

不要只报告工作集：提交量比实际驻留页高；低内存机器还受分页压力影响。CPU 时间（各线程求和）分别约 4.58–5.08 秒 / 1.77–1.95 秒，不是经过时间，也不是 CPU 百分比。

同为 4 线程、Explorer、关闭 busy spinning：关闭任务内内存 arena 时 10 次平均 3473.47 ms，工作集约 260 MiB；开启 arena 后平均 2052.81 ms，但瞬时工作集约 380 MiB。说明**过分节省识别过程中的内存，反而可能延长 CPU 高峰**。段落 2 线程 arena 关闭的单次 2653.1 ms 与开启后 10 次平均 1412.43 ms，只作为探索对照，统计口径不同不能当严格 A/B。

本原型总耗时还有约 0.75–0.85 秒启动/加载等开销。换成 C++ 不等于必然全部省掉：模型初始化仍存在，需重新测。测试模型文件合计约 15.44 MiB（检测 4,745,517 B，识别 10,857,958 B，分类 585,532 B），不是完整交付包大小。

性能分档应固定工作负载/准确率一起评估：建议低配验收至少覆盖 N100 + 8 GB + SSD 和旧款低压四核 + 8 GB + SSD，中档另选常见 U 系列；当前 H 系列仅为高性能参考。品牌/i5/i7/核心数不能单独决定分档。线程限制不能模拟低配处理器。验收时需记录冷进程/首启、P50/P95、峰值工作集/提交量、截图像素和文字行数、CPU 时间、退出后无 worker，以及同时监控时的响应影响。

**当前尚不满足“在低配机器也确保 1–2 秒”的验收；甚至本机 Explorer P95 也略超 2 秒。** 不能用识别阶段的 1.2 秒代替完整耗时宣传。

## 8. Build / tests

- Release 项目构建成功；`NskryOcrNativeProbe` 单独构建成功。
- 现有 CTest 4/4 通过。
- 评测纯逻辑 6/6 通过：漏字/错字/拆字编辑距离、仅空白归一化、空间位置隔离、图标插入非 exact、空结果、trace 解析与截断拒绝。
- 两种图片的模型最终配置各 10 次独立进程；原生段落 10 次生产管线；局部预处理矩阵。

## 9. Remaining known issues

1. 原生部分区域无正确候选；模型仍有图标误识别和标点差异。两张图不足以证明通用准确率。
2. 低配速度和内存峰值未验收；临时内存复用应该配内存预算/取消/超时，不能无上限并行。
3. 一次性 worker 当前只是 Python 评测方式，尚无产品中的 C++ worker、IPC、任务对象清理/异常回退，也未验收结果坐标和高亮。
4. 正式引擎包尚未制作；运行时、代码与具体权重的授权及第三方声明需要按实际交付物核对。现有机器的实验依赖不等于允许无审查再分发。
5. 当前 `det_limit_side_len` 是最短边下限，不是最大像素预算；不能当作限制大图内存的措施。动态识别最小宽度 32 的实验未带来可靠收益、且减少了一行输出，不采用。

## 10. Recommended next step

将 **PP-OCR mobile + ONNX Runtime 的一次性原生 worker** 作为准确模式的下一候选，而不是继续把 Windows OCR 同类全图预处理扩到更多轮；Windows OCR 保留轻量选择。这个判断由主 agent 根据代码和本地结果作出，低成本 subagent 仅核对公开部署/模型/许可事实。

下一实施顺序：先锁定可分发模型与运行时 → 小型 C++ worker 重现当前识别结果/原图坐标 → 主进程受控启动、取消、退出和资源回收 → 在 plugin 包中交付可选资源并明确回退 → 扩展不同字体/DPI/主题/表格图片 → 低配真机验收。正常监控帧不触发 OCR，不后台预热模型。不加入 LLM、联网识别或词典式纠错，不承诺任何 OCR 能对任意截图零错误。

资料依据：[RapidOCR 官方](https://github.com/RapidAI/RapidOCR)（离线、多语言/多语言绑定）；[ORT 线程配置](https://onnxruntime.ai/docs/performance/tune-performance/threading.html)（线程数、busy spinning）；[ORT C 接入](https://onnxruntime.ai/docs/get-started/with-c.html)；[PaddleOCR Windows C++ 部署](https://github.com/PaddlePaddle/PaddleOCR/blob/main/docs/version3.x/inference_deployment/local_inference/cpp/OCR_windows.en.md)。公开服务器性能表不能代替这里的端到端实测。
