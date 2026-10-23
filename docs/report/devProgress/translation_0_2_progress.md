# Translation 0.2.0 / OCR 0.6.3 交互与联动

2026-10-24。实现本轮两条流程，不把开发包称作正式发行完成。

## 完成项

1. 框选后点击 Translation：复制图像 → 按需 Windows OCR → 判断中英方向 → 自动启动离线翻译。
   不再要求二次点击。每次新图默认 Auto；仍可编辑原文、手动覆盖方向、再次翻译。
2. OCR 结果页右侧文本区底部有 Translate。将已有识别文字复制交给宿主，由宿主按需调度翻译提供者。
   不再次截图/识别，不加载另一个插件的私有 DLL/模型，不绑定安装前置。关闭 OCR 后翻译独立继续。
3. 翻译缺失/禁用/不兼容时入口禁用；激活 OCR 结果页时刷新 metadata，无常驻检查。
   忙时明确拒绝，取消/关闭不会在任务完成后意外自动开始下一阶段；结果不保留为下一次请求的旧译文。
4. 默认 Auto 采用固定开销的文字脚本判断，无额外检测模型或联网。
   支持当前语言包的中文→英文、英文→中文；混合文字按受支持脚本占比选择。
   纯数字/无明显语言信号、日文假名/韩文/西里尔等明显不支持的脚本会提示检查/手动选方向。
   **不是通用语言检测**：其他纯拉丁字母语言可能误判为英语，纯汉字日文也可能误判；未声称支持全部语言。

## 接口与兼容

新增 sdk/nskry_text_translation.h，可选、版本化的宿主文字翻译交接 v1：
text_translation 提供者 / text_translation_client 调用方。API 2 布局与 reserved 不变。
metadata 发现不加载 DLL；用户操作才选择/加载提供者。多提供者由宿主菜单选择，不按 ID/来源猜测功能。
不实现通用服务依赖，不放宽 requires_plugins 限制。说明见 docs/plugins/text_translation_extension_v1.md。

仅宿主 UI 线程可用；后台/退出期间拒绝。数据有长度/UTF-16 上限，返回前复制，防交接重入；
交付前重查 Registry。明确区分 ACCEPTED（只是接受异步任务）、BUSY、INVALID、UNAVAILABLE、FAILED、CANCELED。
缺导出、未知版本、异常不会伪装成功。调用方不持有提供者私有地址，提供者不持有 OCR 结果生命周期。

联动需要本轮 Core + OCR 0.6.3 + Translation 0.2.0；旧 Core/旧插件仍能使用原有独立功能，但不具备这条联动。
已有离线语言包、HKCU ModelRoot 不变，无需重新下载/转换。
BYOK/免费在线/较大离线引擎仍是后续范围，没有新增在线请求或暗中 fallback。

## 资源与验证

不增加常驻任务/线程/模型；文本判断为一次线性扫描，交接可避免重复 OCR。
保留单任务、worker 退出、Job 1 GiB 私有提交上限、DPAPI 临时数据保护和阶段超时。
本轮没有重新测完整资源峰值或低配硬件，不能由少跑一次 OCR 推断具体节省百分比。

build/verify/Nskry.exe 正在运行（只读确认），未停止它，也未覆盖该 EXE。
独立 build/verify-translation 完整 MSVC Release 构建通过；首次新目录编译器检测受 PDB 通信限制，
改为正常工具通信和 Release 检测后构建成功，不是源代码编译失败。

- 最终完整 CTest 37/37 通过，45.89 秒（包含缺导出和取消续跑回归）；方向提示微调后另有 6/6 定向复核通过，5.34 秒。
- Python worker 纯逻辑 3/3 通过。
- 新纯测试覆盖中英/混排、数字、不支持脚本、UTF-16/长度及错误缓冲边界。
- 新 runtime fixture：无提供者、metadata 不加载、UI 线程约束、版本、缺导出、忙、异常/非法返回、禁用/兼容/退出。
- 真实自动翻译：框选图后自动翻译、中→英直接文字交接、借用文本立即修改不影响结果、忙保护、取消识别后不自动续跑。
- 真实 OCR→Core→Translation：禁用入口、重新启用、原文完全一致、没有第二次 OCR 进度窗、关闭 OCR 后翻译完成。
- 首轮集成测试将 OCR 合成图的 I/l 拼写混淆当失败；已改为检验稳定片段及完整原文一致性，不修改识别输出或译文。

仍需人工验收：用户实际安装更新后的全流程、多翻译提供者菜单、复杂混排/其他拉丁语言、
多屏高 DPI、低配/内存压力。正式分发的依赖许可审计仍未完成，沿用 DEVELOPMENT_ONLY 限制。

## 试用产物（未安装）

- Core：build/verify-translation/Nskry.exe。
- OCR：installer/bundled/nskry-ocr.nskryplugin（0.6.3，完整引擎包）。
- 翻译：build/translate-local/nskry-translate-development-0.2.0-final.nskryplugin。
- 语言包根目录仍为 build/translate-languages。

用户须自行退出旧进程，再使用新 Core 安装/暂存两个包并重启。不会自动替换运行中的 DLL、自动安装或 push。
