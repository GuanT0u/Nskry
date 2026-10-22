# Nskry 项目概况与维护指南

更新日期：2026-10-22。本文描述当前源码状态；性能数据的测量日期和限制见对应报告。

## 当前功能与边界

Core 提供框选、范围调整、放大镜和 Alt 精调、截图标注、复制/PNG 保存、固定图、
WGC/PiP 监控、窗口管理、设置和插件基础设施。通用 Image Editor 支持裁剪、缩放/平移、
整行/整列区段切除并拼合，以及后续标注入口，普通截图也可使用。

长截图采集由 `plugins/nskry-scroll/` 提供；OCR 推理和结果页由 `plugins/nskry-ocr/` 提供。
OCR Edit 调用 Core 编辑器，无需安装长截图插件。翻译尚未交付。
2026-10-15：独立 nskry-record 已有单屏固定区域 MP4/H.264 首版，按需一个录制 worker；无音频/GIF。
暂停关闭采集，停止封装后发布，取消清理临时文件。实测/未验收见 [录屏报告](report/devProgress/recording_phase5_20261015.md)。
0.1.1 增加失败片段收尾、恢复/取消竞争测试、存储故障注入及按需资源基准，修正约 16fps 的等待节奏。
历史资源问题见 [可靠性复测](report/devProgress/recording_hardening_20261015.md)。
0.2.0 将 MF 编码移入每次录制才启动的私有进程，增加暂停/继续快捷键、SSE2 像素转换、BT.709 颜色修复、控制窗内关闭确认及进程故障回归。
SDK/API 2 和 Core 采集不变；编码隔离不等于 WGC 已隔离，限制及最新数据见 [编码隔离报告](report/devProgress/recording_isolation_20261019.md)。
插件动作由 manifest 元数据声明，宿主决定工具栏、悬浮按钮和菜单布局。
0.3.0 增加录前30/60fps与三档质量、可选窗口跟随、独立GIF按钮、录后保存/复制文件/起止裁剪。
可选采集v2、额外capture动作v1和插件设置UI v1均不改API 2布局；旧插件无需重编译。
0.4.0 固定区域v2改用Desktop Duplication，消除显示器WGC全屏黄框；窗口跟随仍用窗口WGC。
录前选区红框、下方水平工具条、逐帧自适应GIF调色板和按需首尾时间轴预览已接入。
当前兼容边界、测试与限制见 [0.4.0报告](report/devProgress/recording_ux_20261022.md)。

PiP 可在源窗口获得焦点时隐藏，并在稳定隐藏后回收采集会话；设置和右键可调整策略。
Pin/PiP 支持可选阴影与圆角，初始关闭，效果不进入复制、保存或 OCR 的图像像素。
这两项已有自动回归测试，低资源机器、多屏和真实监控恢复延迟仍待实机验收。

## 架构与资源生命周期

- `src/main.cpp` 绑定宿主服务、设置和窗口/采集生命周期。
- `src/core/` 包含设置、插件注册/安装/更新、图片区段模型及监控可见性策略。
- `src/capture/` 管理 D3D11/WGC，会话关闭通过回调门禁排空在途操作。
- `src/ui/` 提供选择、Pin/PiP、共享编辑器、标注、设置和窗口视觉适配。
- `sdk/nskry_plugin.h` 是 API 2 的公开 ABI，内部 C++ 类型不是插件接口。
- `plugins/` 是插件源码；已安装包和配置在 `%LOCALAPPDATA%/Nskry/`。
- `tests/` 保存纯逻辑、插件运行时、UI、GPU 帧和 OCR 回归测试。

启动读取 Registry/manifest，不通过扫描 DLL 执行信息函数来发现插件。
`installed != loaded`：普通插件按需加载；已加载 DLL 的更新、卸载通过重启应用。
设置窗口按需创建，关闭即释放。更新检查由用户点击触发，官网地址尚未配置时明确提示。
`standalone` 命令界面、跨插件依赖解析和服务协商仍是后续协议，不能当作现有能力使用。
已加载任务的显式控制命令和独立区域采集使用 [可选 v1 扩展](plugins/recording_extensions_v1.md)，不占 API 2 reserved。

实时监控的采集/裁剪/呈现主要在 GPU 上完成；OCR、静态帧导出和部分标注取样会读回像素。
不对整个程序宣称“零拷贝”或固定 RAM 上限。旧原型的约 135 KB/20 MB 数据不代表当前版本。
区段编辑历史保存元数据，导出时物化图像；阴影缓存不随每帧采集重新生成。

## OCR

默认精确模式启动一次性 PP-OCRv4 CPU worker，任务完成、取消或关闭时退出。
插件本体不常驻推理模型。Windows OCR 保留为 Fast 选项，最多三轮、按质量条件追加增强。
两种引擎不合并结果，不做词典或语义强制纠错。显示、复制和编辑始终使用原图，识别框映射回原图。

结果页提供原图/文本、文本选择、缩放、平移、旋转重识别和 Core 编辑入口。
监控入口按用户操作截取当前一帧；长图可在共享编辑器选择区域识别。
低配 1–2 秒目标及所有漏字场景尚未验收；历史实测见 [原生 worker 报告](report/devProgress/ocr_native_worker_implementation_report.md)。

## 构建与验证

从项目根目录按 [README 构建说明](../README.md#构建) 准备依赖并构建 Release。
Windows-only OCR 可使用 `-DNSKRY_OCR_MODEL_ENGINE=OFF`。完整 OCR 包包含 engine/models，不能只复制 DLL。
插件包生成到 `installer/bundled/`，随后可按 [安装器说明](../installer/README.md) 构建 Setup。

```powershell
ctest --test-dir build --output-on-failure
```

`build/verify/` 是独立验证构建目录，不是安装目录。发布程序和测试程序必须来自同一套已验证源码。
运行时资源目标与实机验收方法见 [资源优先方案](Proposal/resource_first_features_20261011.md)。

## 维护约束

图像句柄所有权、线程回调、structSize/version/reserved、按需加载和重启更新契约必须保留。
新增插件优先使用宿主已公开服务，不依赖其它插件私有窗口或 DLL；前置服务须职责独立、显式协商。
插件开发以 [Core 契约](architecture/core_plugin_contract.md) 和 [交付规范](plugins/development_standard.md) 为准。

渲染回归需关注 Flip Model 与浮动工具栏层级、透明标注的 alpha、移动消息对文字输入框的影响、
隐藏/最小化门禁及源尺寸变化。不能只因代码历史类名为 `LongImageCropWindow` 就把编辑器归入 Scroll。

Core 使用 GPL-3.0-or-later，并附 [独立开源插件例外](../PLUGIN_EXCEPTION)。第三方插件依其开发者许可，
正式范围见 [许可说明](governance/plugin_license_exception.md)；官方插件身份与第三方风险见 [政策](plugins/third_party_policy.md)。
发行对应源码、第三方依赖/模型授权、签名和官方目录上线均有独立审核要求。

## 后续工作与历史设计

Stage A–G、长截图、OCR 和本轮 Core 编辑/监控/视觉首版已实现；不代表所有实机性能门禁已通过。
翻译、录屏音频/GIF/隔离与更广设备验收、API 3 多动作/服务协商、官方签名与撤销、持久化事务仍待开发。

[原规划](implementation_plan.md) 与 [二次规划](nskry_distribution_plugin_plan.md) 保留设计演进，
旧 SDK、目录和原型示例不作为当前交付要求。实测报告按日期保留，不把历史数字改写成新版本结论。
原始调试截图和本机快捷方式保留在忽略目录，不进入公开提交。
