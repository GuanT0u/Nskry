# 独立开源插件例外 1.0 实施记录

> 公开归档说明（2026-10-12）：以下记录对应各自实施日期；当时的测试数量、构建问题和发布状态不代表当前状态。最新功能概况见 [维护指南](../../HANDOFF.md)。

日期：2026-10-11。

维护者确认相关授权后，准备正式 PLUGIN_EXCEPTION，并在 LICENSE 与公开 SDK 引用。
适用独立开源插件，不要求统一 GPL；闭源不在本次例外范围。Core 及其修改仍遵守 GPL。
SDK 的声明/宏/内联/示例复制有明确许可；例外不豁免 Core 实现复制、其它权利人代码或模型许可。
COPYING 未修改；没有冒用注册 SPDX 例外 ID。正式文本是自定义附加许可，公开发行前建议专业复核。

同步 README、开发/第三方政策、发行治理文档与 HANDOFF；许可说明使用正式文件名 `plugin_license_exception.md`。
安装器与 OCR/Scroll 打包均附 LICENSE/COPYING/PLUGIN_EXCEPTION，依赖列表含例外文件，变更会重新打包。
官方插件本身不因附带例外被重新许可。旧发行物不自动获得例外；后续贡献与下游 fork 须核对授权范围。

验证：

- build/verify 完整 Release 构建通过；12 项 CTest 全通过。
- 两个 bundled 插件包的三个许可文件逐项 SHA-256 与仓库根文件一致。
- Inno Setup 编译通过，测试产物在 build/verify/installer-validation/NskrySetup.exe，未运行安装器。
- git diff --check 通过；COPYING 相对原暂存内容无差异。
- 未改 API 2 ABI/运行时功能，未安装到用户目录，未提交或推送，

正式许可准备及仓库声明不等于已完成对外发行、完整贡献台账独立核查或法律专业审核。
