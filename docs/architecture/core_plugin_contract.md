# Nskry 基础能力与插件边界（2026-10-08）

状态：当前 API 2 的规范与实现核对。本文件是二次规划的补充，以实际版本化 SDK 为执行契约。
“计划”条目不是已经可调用的接口；不改变 installed != loaded、设置按需创建或重启应用更新原则。

## 1. 确定的功能归属

| 能力 / UI | 归属 | 不装插件时 | 扩展方式 |
| --- | --- | --- | --- |
| Tray、快捷键、Settings、插件安装/注册/更新 | Core | 可用 | Settings service 与宿主命令，不允许插件直接改管理器数据 |
| 框选、范围调整、放大镜、Alt 精调 | Core | 可用 | 插件接收截取后的位图，不接管全局选择窗口 |
| 复制图片、保存 PNG、固定窗口 | Core | 可用 | SDK copy 回调；保存/固定现为宿主 UI，不宣称有独立 SDK 回调 |
| 监控范围、WGC/PiP、窗口拖动/缩放/最小化/关闭 | Core | 可用 | 通过 post_capture 获取一次当前画面；不是给每帧自动执行插件 |
| 图片裁剪/尺寸调整/平移缩放、标注编辑 | Core 共享 Image Editor | 可用 | API 2 `openBitmapEditor`；不依赖滚动截图 DLL |
| 整行/整列区段删除并拼合 | Core Image Editor | 可用，无需插件 | 适用于普通截图、长图、固定图及监控静态帧，不属于滚动采集算法 |
| 宿主窗口按钮布局、菜单、悬停、基础视觉资源 | Core | 可用 | 插件只声明动作；宿主负责渲染、限额、生命周期 |
| 滚动采集、拼接、自动滚动与采集预览 | nskry-scroll | 无该可选动作 | toolbar_action；完成后交给宿主编辑器 |
| OCR 引擎、模型、识别结果页与文字选择 | nskry-ocr | 无该可选动作 | toolbar_action + post_capture；Edit 使用 Core |
| 单屏固定区域 MP4/H.264 录屏 | nskry-record | 无该可选动作 | toolbar_action + 可选区域采集/命令 v1；控制/编码/文件归插件 |
| 翻译、上传等专项功能 | 后续插件 | 尚未提供 | 单独包、按需资源；不得偷用 OCR/Scroll 私有 UI |

裁剪/标注是多种图像功能共有的能力，确定并入 Core，不再拆一个“必须安装的编辑插件”。
不为已确定的基础编辑窗口增加前置安装链；这不禁止其他独立、边界清晰的服务插件成为前置。
当前编辑实现历史文件名为
`src/ui/long_image_crop_window.*`，但它由 Nskry.exe 编译，不属于 `plugins/nskry-scroll/`。
窗口标题已改为 Image editor / Crop；名称迁移不是功能或 ABI 依赖。
区段编辑使用非破坏坐标映射；只在导出时物化像素。裁剪后的图像动作没有可回溯的实时窗口来源，
`sourceHwnd` 为 null，`capturedRegion` 仅作为结果页放置锚点，不得据此继续采集编辑器窗口。
聚焦隐藏、阴影与圆角属于 Core 悬浮窗策略/显示层，不改变插件 ABI，也不会进入 Copy/Save/OCR 像素。

## 2. OCR Edit 的实际链路

OCR result → 复制原图 → `NskryHostContext.openBitmapEditor` → `main.cpp::OpenBitmapEditor`
→ Core crop window → Core annotation engine / pin editor。

不存在 `OCR → LoadLibrary(nskry-scroll)`。没有 OCR 时宿主仍能裁剪、标注、复制/保存；
没有 Scroll 时 OCR Edit 仍能用。删除/禁用一个插件只能删除其贡献的动作，不能删除 Core 控件。

## 3. 悬浮按钮 / 菜单扩展契约

宿主保留 Edit、Minimize、Close。可选图像动作通过 manifest `post_capture` 和 SDK execute 提供：

- 无动作：隐藏图像动作按钮，不留下点击无效果的 OCR 占位。
- 一个动作：点击直接执行，仍然只在执行时 EnsureLoaded。
- 多个动作：`…` 按钮展示宿主菜单，由用户选择；不因名称或 `nskry-ocr` ID 给予特殊入口。
- Pin 使用图片原像素；PiP 复制当前可用帧并在调用前解除选入 DC，帧句柄只在同步调用期借用。
- 右键菜单同样接入 post_capture；编辑器允许先选图像区域再执行动作。
- 当前菜单最多展示 100 个动作；按 registry 既有排序，不支持任意像素位置或自定义 HWND 注入。

这已经是 API 2 可使用的声明式接口：不是让作者操作宿主的 BUTTON HWND。
宿主内部 `ui/image_action.h` 的 C++ 容器不是公开插件 ABI。
实时截图工具栏用 `toolbar_action`；长截图这种需要页面滚动的动作不应仅因有图像就声明 post_capture。
窗口打开时是元数据快照，启用状态以执行时 runtime 再检查为准；更新菜单不需要加载 DLL。

API 2 每插件只有主 execute 动作。2026-10-15 的 [可选命令/采集 v1](../plugins/recording_extensions_v1.md)
允许已加载任务的显式控制命令，不负责独立任务启动；standalone 位仍未启用。
更广的多个动作、动作图标、排序/分组、专用 hover 快捷槽，
需要后续 API 3 的版本化 Action Descriptor；不要先占用 reserved 字段塞未定义指针。

## 4. 宿主服务 vs 前置插件

允许依赖：已声明版本的宿主 SDK、包内 private DLL/EXE/模型、明确系统运行库。
项目允许显式前置插件：前置必须独立交付、职责单一，不能为了小能力强制安装无关大功能。
例如可将共享识别引擎独立为按需服务包，而不能要求翻译插件安装整个录屏插件来借用其中的识别模块。
“禁止隐式依赖”不等于“禁止前置插件”。
当前 API 2 尚不支持依赖另一个已安装插件的 DLL、导出符号、窗口类、目录或回调。
`requires_plugins` 非空会被当前 manifest loader 拒绝，不能宣称已实现依赖管理。
普通未知描述性字段仍可保留，为未来 metadata 增长留空间。

若多个插件共享逻辑，先判断：

1. 图像通用 UI/操作 → Core 服务，按需实例化。
2. 无 UI 的算法库 → 源码/静态库复用或包内 private 库；版本由各包自含，不依赖另一个插件安装。
3. 重引擎/模型 → 按需独立 worker，不能借“通用”之名让 Core 常驻。
4. 真正可替换服务提供者 → 先做 API 3 service query/依赖版本/禁用卸载冲突设计，再实施。

轻库无需一律提升为 Core；多个插件确实需要、可独立测试的专用服务可规划为前置插件。
前置的声明、安装确认、服务协商与资源生命周期未实现前，保持当前拒绝检查，不能仅移除检查。
详细规划见 [边界与前置补充](../Proposal/plugin_prerequisites_and_image_splice.md)。

未来依赖方案须含：可选/必需区分、版本范围、能力协商、循环检测、缺失提示、
禁用/卸载影响确认与多版本冲突；不能把两个插件的 execute 函数互相转发当成依赖协议。

## 5. 现有 SDK 的线程与所有权

`capturedBitmap` 借用，execute 返回后若仍需要必须先复制；不得 DeleteObject 宿主句柄。
`openBitmapEditor` 转移所有权，调用后不再访问/删除该图；copy callback 不转移所有权。
UI 回调在宿主 UI 线程使用，后台完成通过 UI dispatch 交付，不能保存栈上 context 指针。
共享 immediate D3D context 不应由插件后台线程并发操作；异步图像处理优先自有复制资源。
禁用只阻止新的宿主 execute，已打开的插件 UI/任务不做强制热中断；已加载状态明确提示重启。
shutdown 清理自己线程/任务、窗口、COM/GPU/GDI、callback；已载入 DLL 在退出前保留，更新/卸载下次重启应用。
进程内原生插件不是沙箱，结构性约束不能防御故意恶意的 DLL。

## 6. 结构审核与不变项

Runtime、Registry、Package、Update、Settings service 已分开，源码目录与安装目录是不同视图，并未整体偏离二次规划。
动作枚举是 runtime 的轻量 metadata facade，可保留；不必为了“只做生命周期”复制一个同职能管理器。
目前偏差是 Core 窗口曾把 OCR ID/名称硬编码，已改为通用动作；共享编辑器的历史命名已明确归属。

保留 `structSize`、API version、reserved[4]、on_demand/startup、post_capture/toolbar_action、
更新 staging/rollback、Settings service callback、模型 trace/test：这些是契约/规划，不按“暂时用不到”删除。
`standalone` 位保留，但当前没有独立命令注册 surface，SDK 注释明确预留而非宣称已实现。
不扩展 API 2 结构，不热更新 DLL，不引入 WebView、服务进程或启动时 catalog 网络请求。
