# nskry-ocr OCR 优化方案

## 1. 性能结论

这些优化会增加一定 CPU 和 RAM
占用，但**图像预处理本身通常不是主要开销**。真正比较耗时的是多次调用
Windows OCR 引擎。

因此不要设计成"4 种预处理 × 多种倍率全部 OCR"。推荐采用：

``` text
普通图片 → 1 次 OCR → 质量足够 → 立即返回
困难图片 → 额外 1 次增强 OCR
仍然困难 → 最多第 3 次 OCR
```

也就是说，目标是让大多数正常请求仍然只有 **1 pass**，困难图片通常 **2
pass**，极端情况最多 **3 pass**。

------------------------------------------------------------------------

## 2. 推荐架构：Fast Path + Enhanced Path

``` text
Input Image
    ↓
估算文字尺寸 / 图片尺寸
    ↓
选择合理缩放倍率
    ↓
Windows OCR
    ↓
质量评分
    ↓
质量足够？
 ┌──Yes──→ 返回结果
 │
 No
 ↓
Enhanced Path
```

Enhanced Path 再根据情况选择：

-   灰度 + 对比度增强
-   轻度锐化
-   二值化
-   反色

不需要每一种版本都执行 OCR。

推荐顺序：

``` text
Original / Basic
      ↓
Enhanced
      ↓
Binary OR Invert
```

并限制：

``` text
MaxOcrPasses = 3
```

------------------------------------------------------------------------

## 3. 为什么不会明显拖垮性能

### CPU

Resize、Grayscale、Contrast、Sharpen、Threshold、Invert
都属于一次性的像素处理，通常远小于反复运行 OCR 引擎的成本。

真正应该控制的是：

``` text
OCR Engine Invocation Count
```

所以：

-   普通图片：1 次 OCR
-   困难图片：2 次 OCR
-   极困难图片：最多 3 次 OCR

不要默认同时跑很多 OCR。

### RAM

主要 RAM 来源是：

-   原始 Bitmap
-   缩放 Bitmap
-   预处理 Bitmap
-   OCR 输入
-   OCR 结果

不要同时保存所有预处理图片。

推荐串行处理：

``` text
Original
 ↓
Create Enhanced
 ↓
OCR
 ↓
Dispose Enhanced
 ↓
Create Binary
 ↓
OCR
 ↓
Dispose Binary
```

而不是一次保存：

``` text
Original
Enhanced
Binary
Invert
Sharpened
...
```

所有临时 Bitmap、Stream、SoftwareBitmap 等资源都要及时 Dispose。

------------------------------------------------------------------------

## 4. 不要只根据图片尺寸决定倍率

当前逻辑是"图片较小就再放大"，建议改成根据**估计文字高度**决定。

参考：

  估计文字高度     建议倍率
  -------------- ----------
  \< 12 px               4x
  12--18 px              3x
  18--28 px              2x
  \> 28 px               1x

同时必须遵守 Windows OCR 的尺寸上限。

如果无法可靠估计文字高度，则回退到现有安全缩放策略。

------------------------------------------------------------------------

## 5. 图像预处理

### 5.1 Grayscale

``` text
RGB → Grayscale
```

减少输入复杂度。

### 5.2 Contrast

适当增加文字和背景的亮度差。

建议做成配置参数，例如：

``` text
contrastFactor = 1.1 ~ 1.5
```

避免过度增强导致细笔画消失。

### 5.3 Sharpen

只做轻度锐化，目的是强化字体边缘，而不是制造噪声。

### 5.4 Threshold

不要固定只使用一个阈值。

可以准备：

``` text
100
140
180
```

但不要全部执行 OCR；应该根据前一次结果决定是否需要继续。

### 5.5 Invert

针对深色 UI：

``` text
Dark Background + Light Text
        ↓
Invert
        ↓
Light Background + Dark Text
```

建议自动判断背景亮度后再决定是否尝试。

------------------------------------------------------------------------

## 6. 结果评分：不要只比较"有效字符数量"

当前规则：

``` text
第二次识别出的有效字符更多 → 使用第二次结果
```

容易把错误结果选出来。

建议增加 `OcrQualityScore`，考虑：

1.  OCR Confidence（如果 Windows OCR 提供）
2.  有效字符比例
3.  异常字符比例
4.  文本长度
5.  行完整性
6.  行位置一致性
7.  与其他 OCR pass 的空间一致性

可以从类似下面的权重开始：

``` text
Score =
    0.35 * Confidence
  + 0.20 * ValidCharacterRatio
  + 0.15 * LineCompleteness
  + 0.15 * SpatialConsistency
  + 0.15 * CharacterQuality
```

权重应该可配置、可 Benchmark 调整。

如果 Windows OCR 无法提供可靠 confidence，不要伪造
confidence，而是重新归一化其他指标。

------------------------------------------------------------------------

## 7. OCR 结果合并

不要：

``` text
结果 B 字符更多 → 覆盖结果 A
```

建议：

``` text
OCR A
OCR B
OCR C
 ↓
BoundingBox 行匹配
 ↓
判断是否为同一行
 ↓
逐行计算 Score
 ↓
选择最佳行
 ↓
保留统一原图坐标
```

这样可以避免第二次 OCR 只是因为分词或空格不同，就被错误地认为更好。

------------------------------------------------------------------------

## 8. 多栏内容

如果 Windows OCR 把多栏内容顺序弄乱，可以基于 BoundingBox
做轻量空间排序：

1.  按 Y 聚类文字行。
2.  判断同一行中的文字块。
3.  按 X 从左到右排序。
4.  对明显多栏区域进行列分组。

不要强制重排所有 OCR 结果，简单布局继续保持 Windows OCR 原有顺序。

------------------------------------------------------------------------

## 9. UI 场景优化

你的 OCR 主要面向截图、钉住窗口、监控窗口、长截图等 UI
场景，因此应该重点优化：

-   深色 UI
-   浅色 UI
-   小字体
-   高 DPI
-   低对比
-   抗锯齿文字
-   中文 + 英文 + 数字
-   多栏 UI

不要为了自然语言文本而加入大型语言模型纠错。

尤其是：

-   文件名
-   路径
-   代码
-   数字
-   用户名
-   UI 标签

这些内容不适合强行做语言模型纠错。

------------------------------------------------------------------------

## 10. 并发与 UI 响应

第一版建议**串行 OCR**：

``` text
Pass A
 ↓
判断质量
 ↓
Pass B
 ↓
判断质量
 ↓
Pass C
```

不要默认同时启动三个 OCR。

OCR 和图像预处理都不应该阻塞 UI Thread：

``` text
UI Thread
   ↓
Start OCR Task
   ↓
Background Thread
   ├─ preprocessing
   ├─ OCR
   └─ scoring
   ↓
UI Thread
   ↓
Display Result
```

------------------------------------------------------------------------

## 11. Cancellation

如果用户连续触发 OCR，新请求应该取消旧请求。

建议：

``` text
CancellationToken
```

流程：

``` text
旧 OCR
   ↓
新 OCR 请求
   ↓
Cancel old request
   ↓
Start new request
```

避免后台继续浪费 CPU。

------------------------------------------------------------------------

## 12. 资源生命周期

临时资源必须及时释放：

``` csharp
using var resized = ...;
using var grayscale = ...;
using var binary = ...;
```

或者使用项目现有的等价 Dispose 规范。

特别注意：

-   Bitmap
-   SoftwareBitmap
-   MemoryStream
-   ImageStream
-   OCR 相关对象

不要因为一次 OCR 请求让大量图像长期留在内存中。

------------------------------------------------------------------------

## 13. 推荐最终 Pipeline

``` text
                         Input Image
                              │
                              ▼
                    ┌──────────────────┐
                    │ Image Validation │
                    └────────┬─────────┘
                             │
                             ▼
                    Estimate Text Size
                             │
                             ▼
                     Select Scale 1x~4x
                             │
                             ▼
                    ┌──────────────────┐
                    │    Fast Pass     │
                    │ Original/Basic   │
                    │ Windows OCR      │
                    └────────┬─────────┘
                             │
                             ▼
                      Quality Scoring
                             │
                    ┌────────┴─────────┐
                    │                  │
                  Good               Poor
                    │                  │
                    ▼                  ▼
                  Return        Enhanced Pass
                                   │
                         ┌─────────┼─────────┐
                         ▼         ▼         ▼
                     Enhanced   Binary    Invert
                         │         │         │
                         └────┬────┴────┬────┘
                              │         │
                              ▼         ▼
                         Windows OCR
                              │
                              ▼
                       Result Alignment
                              │
                              ▼
                        Result Scoring
                              │
                              ▼
                         Final Result
```

实际执行不需要三个增强版本全部跑：

``` text
Fast Pass
   ↓
Good → End

Poor
   ↓
Enhanced Pass
   ↓
Good → End

Poor
   ↓
Binary OR Invert
   ↓
Final
```

------------------------------------------------------------------------

## 14. 第一阶段必须实现

-   [ ] Fast Path / Enhanced Path
-   [ ] 动态文字高度缩放
-   [ ] Grayscale
-   [ ] Contrast Enhancement
-   [ ] 轻度 Sharpen
-   [ ] Binary Threshold
-   [ ] Invert
-   [ ] OCR pass 上限 3
-   [ ] OCR 结果质量评分
-   [ ] 按 BoundingBox 对齐行
-   [ ] 保持原图坐标
-   [ ] 临时 Bitmap 正确 Dispose
-   [ ] OCR 在后台线程运行
-   [ ] CancellationToken

------------------------------------------------------------------------

## 15. 第一阶段暂时不要实现

-   [ ] 大型 OCR 模型
-   [ ] LLM
-   [ ] 在线 API
-   [ ] 网络 OCR
-   [ ] 复杂语言模型纠错
-   [ ] 默认多线程同时 OCR
-   [ ] 永久缓存所有预处理图片

------------------------------------------------------------------------

## 16. 兼容性要求

必须保持：

1.  Windows OCR 仍然是主要 OCR 引擎。
2.  保留现有 OCR 语言选择和回退逻辑。
3.  原始图片继续用于结果展示。
4.  OCR 预处理图片不能影响最终显示图片。
5.  BoundingBox 最终必须映射回原图坐标。
6.  截图、钉住窗口、监控窗口、长截图等现有入口不能被破坏。
7.  增强流程失败时自动回退到现有 OCR。
8.  新功能不能导致整个 OCR 功能不可用。

------------------------------------------------------------------------

## 17. Agent 实现方式

不要一次性重写整个 `nskry-ocr`。

先定位：

1.  现有 OCR pipeline
2.  图片 resize 部分
3.  Windows OCR 调用部分
4.  两次 OCR 的 merge 部分

然后在现有架构上增加独立组件，例如：

``` text
ImagePreprocessor
OcrPassRunner
OcrResultScorer
OcrResultMerger
```

尽量减少 UI 层修改。

建议每完成一个模块就验证现有 OCR 功能。

------------------------------------------------------------------------

## 18. Benchmark

至少准备 30--50 张真实 UI 截图，覆盖：

-   小字体
-   深色 UI
-   浅色 UI
-   中文
-   英文
-   数字
-   中文 + 英文 + 数字
-   多栏
-   低对比
-   模糊
-   高 DPI
-   长截图
-   监控窗口

记录：

``` text
OCR total time
Preprocessing time
OCR engine time
Number of OCR passes
Average RAM
Peak RAM
Cancellation rate
字符准确率
行准确率
漏行率
```

重点目标：

``` text
普通图片：尽量维持 1 pass
困难图片：通常 2 pass
极困难图片：最多 3 pass
```

不要为了极少数图片的准确率，让所有 OCR 请求都变成 3--6 次 OCR。

------------------------------------------------------------------------

## 19. 推荐默认配置

``` text
MaxOcrPasses = 3

EnableGrayscale = true
EnableContrastEnhancement = true
EnableSharpen = true
EnableThreshold = true
EnableInvert = true

MinScale = 1.0
MaxScale = 4.0

TextHeightTarget:
    < 12px  → 4x
    12-18px → 3x
    18-28px → 2x
    > 28px  → 1x

DefaultThresholdCandidates:
    100
    140
    180

RunEnhancedPassOnlyWhenQualityIsLow = true
RunOcrPassesSequentially = true
CancelPreviousOcrOnNewRequest = true
DisposeTemporaryImagesImmediately = true
```

这些参数集中在配置中，不要散落在业务代码里。

------------------------------------------------------------------------

## 20. 最终设计原则

> **不要让所有图片都付出增强 OCR 的性能成本。**

目标：

``` text
简单图片
→ 快速 OCR
→ 立即返回

困难图片
→ 自动检测
→ 增强处理
→ 再 OCR

非常困难图片
→ 最多 3 passes
```

这样才能在**准确率、CPU、RAM、响应速度**之间取得合理平衡。
