# 插件开发与交付规范（API 2）

快捷键、默认键和 General 跳转入口须遵循 [输入扩展 v1/v2](shortcuts_and_input_extension.md)。
不得只展示可配置快捷键而未实现配置导出，亦不得占用 API 2 reserved 字段。
已加载任务的全局控制与连续区域采集另见 [可选命令/采集 v1](recording_extensions_v1.md)；
不是 screenshot context 的帧流，也不是已实现 standalone 或跨插件服务。
区域/窗口跟随采集v2、额外capture动作与设置UI见 [录屏扩展v2](recording_extensions_v2.md)。
用户主动发起的 OCR→翻译交接见 [文字翻译扩展v1](text_translation_extension_v1.md)；
它不是任意跨插件服务或前置依赖解析，不允许直接调用另一个包的私有 DLL。

适用官方及独立第三方作者。先阅读 [基础能力契约](../architecture/core_plugin_contract.md)
与 [第三方安全政策](third_party_policy.md)。官方身份不改变 SDK 约束。

## 1. 最小交付与元数据

一个 `.nskryplugin` ZIP 是一个独立根目录：manifest.json、entry DLL、私有依赖/模型、许可/声明。
禁止入口前置嵌套一层目录、路径穿越、绝对路径、reparse/symlink。安装/更新/Setup 共用 PackageManager。
不得要求用户从另一个插件目录拷贝 DLL。源码位于 `plugins/<id>/`，不把算法塞到 `main.cpp`。

```json
{
  "manifest_version": 1,
  "id": "org.example.image-action",
  "name": "Image action",
  "version": "1.0.0",
  "author": "Example developer",
  "description": "A brief description of what the action does",
  "runtime": {"entry": "image-action.dll", "architecture": "x64", "api_version": 2, "load_policy": "on_demand"},
  "capabilities": ["toolbar_action", "post_capture"],
  "compatibility": {"nskry_min": "0.5.0", "nskry_max": null},
  "source": {"homepage": "https://publisher.example/plugin", "repository": "https://publisher.example/source"},
  "update": {"type": "manifest", "url": "https://publisher.example/plugin/update.json"}
}
```

`.example` 是文档占位，不能复制为生产地址。ID/version/runtime 必须匹配 DLL 的 PluginInfo。
metadata 在加载 DLL 前读取，不能为取描述、按钮或更新 URL 执行插件。
ID 使用可归属的命名空间；`nskry-*` 名称不自动证明官方身份，不在 manifest 自称 trust/official 来取得认证。

## 2. 必须使用宿主能力的地方

- 图像 Crop/Edit 调 `openBitmapEditor`，不调 nskry-scroll 的窗口或导出函数。
- 复制图片调宿主 copy callback，检查返回值；需要后台识别先复制借用图。
- 按钮通过 capabilities 提供宿主动作，绝不按 HWND/class/name 搜索并修改宿主控件。
- post_capture 必须支持静态截图和 PiP 的单帧，不能假设每次都有原窗口滚动上下文。
- 没有图像/host callback 时清楚提示不可用，不能崩溃或静默成功。
- 长图优先由用户选定区域再识别/翻译，不直接无界处理整张长图。

允许开发独立、边界清晰的前置插件，不允许把小服务绑在用户不需要的大功能上。
当前 API 2 没有跨插件依赖管理。禁止隐式依赖，非空 `requires_plugins` 会拒绝；这是版本限制，不是永久禁止前置。
共享轻算法优先私有静态库，通用 UI 优先 Core，重引擎按需 worker。
未来提供跨插件服务须升级 SDK/manifest 协议，不能直接使用 reserved 字段或猜测目录。

## 3. 生命周期与资源

普通插件 on_demand；startup 只用于用户明确开启的后台任务，并说明理由和稳态预算。
init 不应预热 OCR/录屏模型；execute 启动任务，完成/失败/取消/替代/shutdown 均释放临时资源。
idle 与任务峰值分别测量。模型工作集、提交量、CPU 累计时间、端到端延迟必须分开报告。
限制输入/输出/线程/内存/超时，避免无限任务队列；短峰值不代表允许持续占用。
异步任务不得直接调用 UI callback；保存复制的值而不是 execute context 指针。
shutdown 后不能有仍执行 DLL 代码的线程、窗口或注册回调；当前不承诺热卸载/热更新。
init 返回失败或抛出 C++ 异常后，宿主会在释放 DLL 前调用 shutdown；cleanup 必须支持部分初始化。

## 4. 更新来源与官方认证

官方：项目维护者开发，或被维护者明确认证收编的个人/团队开发；不是“用了 nskry 前缀”或“作者填 Nskry”。
第三方：独立开发者/团队自行发布，作者对内容和更新服务负责。
当前 registry 的 source_type 记录本地安装来源，不是签名认证；Settings 会明确 identity not verified。
从本地 Browse 导入即按第三方/未验证处理，官网未上线不伪造 URL 或认证。

官方已安装记录只读取配置的官方 catalog；非官方只读取自己的 update.url，且只接受自身 ID。
feed 不得更新其他插件、升级自己的信任等级或替换官方身份。无 feed 不代表已检查到最新。
提供 HTTPS URL、SemVer、包 SHA-256、发布说明和兼容范围；下载包再次校验 ID/version/host compatibility。

```json
{
  "id": "org.example.image-action",
  "latest_version": "1.1.0",
  "package_url": "https://publisher.example/plugin-1.1.0.nskryplugin",
  "sha256": "REPLACE_WITH_64_HEX_CHARACTERS",
  "min_nskry": "0.5.0",
  "release_notes": "Describe the changes"
}
```

示例 SHA 是占位不是有效值。官方 catalog 使用 `{"schema":1,"plugins":[上述对象...]}`，
由维护者认证流程决定哪些 ID/publisher 被收编；第三方 feed 即使返回数组也只能更新自身 ID。
当前文本下载上限 2 MiB，包下载上限 128 MiB；超大模型另行评估，不能默认提高所有插件限额。
包验证额外限制 central directory 为 16 MiB、4096 项、声明解压总量 512 MiB；ZIP64 不支持。
检查/下载/验证/解压使用按需可取消任务和元数据快照，禁止 worker 访问活 Registry、UI 或插件生命周期。
主线程交付前再次核对 ID、版本和来源；持久化暂存目录不是任务临时资源，仍在重启时应用。
下载 hash 只校验内容一致性，不证明签名/作者身份。数字签名与吊销是后续发行前置工作。

## 5. 许可与隐私

保留依赖及模型来源、版本、hash、原许可声明。打包二进制时提供适用许可证及对应源码/构建资料。
项目政策：第三方开发者自行选择插件许可证，Nskry 不要求其统一采用 GPL，也不替作者决定许可。
Core 仍为 GPL-3.0-or-later；2026-10-11 加入
[独立开源插件例外 1.0](../../PLUGIN_EXCEPTION)，允许仅通过公开 SDK 结合的独立开源插件自选开源许可。
公开 SDK 声明/宏/内联/示例的必要复制与适配在例外内；分发 SDK 源码时保留许可与例外声明。
例外不覆盖闭源插件，不豁免复制 Core 代码、第三方 GPL 依赖或模型许可义务。
官方插件仍按其声明的 GPL 发布，不因附带宿主例外自动改许可；下游 fork 可能移除例外，使用前核对实际发行声明。
见 [授权与范围说明](../governance/plugin_license_exception.md)。
截图/识别文字日志默认关闭，说明任何联网、上传、存储和删除行为；禁止为了宣传离线而暗中调用在线服务。

## 6. 发布门禁

1. 最小 Core + 本插件 + 明确声明的最小前置集合测试；无前置插件须独立运行。
   宿主基础编辑能力不需要 OCR/Scroll。未来前置须覆盖缺失/不兼容/循环/禁用/卸载/更新冲突测试。
2. 插件未安装、已禁用、未加载、已加载、升级暂存、卸载暂存分别验证。
3. callback 所有权、取消、关闭、异常、输入上限、线程及资源回收测试。
4. 图片路径/安装路径含中文/空格、缺依赖、损坏包、错误 ID/version/API、哈希失败测试。
5. 新逻辑纯单元测试 + UI/workflow smoke + 无启动网络/后台模型加载检查。
6. 源码、build 脚本、第三方声明、模型准确授权、release notes 和支持渠道齐备。
7. 官方身份须维护者批准；签名/官网尚未上线时只能标明开发包/来源未验证。

禁止用识别测试期望词硬编码补字，禁止宣称两张图通过等于准确率 100%，禁止把保留接口当死代码删除。

## 7. Core 与输入代码的实现约束

- 行为路由使用显式 command/context/capability，不能通过窗口类名、英文标题或插件 ID 猜测能力。
  未安装产品的预留 launch 项可以列入宿主目录，但不能据此给已安装包提升来源身份。
- 局部快捷键由插件 manifest 声明，Core 不复制 OCR/Scroll 的局部按键表。全局注册属于宿主。
- 可扩展列表不能将行号无限编码进 Win32 的 16-bit 控件 ID；使用发送控件与完整索引，并核对归属。
- 高频消息不得反复解析同一快捷键、复制整份设置或加载光标文件；缓存按配置变化更新。
  临时提示只在需要时使用定时器；隐藏/关闭页面应终止操作并释放自身资源。
- 设置保存只覆盖本版本已实现字段，保留未知/预留字段；解析失败的原文件不能静默覆盖成默认值。
- 失败必须区分“设置已保存但插件未接受配置”和“保存失败/回滚也失败”，不能静默成功。
- 修改过的小模块保持可读的多行分支与一致缩进；不为格式统一重写整份 OCR 或采集窗口。
  删除前确认没有调用且不是公开契约/规划项；替代旧实现时保留需要的接口并收敛到统一实现。
- 新增可独立核对的逻辑须有纯测试；边界修复补运行时/UI 消息回归，不以自动测试冒充实机体验验收。
