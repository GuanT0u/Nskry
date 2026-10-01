# OCR Candidate Arbitration Implementation Report

验证日期：2026-10-01。依据 `docs/Proposal/nskry_ocr_candidate_arbitration_proposal.md` 的 Step 1→9 顺序开发；每步接入后均构建 OCR plugin。测试环境为当前 Windows 主机与已安装的 Windows OCR 中文语言包。

## 1. Changed files

- `plugins/nskry-ocr/src/nskry_ocr.cpp`：独立 pass、计时/trace、处理路径、仲裁接入、最终显示框、1x benchmark 开关。
- `plugins/nskry-ocr/src/ocr_arbitration.h/.cpp`：纯逻辑的行空间对齐、候选评分、冲突替换、未覆盖区域补入和弱几何检查。
- `plugins/nskry-ocr/CMakeLists.txt`、根 `CMakeLists.txt`：编译新模块，注册纯逻辑测试与 Explorer fixture 测试。
- `tests/ocr_arbitration_tests.cpp`：不依赖 Windows OCR 引擎的仲裁测试。
- `tests/ocr_plugin_smoke.cpp`：增加 PNG fixture/连续运行入口，保留原有双请求烟雾测试。
- `plugins/nskry-ocr/manifest.json`：插件版本升至 0.5.4；导出信息同步。
- 本报告。用户提供的 fixture、expected 文本及失败截图原样保留。

## 2. Architecture changes

Pass A/B/C 的原始 `RecognizedLine` 和 Windows word bbox 独立保存到仲裁结束。UI 所需的汉字等宽拆框仅在最终决策完成后生成，不再影响空间对齐。旧的 `MergeLines` baseline+append 路径已删除。Pass 结果只含文字、bbox、处理类型、缩放及耗时；预处理 bitmap 在对应识别后释放。trace 由 `NSKRY_OCR_TRACE_FILE` 显式开启，默认 Release 不写日志。仲裁异常安全回退到 Pass A。

## 3. Candidate arbitration algorithm

先按原图坐标中的纵向重叠、中心距离、横向距离组成行组，同一 pass 在一个组中至多有一行，避免仅凭 Y 把远处跨栏文字当成同一局部冲突。行候选评分由基本字符质量、跨 pass 文本一致性和弱 CJK 几何惩罚组成；归一化仅用于比较，包括空白折叠及相邻汉字间 OCR 空格折叠，不修改原始候选。B/C 同区域一致时可替换 A；再只把其他候选中确实未被 primary 覆盖的词框补入。重叠冲突不会并列拼接。没有 Windows OCR 逐字 confidence，因此未伪造置信度，也未使用词典或语义纠错。

纯逻辑测试覆盖同框两票一致替换、冲突不重复、中文两字缺口、英文 `build` 缺口、行对齐容差/跨栏分离、归一化与弱几何惩罚。

## 4. Fast / Enhanced / Third pass behavior

- Pass A：保留现有自动缩放 Fast Path；普通、质量足够的单行小字不再仅因行高 `<19` 固定跑 B。
- Pass B：灰度、对比、轻锐化；暗背景不再自动反色。
- Pass C：只在 A/B 后全局质量低、局部冲突、覆盖差异或几何异常时运行。暗背景且有冲突可选反色增强，否则选二值化。最多 3 pass。
- `NSKRY_OCR_BENCH_FAST_1X=1` 只供 1x 对照，不改变默认路径。

## 5. Regression fixture results

### explorer_dark_zh

Expected：`此电脑`、`类别`、`build`、`查看`、`文件夹`，来源于 `tests/ocr/fixtures/explorer_dark_zh.expected.txt`。

最终版本默认缩放时，A/B/C 各自均能在某处识别到 `此电脑`、`build`、`文件夹`；最终结果也有这三项。但 `类别` 与 `查看` 在三轮中都没有正确候选，最终也缺失。需要区别位置：左侧导航的 `此电脑` 被识别到；上方路径位置 A 为 `比申，月囟`，B/C 为 `比申，脑`，因此该位置仍然错误。全图 token 命中是 3/5，不能据此声称路径位置已修复。

本轮没有把三轮都不存在的文字硬编码进结果。失败归类为 Windows OCR / 当前 preprocessing 的候选生成问题，而不是候选仲裁能够自行解决的问题。完整逐词 bbox、A/B/C 文本与每个 group 的决策保存在 [最终 10 次 trace](../../../build/verify/ocr_explorer_final_v2_10runs_20261001.trace)；[1x 对照 trace](../../../build/verify/ocr_explorer_1x_final_10runs_20261001.trace)。

## 6. 10-run stability result

以下每行的 A/B/C 与 Final 列表示五个 expected token 中实际出现的项；每次运行的**完整** A/B/C 和逐组仲裁结果见上方 trace。10 次中三轮原始行输出各自完全一致，最终输出也完全一致；每次均 12 个冲突替换决策、2 个未覆盖词框补入决策。此处的决策数量不等于正确字数。

| Run | Pass count | A | B | C | Final | Total ms |
| ---: | ---: | --- | --- | --- | --- | ---: |
| 1 | 3 | 此电脑/build/文件夹 | 此电脑/build/文件夹 | 此电脑/build/文件夹 | 此电脑/build/文件夹 | 1216.00 |
| 2 | 3 | 同上 | 同上 | 同上 | 同上 | 1286.60 |
| 3 | 3 | 同上 | 同上 | 同上 | 同上 | 1349.35 |
| 4 | 3 | 同上 | 同上 | 同上 | 同上 | 1280.43 |
| 5 | 3 | 同上 | 同上 | 同上 | 同上 | 1267.71 |
| 6 | 3 | 同上 | 同上 | 同上 | 同上 | 1192.54 |
| 7 | 3 | 同上 | 同上 | 同上 | 同上 | 1176.13 |
| 8 | 3 | 同上 | 同上 | 同上 | 同上 | 1229.07 |
| 9 | 3 | 同上 | 同上 | 同上 | 同上 | 1195.66 |
| 10 | 3 | 同上 | 同上 | 同上 | 同上 | 1182.07 |

A/B/C 行输出哈希在 10 次中分别恒为 `FB737BF1 / 8302BEB3 / D21127E2`，最终行输出哈希恒为 `B4166520`（仅用于本次稳定性比对，不是质量分数）。

## 7. Performance

- 1 pass：普通烟雾图片的最终构建 trace 为 59.7 ms，1 pass（单次测量，不作为平均值）。
- 2 passes：未有独立、可代表真实图片的本次计时样本；代码保留条件触发，不虚构数据。
- 3 passes：Explorer 默认自动缩放 10 次平均总计 1237.6 ms；A/B/C 各轮预处理+引擎平均约 404.3 / 417.1 / 410.2 ms。
- Fast 1x 对照：同图 10 次平均总计 916.0 ms，首轮平均 219.4 ms；默认自动缩放首轮 94 行，强制 1x 仅 82 行，且 1x 首轮缺失多处 `文件夹`，需靠后续 pass 补回。最终两者都仍为 3/5 token，故暂不改默认 Fast 缩放。
- 未做长期 RAM 峰值的仪器化测量；临时 bitmap 仍按 pass 串行释放，pass 数据在请求结束后释放。

## 8. Build / tests

`cmake --build build/verify --config Release --parallel 2` 成功并生成 0.5.4 插件包；`ctest --test-dir build/verify --output-on-failure` 4/4 通过：Core、OCR smoke、Explorer fixture pipeline、OCR arbitration pure logic。Explorer fixture 测试验证流程可运行，不把当前无法达到的 5/5 准确率伪装成通过。

## 9. Remaining known issues

1. Explorer 的 `类别`、`查看` 在三轮原始 OCR 都未正确出现；路径位置的 `此电脑` 三轮均识别错误。这些不能由仲裁无中生有修正，Proposal §16.1 的最低准确率验收**尚未达成**。
2. 当前行/词级 heuristic 没有真实 OCR confidence；12 次替换并不保证每处更正确。只用这一张固定图也不足以证明其他浅色、低 DPI、多栏图片没有质量回归。
3. Windows `RecognizeAsync(...).get()` 运行中仍无法立即取消；代次检查继续丢弃过期结果，真实中断应另立任务。未改变 UI、截图入口或 plugin 包结构。

## 10. Recommended next step

先基于保留的 bbox trace 定位 `类别`、`查看` 和路径 `此电脑` 的区域，增加少量不同 DPI/主题的固定图片，再在 **最多三轮** 约束内评估局部 crop / resize 或 treatment 选择是否能让 Windows OCR 自身产生正确候选。若三轮仍无候选，应继续如实标记 OCR 引擎/预处理限制，而非加入词典硬修。原 `ocr_section14_progress.md` 可补充本报告链接；其中关于不能中断正在执行的 Windows OCR 的结论仍成立。
