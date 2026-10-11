# 固定图/PiP 阴影与圆角：四种实现路径

> 2026-10-12：几何外缘缓存与原生/region 圆角首版已实现，见 [实施报告](../report/devProgress/resource_first_features_20261011.md)。
> 下方“当前结构”指实施前基线；实机视觉、多屏和与 DComp 的性能对照尚未完成。

日期：2026-10-11。状态：方案比较，未实现。总评见 [资源优先总案](resource_first_features_20261011.md)。

## 1. 当前结构与不可忽略的限制

Pin 是 GDI 静态图，PiP 是 GPU DXGI SwapChain；两者均为无标题栏 popup、手动 resize hit-test。
WM_NCCALCSIZE 返回 0，WM_NCACTIVATE/WM_NCPAINT 抑制系统边框，已有历史焦点边框和最小尺寸问题不能复发。
当前只设置 DWMWA_BORDER_COLOR，不存在用户阴影/圆角设置。
两者悬浮按钮有自己的绘制/透明度，不能把“整个窗口变半透明”用作调节阴影。

系统原生圆角只是 DEFAULT/ROUND/ROUNDSMALL 等偏好，不是任意半径 API；无框、区域裁剪、逐像素 alpha 等会影响适用性。
原生窗口区域与 per-pixel alpha 不支持系统自动圆角，调用返回成功也不是外观保证。
依据：[Microsoft Windows 11 圆角说明](https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/ui/apply-rounded-corners)。

原生 DWM 阴影没有本需求所需的公开透明度/模糊/范围调节参数；不使用私有 API、注册表 patch 或注入 DWM。
因此 A 是最低资源基准但非完整候选，B/C/D 才是满足自定义功能的三条不同技术路径。

## 2. 共同行为与设置

### 开关与默认规则

阴影、圆角各自独立可选；建议新选项初始关闭以保持现有截图外观，用户启用时：
圆角规则默认 Win11 系统偏好；阴影默认 macOS 风格预设。
这区分“是否开启效果”和“开启后用什么默认规则”，不是把 Win11/macOS 混成一个无法解释的开关。

每个区块提供 Win11 / macOS / 自定义三个预设。macOS 仅是 Nskry 的柔和视觉风格，
不宣称复制 Apple 私有、随版本/焦点变化的准确参数；Win11 自定义阴影也只是近似风格。

### 建议初始参数（产品草案，不是系统实测常量）

| 预设 | 阴影不透明度 | blur sigma | 外部范围 | Y 偏移 | 圆角规则/可编辑参考值 |
| --- | --- | --- | --- | --- | --- |
| Win11 风格 | 18% | 4 DIP | 12 DIP | 2 DIP | 系统偏好；转自定义参考 8 DIP |
| macOS 风格 | 22% | 6 DIP | 18 DIP | 3 DIP | 固定 10 DIP |
| 自定义 | 沿用切换前的数值 | 同左 | 同左 | 同左 | 固定或短边百分比 |

阴影 UI：透明度 0–50%，模糊 sigma 0–8 DIP，范围 0–24 DIP，偏移 0–6 DIP。
范围小于模糊支撑时采用边缘平滑截断，不突然切出硬框；具体核形状统一，避免各实现的 blur 名称不可比。
圆角 UI：固定值 0–24 DIP，或短边的 0–15%；半径上限为窗口短边的一半，极小窗口必须再限制。
百分比明确为 `radius = min(clientWidth,clientHeight) × percent/100`，不是屏幕百分比、不是圆角直径百分比。

Win11 系统圆角无法公开读取实际半径；转“自定义”沿用显式保存的参考值（界面标注“参考”），
不能伪造已读到真实 DWM 数值。已编辑自定义数值不因一次下拉/取消丢失。
固定 DIP 随每窗口 DPI 转为物理像素；百分比随窗口大小变化。纯移动不重新计算。

Settings 存持久默认，Pin/PiP 共享同一个 WindowVisualSettings 值对象；设置改变可显式应用现有窗口。
滑动条预览合并更新，松开/应用时保存，不每个鼠标消息写 JSON。
不额外常驻配置/图像 worker。非法配置钳制或回退默认；保存失败可回滚；关闭效果释放自有缓存/辅助 HWND。

## 3. A：完全交给系统 DWM（最低资源基准，但功能不全）

尝试 DWMWA_WINDOW_CORNER_PREFERENCE 与支持的 frame/非客户区配置，依赖系统阴影。
应用不生成阴影像素、不增加专用渲染循环；通常是最少的自有资源。
但不能提供自定义阴影参数或百分比半径，当前无边框样式也未必被 DWM 圆角接受。
不能为获取阴影恢复可见粗边框/标题栏，不能把阴影开关假实现为窗口透明度。

结论：只作 Win11 默认路径和性能参考，**不能单独满足需求，也不能宣称三种完整方案之一**。

## 4. B：缓存边缘阴影 + 原生/区域圆角（低资源首选）

不改 Pin GDI/PiP SwapChain 的内容渲染路径。
阴影依据窗口几何而不是每帧内容：生成少量角片/边片 alpha，缓存后只在大小、DPI、半径或参数变化时更新。
采用矩形周边分片承载，避免一个“中间全透明”的全窗口大位图；考虑最多 8 个不激活的薄层辅助窗口。
阴影窗口不进任务栏、不接管鼠标、位于内容下方、跟随最小化/自动隐藏/关闭；中心必须真正不遮挡输入。

阴影不跟随视频内容做高斯模糊；纯拖动仅批量移动分片，不重算 blur，不在每帧 UpdateLayeredWindow。
系统合成依然有成本，不称为 GPU 零开销。辅助窗口数量也是成本，需要同时测 1/10/30 个固定图。
层窗口更新会涉及像素上传；尺寸越小通常更合适。
依据：[Microsoft UpdateLayeredWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-updatelayeredwindow)。

圆角：Win11 默认先走原生偏好；自定义/不适用时通过 CreateRoundRectRgn + SetWindowRgn。
区域只在几何/DPI/参数变化时更新，主窗口内容不用读回；缺点是边缘可能有像素阶梯。
启用区域后不得叠加原生圆角并假定两者能同时控制；切回原生先移除 region。
SetWindowRgn 成功后区域所有权交给系统，失败才由调用者释放。
依据：[Microsoft SetWindowRgn](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowrgn)。

额外辅助阴影窗口也可能影响 DWM 原生圆角启发式：必须做真实 UI 验证，失败走明确的区域回退。
回退质量不满意时可升级到 C 的抗锯齿 clip，不能默默宣称“这是系统原生 Win11 圆角”。

### 内存估计

全窗口阴影 BGRA 表面约 `4(W+2b)(H+2b)` 字节；窄周边约 `4[(W+2b)(H+2b)-WH]` 字节，
另有角片/缓存、句柄及系统合成开销。这里是几何估计，不是工作集实测。
1920×1080、外扩 32 物理像素，周边像素约 0.75 MiB，而完整透明中心表面约 8.66 MiB。
分片/共享缓存可以进一步复用，但不能忽略每个辅助 HWND 的实际开销。

### 更新约束

缓存键含 DPI/预设数值/半径/几何，使用有界 LRU；内容窗口缩放时边片拉伸/重排，结束后精确重建。
尺寸操作期间只合并一次性重绘，不开长驻追踪 timer；关闭效果后释放；限制最大物理阴影宽度与总缓存字节。
z-order 同步在窗口位置消息处理，防重入；两张固定图之间阴影必须跟随各自宿主，不能盖住另一张图的内容。

## 5. C：DirectComposition 几何层、阴影效果与圆角 clip

改为合成树：内容 visual + 独立、静态几何 shadow visual + 带圆角参数的 rectangle clip。
PiP 可用 composition swap chain，Pin 静态图上传一次后复用。阴影输入是窗口形状，不是视频纹理。
参数变更只 Commit 属性；静态效果不由应用逐帧跑 CPU blur；圆角可抗锯齿，数值/百分比可控。

代价：需要重构两种窗口显示路径，增加 DComp/图形资源、设备丢失处理和 fallback；
Pin 原本无需 GPU 专属 surface，可能增加内存；低配集显/RDP 的合成成本必须测量。
自定义 clip 的视觉边界与输入命中仍须统一，不能把透明角落变成能拦截其他窗口的矩形。
无框外部阴影仍需足够的透明承载空间或辅助宿主；不能画到 HWND 之外又不保留 margin，
margin 必须与图片区域/缩放命中分开，初始图片坐标与大小不得因外扩改变。

依据：[Microsoft ShadowEffect](https://learn.microsoft.com/en-us/windows/win32/api/dcomp/nn-dcomp-idcompositionshadoweffect)、
[RectangleClip](https://learn.microsoft.com/en-us/windows/win32/api/dcomp/nn-dcomp-idcompositionrectangleclip)。

这是视觉质量优先时的候选；GPU 加速不自动等于总资源最少。它可能比 B 的多 HWND 更合适，但需实测。

## 6. D：CPU 合成整窗口，逐像素 alpha layered window

软件渲染驱动：将图片/实时帧、圆角遮罩、阴影一起绘制到完整 BGRA 缓冲，再 UpdateLayeredWindow。
自定义参数容易一致，抗锯齿质量可控，Pin 静态时可缓存整窗结果。

PiP 每帧需要 GPU staging/读回、CPU 混合与整窗更新，打断现有 GPU 链路；
分辨率/刷新率增大时带宽和 CPU 更不利。即便阴影缓存了，实时内容仍要更新。
主窗口逐像素 alpha 路径不再使用系统原生圆角；需要自己负责透明像素命中与边界。
全窗输入+输出+staging 多份缓冲，低配机最不适合连续监控，故不选为共同默认。

## 7. 分别判断阴影和圆角的最省资源路径

| 组件 | 默认系统规则 | 完整自定义需求 | 次选 | 不推荐 |
| --- | --- | --- | --- | --- |
| 阴影 | 原生 DWM 自有资源最少，但无法给滑条 | B 几何缓存窄周边、事件更新 | C 静态几何合成 | D 实时 CPU 整窗混合 |
| 圆角 | A Win11 原生偏好 | B region 更新，接受边缘质量差异 | C 抗锯齿 clip | D 实时整窗读回 |

**推荐 B 为当前低资源交付主路线，默认圆角尽量借 A；C 留作质量/多窗口实测后的替代实现。**
“B 最省”是现有 GDI/GPU 分离架构下的预测，不是跨硬件结论；多辅助 HWND 的成本可能让 C 反超。
必须先做窄周边+region 的小原型资源/交互门禁；若跨窗口输入/z-order/边缘质量不合格，不硬上线 B。
D 不作为低配 PiP 方案。所有自定义阴影都使用圆角矩形形状，不模糊截图/文字本身。

## 8. 验收与回退

回归：初始图片像素坐标/尺寸、激活/失焦无粗边框、小窗口不放大、四角等比/四边自由缩放、悬浮按钮、最小化。
圆角：100/150/200% DPI、跨屏、极小窗口、百分比、固定值、移除 region、Win10/Win11、远程/设备丢失。
阴影：纯拖动无生成/blur，PiP 60fps 时阴影生成次数不随帧数增长；下方窗口能正常点，透明角落行为正确。
同步：自动隐藏/最小化/源关闭时全部辅助视觉同步消失，关闭无悬空阴影、无 USER/GDI 句柄增长。
导出/OCR：效果只影响宿主显示，不烘焙入图片、剪贴板、OCR 图或源监控画面。
Settings：自定义沿用上一个预设数值、取消恢复、合法范围、预览限频、失败回滚；新版本读取旧设置不改变已有行为。
资源统计包含 Nskry、DWM 和 GPU；不能只量主进程 CPU 后就称系统开销为零。
