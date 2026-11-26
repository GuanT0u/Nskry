# Translate 0.3.4 在线公测候选验收

2026-11-26 · 内部候选包 RC2

## 首发范围

Windows x64 上进行中英双向翻译。在线包自带本地 OCR，首次打开等待用户选择在线引擎。截图可以先识别，用户可在发送前检查和编辑原文。MyMemory 无需配置，每次发送都需确认。OpenAI 兼容和 LibreTranslate 标为实验支持，服务由用户配置。官方插件目录和网站继续标为开发状态。

## 候选包

- 文件为 `nskry-translate-0.3.4-online-rc2.nskryplugin`，共 21,749,347 字节、24 个归档条目。压缩包与逐文件 SHA-256 位于本地 `build/translate-dist/`。
- 包 SHA-256 为 `cf0bc6c7e1c51f6424d7f69a4ecad05f2c375bd39700e28bd0bfec54b5fa79ba`。
- 使用 `plugins/nskry-translate/tools/package_development.py` 的 `--online-only` 和启用本地图像模型的 x64 Release 构建。归档校验覆盖重复文件名、完整性、版本和两份模型哈希。包内含本地 OCR worker、ONNX Runtime、检测和识别权重、项目许可证及 OCR 第三方声明。
- 两个权重哈希均与 RapidOCR v3.10.0 ONNX Runtime 登记中的中英文 PP-OCRv4 mobile 模型一致。随包声明记录 PaddleOCR 权利归属、RapidOCR 的 ONNX 转换和原始 wheel 来源。[版本化模型登记](https://github.com/RapidAI/RapidOCR/blob/v3.10.0/python/rapidocr/default_models.yaml) · [RapidOCR 模型许可](https://github.com/RapidAI/RapidOCR/blob/main/python/MODEL_LICENSES.md)
- 对已安装产物运行 `dumpbin /dependents`。插件、OCR worker 和 ONNX Runtime 的直接依赖包括 Windows 系统库和 MSVC 运行库。干净机器使用匹配构建工具版本的 x64 VC++ v14 Redistributable，需覆盖 `MSVCP140_1.dll`、`CONCRT140.dll` 等依赖。[微软运行库说明](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist/)

## 已验证

- 真实 Nskry 安装器可在隔离的本机数据目录安装候选包，并将从 0.3.3 到 0.3.4 的更新排入重启队列。同一目录通过 Nskry 启动时调用的包管理代码验证了更新应用、禁用、重新启用和卸载。
- 从安装目录加载候选 DLL，验证新配置、沿用离线偏好、截图先本机识别、OCR 原文交接、拒绝上传、保存在线偏好和重新打开。测试程序命令为 `NskryTranslationPluginTests.exe <绝对DLL路径> --online-candidate`。追加 `--live-free` 仅发送预置的两个公开测试句。
- MyMemory 英译中与中译英均从候选包实际返回译文，测试核对了「世界」或 `world`。两次请求分别经过确认。OpenAI 兼容和 LibreTranslate 的本轮验收范围是配置、协议与传输回归。
- 相关 Windows 回归 14 项、网站数据测试 10 项通过。

## 公开上架前

1. 在干净的 Windows x64 环境验证最低支持版本、VC++ 运行库、插件管理器安装、两个重启选项、升级实际生效、禁用和卸载。完成「框选 → 翻译 → 复制／保存」及高 DPI、中英文界面检查。
2. 用聊天、混排、数字地址和多栏样本抽检识别与翻译，分别记录原图、识别原文和译文中的错误。
3. 指向候选包对应的源码快照和构建材料，确认分发渠道后再配置下载资产。官方更新目录沿用项目已有的目录上线流程。
4. 离线发行另行验证冻结运行时依赖、模型语言包交付及无 Python、断网安装场景。
