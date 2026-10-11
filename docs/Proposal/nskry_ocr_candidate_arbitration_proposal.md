# Nskry OCR 第二阶段优化 Proposal
## 候选结果仲裁、局部漏字恢复与可观测性

> 历史 Windows OCR 仲裁设计。2026-10-12：独立候选、空间对齐和仲裁已实现；
> 当前默认精确引擎为一次性离线 worker，本文的 Windows 主引擎约束是当时阶段范围。
> 实施与限制见 [仲裁报告](../report/devProgress/ocr_candidate_arbitration_implementation_report.md)
> 和 [原生 worker 报告](../report/devProgress/ocr_native_worker_implementation_report.md)。
> Explorer 实际列名是“类型”；本文“类别”示例只用于通用短标签逻辑，不再作为该图片的正确预期。

- **适用项目：** Nskry
- **主要模块：** `plugins/nskry-ocr`
- **主要实现文件：** `plugins/nskry-ocr/src/nskry_ocr.cpp`
- **前置方案：** `docs/Proposal/nskry_ocr_optimization_spec.md`
- **前置进度报告：** [Section 14 历史进度](../report/devProgress/ocr_section14_progress.md)
- **目标阶段：** Section 14 第一阶段主体实现后的第二阶段 / Phase 1.5

---

## 0. 实施范围与验收约束（历史）

本 Proposal 记录分阶段实施依据；当前代码与现行公开契约优先于历史函数名和结构。

开始修改前必须先：

1. 阅读：
   - `docs/Proposal/nskry_ocr_optimization_spec.md`
   - `docs/report/devProgress/ocr_section14_progress.md`
   - `plugins/nskry-ocr/src/nskry_ocr.cpp`
   - 与 OCR plugin 构建、测试有关的 CMake / test 文件
2. 对照当前仓库代码确认本文描述是否仍然成立。
3. 如果当前代码已经变化，以当前代码为准，不要机械套用本文中的函数名或结构。
4. 不要一次性重写整个 OCR plugin。
5. 按本文阶段逐步实现，每个阶段均应保持项目可构建、OCR 基本功能可用。
6. 优先修复“结果仲裁 / merge / 评分 / 可观测性”，不要先继续堆叠更多图像预处理算法。
7. Windows OCR 仍然是主 OCR 引擎，不新增在线 OCR、LLM、大型本地 OCR 模型或词典式强制纠错。
8. 正常图片尽量维持 1 pass；困难图片最多 3 pass。
9. 所有最终 BoundingBox 必须保持为原图坐标。
10. 最终必须给出：
    - 修改文件列表
    - 关键设计说明
    - 构建结果
    - 测试结果
    - benchmark / fixture 结果
    - 尚未解决的问题
    - 对 `ocr_section14_progress.md` 或新的 progress 文件的更新建议

---

# 1. 当前问题

Section 14 第一阶段已经基本实现：

- Fast Path / Enhanced Path
- 动态文字高度缩放
- Grayscale
- Contrast Enhancement
- Sharpen
- Binary Threshold
- Invert
- OCR pass 上限
- OCR 结果质量评分
- BoundingBox 对齐
- 原图坐标映射
- 后台 worker
- 基本资源生命周期管理

但当前真实 UI 截图仍存在以下类型问题：

### 1.1 合法汉字错识别

例如：

```text
此电脑
```

可能识别为：

```text
比申，脑
比申，月囟
```

这些错误字符仍然是合法 Unicode 汉字，因此现有 `LineQuality()` 很难发现。

---

### 1.2 一个汉字被拆成多个字符

例如某些情况下：

```text
件
```

可能被 OCR 识别为两个偏旁 / 两个近似汉字。

如果这些字符均属于 Han 范围，当前“合法字符比例”仍可能给出较高分数。

---

### 1.3 局部漏字 / 漏词

已观察到类似：

```text
类别
build
查看
```

在最终结果中整体缺失。

同时图片其他区域识别正常，因此单纯依靠全局平均质量分数可能无法触发额外 pass。

---

# 2. 当前实现中需要重点验证的问题

以下内容是本阶段必须优先验证的假设，而不是未经验证就直接大改。

---

## 2.1 当前 Merge 更偏向“补空白”，而不是“候选替换”

当前 `MergeLines()` 的主要行为是：

```text
Fast Pass 结果作为 baseline
        ↓
Enhanced / Binary 得到 candidate
        ↓
如果 candidate 的 BoundingBox 区域已经被 baseline 覆盖
        ↓
candidate 可能直接被忽略
```

这种逻辑对于“补回原先完全没有识别出的文字”有效，但对于：

```text
Pass A: 比申，月囟
Pass B: 此电脑
```

这种**同区域内容冲突**不够有效。

本阶段必须把逻辑从：

```text
baseline + append missing boxes
```

调整为：

```text
preserve every pass
        ↓
spatial alignment
        ↓
candidate arbitration
        ↓
select / replace / supplement
```

---

## 2.2 当前评分无法判断“合法但错误”的汉字

类似：

```text
此电脑
比申，月囟
```

对现有字符合法性规则而言，两者都可能包含大量“有效字符”。

因此本阶段需要加入：

- pass 间一致性
- 几何合理性
- 局部覆盖情况
- 局部 conflict
- 行 / word 数量变化

而不是继续只增加“合法字符数量”权重。

---

## 2.3 `HasUncoveredHanGap()` 对短 UI Label 覆盖不足

Windows UI 中大量文字只有：

```text
类别
查看
文件夹
此电脑
```

即 2～4 个字符。

如果 gap 检测依赖足够多的单字符 Han box，它对短 UI 标签的作用有限。

不要简单把当前阈值从 4 改成 2 后就结束；需要避免制造大量误报。

本阶段更应该通过：

```text
Pass A / Pass B 空间覆盖差异
```

发现短标签漏识别。

---

## 2.4 Invert 当前与 Enhanced / Binary 耦合较强

当前暗背景检测后，预处理路径会直接改变灰度方向。

本阶段应该确认：

```text
Enhanced
```

和：

```text
Invert
```

是否应该成为可独立比较的 treatment。

不要默认认为暗色 UI 的所有增强 pass 都必须反色。

---

## 2.5 Fast Pass 的 resize 行为需要 benchmark

当前 Fast Pass 可能在 OCR 最大尺寸允许范围内自动放大。

这意味着 Fast Path 不一定是真正的原图 / 1x fast pass。

本阶段先测量：

```text
1x / current auto-scale
```

对以下指标的影响：

- OCR 时间
- 漏字率
- 中文错字率
- 英文 UI 文本识别
- ClearType / 抗锯齿字体识别

没有 benchmark 结果前，不要贸然彻底删除当前 resize 行为。

---

# 3. 本阶段目标

实现一个轻量级、多 pass 的候选仲裁层，使第二、第三 pass：

> 不只是“补 baseline 没有的文字”，也能够在同一空间区域内替换更差的 OCR 结果。

目标流程：

```text
Input
  │
  ▼
Fast Pass
  │
  ├── quality good + no suspicious local region
  │       └── return
  │
  ▼
Enhanced Pass
  │
  ▼
Preserve A + B independently
  │
  ▼
Align candidate lines / words by original-image coordinates
  │
  ▼
Detect:
  ├── agreement
  ├── conflict
  ├── missing region
  └── suspicious geometry
  │
  ├── enough confidence → final arbitration
  │
  ▼
Optional Pass C
  │
  ▼
A/B/C arbitration
  │
  ▼
Final lines + final word boxes
```

---

# 4. Phase A — 先增加 OCR 可观测性

这是本 Proposal 的第一优先级。

在不知道每个 pass 实际识别了什么之前，不允许继续盲调 preprocessing 参数。

---

## 4.1 增加内部 Pass Result

建议引入类似结构：

```cpp
enum class OcrPassKind {
    Fast,
    Enhanced,
    Binary,
    Invert
};

struct OcrPassResult {
    OcrPassKind kind{};
    double scale{1.0};

    std::vector<RecognizedLine> lines;

    double averageQuality{};
    int medianLineHeight{};

    // Debug / benchmark only
    double preprocessMs{};
    double engineMs{};
    double totalMs{};
};
```

具体名字可以按现有项目风格调整。

关键要求：

**每轮 OCR 结果在仲裁结束前必须彼此独立保存。**

不要：

```cpp
MergeLines(lines, passB);
```

之后直接丢失原始 Pass B。

---

## 4.2 Debug Dump

Debug 模式增加可关闭的 OCR trace。

示例：

```text
========== OCR PASS A ==========
Kind: Fast
Scale: 1.0
Preprocess: 0.0 ms
Engine: 34.2 ms
AverageQuality: 78.1

Line #0 [x,y,w,h]
Text: "..."
Word: [x,y,w,h] "..."

========== OCR PASS B ==========
Kind: Enhanced
Scale: 3.0
...
```

如果进行了仲裁：

```text
========== OCR ARBITRATION ==========
Group #12
A: "比申，月囟"
B: "此电脑"
C: "此电脑"

Decision: B
Reason:
- B/C text agreement
- strong spatial overlap
- geometry score higher
```

---

## 4.3 Debug 功能必须默认关闭

Release 正常使用时：

- 不写大量磁盘日志
- 不保存所有预处理 bitmap
- 不增加明显后台 RAM
- 不改变 UI

可以使用：

```cpp
#ifdef _DEBUG
```

或项目已有 debug / trace 开关。

---

# 5. Phase B — 不要在仲裁前强制拆分连续汉字

当前 `AddRecognizedWord()` 会在某些情况下把：

```text
此电脑
```

之类连续汉字按 BoundingBox 宽度等分为多个单字符 box。

本阶段应重新检查这个策略。

---

## 5.1 仲裁阶段保留 Windows OCR 原始 WordBox

建议结构分成：

```text
RawOcrWord
FinalDisplayWord
```

或等价方案。

OCR candidate arbitration 使用：

```text
Windows OCR 原始 word text
Windows OCR 原始 word bbox
```

不要先人为按字符平均切割。

原因：

- 错字数量变化会直接改变人为切割数量
- “一个字 → 两个偏旁”会产生完全不同的人工 bbox
- 不同 pass 的 word segmentation 本身可能不同
- 会降低跨 pass 空间对齐的可靠性

---

## 5.2 Highlight 如果仍需要字符级 box

可以在**最终结果已经确定后**再做：

```text
final selected word
        ↓
optional display splitting
        ↓
highlight boxes
```

不要让 UI highlight 的需求污染 candidate arbitration。

---

# 6. Phase C — 建立 Spatial Alignment

---

## 6.1 Line Alignment

不要仅使用文本内容进行匹配。

以原图坐标中的以下信息为主：

- Y overlap ratio
- center-Y distance
- line height ratio
- X overlap / horizontal gap
- overall bbox intersection

可以保留并改进现有 `SameTextLine()`。

建议形成：

```cpp
struct AlignedLineGroup {
    const RecognizedLine* fast{};
    const RecognizedLine* enhanced{};
    const RecognizedLine* third{};
};
```

实际实现可支持某一 pass 缺失：

```text
A only
A + B
B + C
A + B + C
```

---

## 6.2 不要要求三个 bbox 完全一致

以下属于正常情况：

```text
Pass A bbox: [100, 100, 151, 120]
Pass B bbox: [98, 99, 153, 121]
```

应该视作同一空间区域。

---

## 6.3 防止错误合并跨栏 UI

文件资源管理器这类界面可能同时存在：

```text
名称 | 修改日期 | 类型 | 大小
```

不要因为 Y 接近就把所有远距离 UI 元素错误合并成一个 word conflict。

Line alignment 可以覆盖整行，但**局部 conflict 分析必须考虑 X 区域**。

---

# 7. Phase D — Candidate Arbitration

这是本阶段核心。

---

## 7.1 不再采用“后来的 box 被覆盖就丢弃”

旧思路：

```text
candidate area 已被 existing 覆盖
→ continue
```

需要替换。

新思路：

```text
same spatial region
→ compare candidates
→ select / replace
```

---

# 8. 仲裁评分

不要伪造 Windows OCR confidence。

如果 Windows OCR 当前接口没有稳定可用的逐字置信度，则使用可解释的 heuristic。

建议：

```text
CandidateScore =
    BaseTextQuality
  + CrossPassAgreement
  + GeometryPlausibility
  + CoverageScore
  + StabilityScore
  - SuspiciousSegmentationPenalty
```

具体权重必须通过 fixture 调整，不要把本文数字当作绝对常量。

---

## 8.1 BaseTextQuality

可以复用并重构现有 `LineQuality()`，但它只作为一个维度。

包含：

- visible character count
- valid character ratio
- suspicious replacement chars
- empty line penalty

不要让它单独决定最终结果。

---

## 8.2 Cross-Pass Agreement

这是本阶段最重要的新信号之一。

例如：

```text
A = 比申，月囟
B = 此电脑
C = 此电脑
```

如果 B/C：

- bbox 高度重合
- spatial coverage 相近
- normalized text 一致

则 B/C 应得到显著 consensus bonus。

---

## 8.3 Normalization 只用于比较，不修改最终文本

可用于 comparison 的轻度 normalization：

- trim
- collapse repeated whitespace
- normalize CR/LF
- 可选统一常规 ASCII 空格

不要：

- 拼写纠错
- 中文词典替换
- 文件名猜测
- 路径猜测
- LLM correction

最终输出必须仍来自某个 OCR candidate 或明确的空间补全。

---

# 9. CJK Geometry Plausibility

用于检测：

```text
一个正常汉字区域
→ 被识别成多个汉字 / 偏旁近似字符
```

---

## 9.1 只做弱约束，不做强语言纠错

中文 UI 字体中，一个 CJK glyph 的视觉宽高通常不会出现极端不合理比例。

可以从：

```text
bbox width
bbox height
Han char count
```

估算：

```text
estimated char width = bbox width / Han char count
ratio = estimated char width / bbox height
```

如果 ratio 极端异常，可以扣分。

例如：

```text
一个接近单汉字宽度的区域
却输出两个 Han 字符
```

应该有一定 penalty。

---

## 9.2 必须宽容

以下情况不得被强行判错：

- 非等宽字体
- 字号变化
- 标点
- 中英文混排
- DPI scaling
- OCR bbox 本身不精确
- UI icon 附近文字

因此 geometry 只能作为：

```text
small / medium penalty
```

不能单独覆盖两个 pass 的强一致结果。

---

# 10. Local Conflict / Missing Region

本阶段不能再只看全局 `AverageQuality()`。

---

## 10.1 新增局部 disagreement 检测

例如：

```text
A 有 bbox
B 同区域也有 bbox
但文本明显不同
```

标记：

```text
LocalConflict
```

---

## 10.2 新增局部 coverage 差异

例如：

```text
Pass A:
名称 修改日期 大小

Pass B:
名称 修改日期 类别 大小
```

在 B 中：

```text
类别
```

对应区域在 A 没有任何显著覆盖。

应标记：

```text
RecoveredMissingRegion
```

并允许补入最终结果。

---

## 10.3 短中文 UI Label 必须覆盖

不能依赖“至少 4 个 Han 字符”。

以下必须能参与 missing-region 检测：

```text
类别
查看
文件夹
此电脑
```

同样要支持英文：

```text
build
plugins
Testing
```

---

# 11. 最终结果构建策略

不要简单：

```text
整行永远选 A
```

也不要直接：

```text
整行永远选分数最高者
```

推荐两层策略。

---

## 11.1 第一步：选 Primary Line Candidate

在 aligned group 中选择整体最可信的一行：

```text
A / B / C
```

主要依据：

- pass agreement
- line score
- bbox / geometry
- suspicious segmentation

---

## 11.2 第二步：补回 Primary 缺失的非冲突区域

如果 secondary candidate 有一个 word / span：

- 与 primary 同一 line
- 空间位置合理
- primary 在该 X 区域没有对应覆盖
- 不是明显噪声

则允许补入。

例如：

```text
Primary:
名称 修改日期 大小

Secondary:
名称 修改日期 类别 大小
```

最终：

```text
名称 修改日期 类别 大小
```

---

## 11.3 对重叠冲突区域不要同时拼接

例如：

```text
A: [同一 bbox] 比申，月囟
B: [同一 bbox] 此电脑
```

不能最终变成：

```text
比申，月囟此电脑
```

同一 conflict region 只能选择一个候选。

---

# 12. Third Pass 触发条件

第三轮不应该再主要依赖：

```text
AverageQuality < threshold
```

建议：

```text
NeedThirdPass =
    severe local conflict
 || suspicious geometry
 || important uncovered region
 || overall quality low
 || fast/enhanced result unstable
```

仍保持：

```text
MaxOcrPasses = 3
```

---

# 13. Treatment 策略重构

不要增加第 4、5、6 轮 OCR。

仍然最多三轮。

建议 treatment 明确化：

```cpp
enum class OcrTreatment {
    Basic,
    Enhanced,
    Binary,
    InvertEnhanced
};
```

名称可调整。

---

## 13.1 推荐初始策略

```text
Pass A:
Basic / Fast

Pass B:
Enhanced
- grayscale
- contrast
- light sharpen
- 不强制 invert

Pass C:
根据图片和 A/B disagreement 决定：
- Binary
或
- InvertEnhanced
```

---

## 13.2 暗背景不等于必须 invert

全局 border brightness 仍可作为信号，但不是绝对规则。

如果：

```text
A/B 在暗色 UI 已经一致
```

不需要为了暗背景再跑 invert。

---

# 14. Fast Pass Resize Benchmark

这一项先 benchmark，再决定最终代码。

至少比较：

```text
A. Fast Pass 强制 1x
B. 当前 auto-scale Fast Pass
```

记录：

- OCR Engine time
- total time
- recognized word count
- exact expected-token recovery
- 漏字数
- 错字数
- 第 2/3 pass 触发率

如果 1x 明显更快但准确率显著下降，则不要强行改成 1x。

---

# 15. Fixture / Benchmark

必须建立固定截图集。

不要继续主要依靠肉眼随机测试。

---

## 15.1 第一张必须加入的 Regression Fixture

使用当前已知失败的 Windows Explorer 深色模式中文截图。

建议：

```text
tests/ocr/fixtures/explorer_dark_zh.png
```

至少记录以下 expected tokens：

```text
此电脑
类别
build
查看
文件夹
```

如果完整 ground truth 容易维护，优先保存完整文本。

例如：

```text
tests/ocr/fixtures/explorer_dark_zh.expected.txt
```

---

## 15.2 后续至少包含

- Windows Explorer 浅色模式
- Windows Explorer 深色模式
- 2～4 字中文 UI 标签
- 中文 + 英文 + 数字
- 文件名
- 路径
- 小字体
- 125% / 150% / 200% DPI
- 深色 UI
- 低对比
- 长截图
- 多栏 / table-like UI

---

# 16. Benchmark 指标

每张 fixture 记录：

```text
Pass count
Fast pass time
Enhanced pass time
Third pass time
Total OCR time

Expected token hit rate
Missing token count
Wrong token count
Extra token count

Line count
Word count

Pass A/B/C disagreement count
Recovered missing region count
Conflict replacement count
```

---

## 16.1 Explorer regression 最低验收

对于当前失败截图，最终结果应稳定恢复：

```text
此电脑
类别
build
查看
```

至少不能继续稳定出现：

```text
比申，脑
比申，月囟
```

如果 Windows OCR 三个 pass 都没有任何一个 pass 得到正确文本：

- 不要伪造正确结果
- 在报告中明确标记为 OCR engine / preprocessing 层问题
- 保存对应 pass trace
- 再进入 preprocessing 调优

---

# 17. 重复稳定性测试

用户已经观察到偶发结果变化，因此 fixture 不应只跑一次。

建议至少：

```text
same image × 10 runs
```

Debug / benchmark 时记录：

```text
Final output stability
Pass trigger stability
Candidate decision stability
```

如果 Windows OCR 本身输出发生变化，需要把：

```text
engine instability
```

和：

```text
Nskry arbitration instability
```

区分开。

---

# 18. 性能约束

必须继续遵守原 Proposal：

```text
正常图片：
1 pass 优先

困难图片：
通常 2 pass

极困难图片：
最多 3 pass
```

禁止为了这次准确率问题改成：

```text
所有图片固定跑 3 pass
```

---

## 18.1 内存

不要同时长期保存：

```text
Original
Scaled A
Enhanced
Binary
Invert
```

Pass result 只保存：

- OCR text
- BoundingBox
- small metadata
- timing

临时 Bitmap 在对应 OCR 完成后立即释放。

---

# 19. Cancellation

真正中断已经进入：

```cpp
RecognizeAsync(...).get()
```

的 Windows OCR 调用，如果 API / 当前同步结构不适合安全取消，可以不在本 Proposal 中强行重构。

但必须继续：

- generation 检查
- 旧结果丢弃
- 新请求替换等待请求
- 不泄漏 Bitmap / Result

如果实现真正 cancellation 会显著扩大修改范围：

```text
记录为独立后续任务
```

不要为了它阻塞本次 candidate arbitration。

---

# 20. 建议代码模块

不要求一定拆文件，但逻辑上建议形成以下职责：

```text
OcrPassRunner
    运行单次 OCR

OcrPassResult
    保存独立 pass 结果

OcrResultScorer
    评分

OcrResultAligner
    空间对齐

OcrResultArbitrator
    conflict / consensus / missing-region 决策

OcrTrace
    Debug 日志 / benchmark
```

如果目前 `nskry_ocr.cpp` 继续单文件更符合项目阶段，也可以先在单文件中清晰分区实现。

**不要为了“架构漂亮”进行无必要的大规模文件拆分。**

---

# 21. 实现顺序

必须按以下顺序推进。

---

## Step 1 — Trace only

实现：

- independent `OcrPassResult`
- pass timing
- raw line / word dump
- 最终 decision dump

先不改变最终结果。

验证项目可构建。

---

## Step 2 — Preserve raw candidates

停止 Enhanced / Binary 识别后立即 merge 到 baseline。

保留：

```text
Pass A
Pass B
Pass C
```

验证现有 OCR 不崩溃。

---

## Step 3 — Line alignment

构建 aligned groups。

先输出 debug 结果，不立即替换最终文本。

---

## Step 4 — Candidate arbitration

实现：

- cross-pass consensus
- conflict replacement
- primary line selection

---

## Step 5 — Missing-region supplement

允许：

```text
secondary pass
```

补回 primary 没有覆盖到的区域。

重点验证：

```text
类别
build
查看
```

---

## Step 6 — Geometry heuristic

加入弱 CJK geometry penalty。

重点验证：

```text
件 → 被拆成两个字符
```

不要过度惩罚正常中文。

---

## Step 7 — Third-pass trigger

从纯全局平均分触发升级为：

```text
global quality
+
local conflict
+
coverage disagreement
+
geometry suspicion
```

---

## Step 8 — Treatment separation

把：

```text
Enhanced
Invert
```

解耦。

只在有证据时运行 Invert。

---

## Step 9 — Benchmark Fast resize

比较：

```text
1x
vs
current auto-scale
```

根据数据决定是否修改默认 Fast Path。

---

# 22. 不要做的事情

本阶段禁止：

- 引入 Tesseract / PaddleOCR 等第二 OCR 引擎
- 引入云 OCR
- 引入 LLM
- 引入大型语言模型纠错
- 建中文字典强行替换 OCR 内容
- 对文件名做语义猜测
- 对路径做语义猜测
- 默认固定跑 3 次 OCR
- 增加 4～8 个 threshold 后全部 OCR
- 保存全部 preprocessing bitmap
- 为了本次优化重写 OCR UI
- 修改截图 / 长截图 / PiP 等无关模块
- 将 benchmark-only 日志默认开启到 Release

---

# 23. Compatibility 要求

必须保持：

1. Windows OCR 为主引擎。
2. 保留现有语言选择和回退。
3. OCR 展示仍使用原始图片。
4. 最终 bbox 为原图坐标。
5. Highlight / Copy / Save / Rotate 不退化。
6. Screenshot / pinned image / monitor / long screenshot 的 OCR 入口不被破坏。
7. Enhanced 失败时仍保留 Fast Pass。
8. Arbitration 发生异常时应能安全回退到 Fast Pass，而不是让 OCR 整体失败。
9. 官方 plugin 打包流程不被破坏。

---

# 24. 测试要求

至少执行：

```text
CMake configure
Ninja / project build
现有 tests
OCR plugin build
```

如果仓库已有对应测试 target，优先扩展现有测试。

新增逻辑应尽量给：

- spatial alignment
- text normalization
- geometry score
- candidate consensus
- missing-region supplement

增加不依赖真实 Windows OCR engine 的纯函数测试。

---

# 25. 建议增加的单元测试

例如：

### Case A — 相同 bbox，不同文本，两票一致

```text
A = 比申，月囟
B = 此电脑
C = 此电脑

Expected:
此电脑
```

---

### Case B — primary 漏一个区域

```text
A:
名称 | 修改日期 | 大小

B:
名称 | 修改日期 | 类别 | 大小

Expected:
类别被补回
```

---

### Case C — 不允许重复拼接 conflict

```text
A bbox == B bbox
A = 错误
B = 正确

Expected:
只能保留一个
不能得到：
错误正确
```

---

### Case D — 两个短 Han 字符也能参与 coverage comparison

```text
类别
查看
```

不能因为字符数 < 4 就完全不参与局部 missing 检测。

---

### Case E — English UI token

```text
build
```

必须参与 missing-region recovery。

---

# 26. 完成标准

本 Proposal 只有满足以下条件才算完成。

- [ ] 各 OCR pass 独立保留到仲裁阶段
- [ ] Debug 能看到 A/B/C 原始输出
- [ ] 不再只采用“已有 bbox 覆盖 → 丢弃 candidate”
- [ ] 同区域冲突可以执行 replacement
- [ ] 多 pass 一致性参与评分
- [ ] 短中文 label 可参与局部漏字检测
- [ ] 英文 UI token 可参与局部漏字检测
- [ ] 支持补回 non-overlapping missing region
- [ ] conflict region 不会重复拼接
- [ ] CJK geometry heuristic 为弱约束
- [ ] 第三 pass 不再只看全局平均 quality
- [ ] Enhanced 与 Invert 行为被明确分离或完成数据验证后说明为何保持现状
- [ ] 当前 Explorer regression fixture 已加入测试 / benchmark
- [ ] 当前 screenshot 至少重复运行 10 次
- [ ] 输出 OCR timing / pass count
- [ ] 正常 Fast Path 没有被改成固定多 pass
- [ ] Release 不默认输出大量 debug 日志
- [ ] 所有临时图像资源仍及时释放
- [ ] 项目构建成功
- [ ] 现有 OCR 基础功能无明显 regression

---

# 27. 最终报告格式

实施报告采用以下结构：

```markdown
# OCR Candidate Arbitration Implementation Report

## 1. Changed files

## 2. Architecture changes

## 3. Candidate arbitration algorithm

## 4. Fast / Enhanced / Third pass behavior

## 5. Regression fixture results

### explorer_dark_zh
Expected:
- 此电脑
- 类别
- build
- 查看

Result:
...

## 6. 10-run stability result

## 7. Performance
- 1 pass:
- 2 passes:
- 3 passes:

## 8. Build / tests

## 9. Remaining known issues

## 10. Recommended next step
```

---

# 28. 核心原则

本阶段最重要的原则：

> **不要假定后续 OCR pass 只能补字。后续 pass 也可能识别出比 Fast Pass 更正确的同区域文本。**

因此必须从：

```text
Baseline + Append
```

升级为：

```text
Independent Candidates
        ↓
Spatial Alignment
        ↓
Consensus / Conflict / Missing Detection
        ↓
Arbitration
        ↓
Final Result
```

同时继续遵守：

> **不要为了少量困难图片，让所有 OCR 请求都支付 3 次 Windows OCR 的成本。**

先把“已经识别出来的正确候选能不能被 Nskry 正确选中”解决，再考虑增加更多图像预处理。
