# 插件边界、代码审查与优化报告

> 公开归档说明（2026-10-12）：以下记录对应各自实施日期；当时的测试数量、构建问题和发布状态不代表当前状态。最新功能概况见 [维护指南](../../HANDOFF.md)。

日期：2026-10-08。范围：当前 Core/SDK、Pin/PiP/共享编辑器、插件管理/更新、Settings、许可打包。
审查覆盖调用链、架构边界、资源生命周期、更新策略及公开交付要求。
这不是“全仓库无漏洞”认证；OCR 算法及图像采集引擎未在此轮重写。

## 1. 已确定的边界

基础 Nskry：框选、固定、监控、窗口管理、通用裁剪/标注、复制/保存、Settings 和插件基础设施。
可选插件：滚动采集/拼接、OCR 推理/结果 UI，以及后续翻译、录屏等专项能力。
共享编辑器已经在宿主编译，OCR Edit **不依赖 Scroll DLL**；历史类名不代表插件前置。
没有必要再拆一个人人必须安装的“编辑器插件”。

宿主负责悬浮布局和基础按钮；API 2 通过 manifest capabilities + execute 声明动作。
本轮通用 `…` 按钮取代按 OCR ID/英文标签匹配：一个动作直接调用，多个菜单选择，零个隐藏。
自定义位置、多个命令、图标/优先级等需要后续版本化 descriptor，而非插件修改宿主 HWND。
规范详见 [Core 契约](../../architecture/core_plugin_contract.md)。

## 2. 严重性 / 必要性排序与本轮处理

| 级别 | 问题 | 必要性与处理 |
| --- | --- | --- |
| P1 / 必须 | 已加载缓存绕过 enabled，旧窗口快照还能调用禁用插件 | 已修复：先查启用/兼容，再查 DLL 缓存；不做热卸载。独立 fixture 验证加载→禁用→重启用 |
| P1 / 必须 | init 失败后直接释放 DLL，未调用部分初始化清理 | 进入 init 后失败会先调用 shutdown；fixture 验证失败清理计数与最终物理卸载。恶意/不合规插件仍不是沙箱可控对象 |
| P1 / 必须 | 任意第三方 feed 可返回其他安装 ID；官方同时读自身 feed | 已修复：官方 catalog 只更新官方来源记录；非官方 feed 只匹配自己的 ID，不改变来源等级 |
| P1 / 必须 | 下载包未绑定选中 update 的 ID/version | 已修复：hash 后 Inspect，再比较 ID/version/兼容性，最后统一 PackageManager 安装/暂存 |
| P1 / 发布必须 | 只有 GPL 链接，无完整 COPYING / 安装器许可文件 | 已补完整许可；Core Setup 和两个自有插件包带 LICENSE/COPYING；源码、准确模型权重授权仍是发布审核项 |
| P2 / 必须 | 网络响应无限扩张、无显式超时，异常时裸句柄可能漏 | 文本 2 MiB / 包 128 MiB、阶段 timeout、120 s 接收循环期限、禁止 HTTPS 降级重定向、WinHTTP RAII；不是严格总耗时硬实时保证 |
| P2 / 必须 | 监控右键图像动作与快捷入口的 bitmap 选入规则不一致 | 两条路径统一至 RunImageAction，调用 GetDIBits 类插件前解除选入 DC |
| P2 / 必须 | 禁用状态保存失败但内存已修改，UI不提示 | SetEnabled 失败回滚内存状态，UI报告失败；不宣称完成了全部配置事务/断电恢复 |
| P2 / 需要 | 第三方风险说明只有一句，用户无长期查看入口 | 规范文档、插件页政策按钮、安装确认补齐 native 权限/责任/身份边界，默认取消 |
| P2 / 需要 | 插件页不显示介绍，空 feed 却说全部最新 | 新详情区与选中刷新；保留选择；显示未配置/未检查警告，存在部分成功也呈现错误 |
| P2 / 需要 | 官方浏览入口缺失 | 增加按钮与可配置 HTTPS 浏览 URL，未上线明确提示不联网，不伪造网址 |
| P3 / 整理 | 共享窗口称 Long Screenshot、非 OCR 作者无法使用 shortcut | 标题改为 Image editor / Crop；元数据驱动通用动作；保留历史 private 类名避免无意义大规模改名 |
| P3 / 可删 | 未使用 WM_SETTINGS_CLOSED 常量与过时注释 | 删除未发送的死常量，更新 SDK/service 注释；不删除 reserved/standalone/staging 等规划能力 |
| P3 / 工程 | Settings header 依赖 PCH 才有 once_flag | 加显式 mutex include，新独立宿主测试可正常包含此头 |

来源是本地 registry 的 source_type，不是签名验证。“Official source / identity not verified”明确显示。
从普通 Browse 导入仍视作第三方，尚未实现由下载包签名自动识别认证发布者。
官方身份须符合维护者/认证收编定义；不能靠 manifest author/name 自称证明。

## 3. 结构是否偏轨

总体未偏离 Stage A–G：Runtime、Registry、Package、Update、Settings service 分开；
DLL 按需加载，Settings 不常驻，更新/卸载通过重启；源码/安装目录不是同一个布局。
Runtime 提供轻量动作枚举是合理 facade，不应为了形式新增重复 Manager。

需要纠正的是 UI 与某个插件 ID/译文绑定、把共享 Core 编辑器误认为插件界面、
来源记录与认证概念混用、更新信任作用域不足，本轮已明确/修复上述边界。
`main.cpp` 与若干 UI 类仍较大，但本轮不拆成另一组大重写：服务契约/回归先行，拆分可另做。

## 4. 保留 / 可有可无 / 重新规划

- 必须保留：structSize/version/reserved、主线程回调和位图所有权、on_demand/startup、
  post_capture/toolbar_action、metadata 读取、package 路径检查、staging/rollback、Settings service、模型 trace/测试。
- 保留但不声称已实现：standalone command surface、自动检查配置预留、provider 扩展、双进程 Settings；
  用版本化实现补齐，不按死代码删除，也不为它们现在启动 worker/联网。
- 可删的仅有确定无调用/无契约的孤立常量；构建缓存、DebugSamples、用户文档/快捷方式未清理。
- 重规划：任意 HWND 注入、跨插件猜 DLL/窗口类、依赖另一个插件的私有 UI 不纳入公开规范；
  API 2 不支持跨插件 resolver，声明非空 requires_plugins 明确拒绝。
- 相同基础 UI 不拆强制前置插件；轻算法可静态/包内私有库复用；重资源独立按需引擎，不并入常驻 Core。

## 5. 新规范与 GPL 防冒充

- [插件开发标准](../../plugins/development_standard.md)：交付格式、宿主服务、依赖/所有权/线程、许可/模型、最低安装集和发布测试。
- [第三方政策](../../plugins/third_party_policy.md)：官方定义、未沙箱 native 风险、法定权利保留、更新隐私、举报/撤销规划。
- [发行与品牌保护](../../governance/distribution_and_brand_protection.md)：GPL 合规、签名、官网免费渠道、品牌/平台举报与法务门禁。

GPL 允许符合条件的收费分发，不能加“禁止转售”却仍称 GPL；历史 MIT 授权不能追溯收回。
本轮不加帐号/激活/DRM/远程 kill、不更改 GPL 授予的运行/修改权。
许可证、签名和商标权是不同层次；签名/商标登记/权重许可不能通过写一篇规划文档视为完成。

## 6. 官网配置与未来阶段

当前官网未上线，Settings → Plugins 的入口可见、点击显示未配置。参数保持空值：

```json
"updates": {
  "official_plugin_catalog_url": "",
  "official_plugins_page_url": ""
}
```

位于 `%LOCALAPPDATA%/Nskry/config/settings.json`，新增字段兼容旧配置且 Save 保留。
浏览 URL 与 JSON feed 是两件事；以后由维护者配置真实 HTTPS 端点。

| 优先级 / 阶段 | 后续工作 | 验收条件 |
| --- | --- | --- |
| P1 / 官网发布前 | 官方认证、catalog/package 签名、key rotation/revocation；来源状态迁移 | 认证绑定 ID/发布者/版本/hash，不信 manifest 自述；错误/过期/撤销签名测试 |
| P1 / 发行前 | 对应源码、贡献/旧 MIT 台账、准确模型权重授权、品牌权利审核 | 精确版本源码/脚本/依赖许可可获取，专业法律审核必要条款 |
| P1 / 认证迁移前 | 本地替换官方 ID 的 provenance 与 pending source 记录 | 手动覆盖不能继续当密码学认证；现 source 只是历史安装来源、不提供身份保证 |
| P2 / 更新服务完善 | 异步检查/下载、取消、流式文件与请求内 feed 去重 | UI 可关闭/监控继续运行，快照/结果 dispatch 无 registry 数据竞争，任务后释放 |
| P2 / 持久化完善 | Settings/Registry/pending JSON 原子写与回滚事务 | 模拟权限失败、断电、文件损坏，不丢其他插件状态；不只内存回滚 |
| P2 / 共享 UI | 按钮/toolbars 公共组件、窄 Pin/PiP 窗口溢出与 DPI | 不复制功能、不依赖插件，保留 tiny capture 原大小及可访问操作 |
| P2 / API 3 | 多动作 descriptor、command/service query、能力协商 | API 2 不变，清楚区分 required/optional 服务、错误和所有权 |
| P3 / 实际需要时 | 显式跨插件依赖/可替换提供者 | 版本/循环/缺失/禁用卸载冲突均有管理；不通过搜其他目录实现 |

更新 I/O 本轮仍是同步 service 调用；增加 bounds 不等于 UI 已异步，极慢网络仍可能短期阻塞设置和主消息循环。
网站认证、真实远程 feed 和签名验证未上线，不把当前部署称为已认证生态。

## 7. 验证与交付边界

- Release CMake 构建通过；全部 12 项 CTest 通过，6 项评测工具 Python 测试通过，git diff --check 通过。
- 纯逻辑覆盖第三方跨 ID/官方越权、catalog 分类、重复更新、降级、HTTP/坏 hash、错误包 ID/version/API、manifest 前置与 provider 拒绝。
- 宿主 UI 覆盖描述/未验证来源、默认/放大窗口布局、无打开页面网络、关闭销毁；
  零插件/零 runtime 的 Core editor 可打开、原尺寸复制、无插件占位。
- Runtime fixture 验证 lazy load、init 失败 shutdown 清理、已加载禁用后拒绝 execute、重启用、保存失败回滚与最终 DLL 卸载；数据只在 build 内，未写用户插件目录。
- 两个 `.nskryplugin` 包确认含 LICENSE/COPYING，OCR 仍含完整 engine/models/第三方声明。
- Inno Setup 使用 `/DCoreExePath=.../build/verify/Nskry.exe` 编译验证，产物在 `build/verify/installer-validation/`。
  只编译，没有运行安装器、真实安装/卸载、真实官网检查或实际网络故障测试。

未改变用户 AppData 插件、未部署官网、未提交/推送。原始调试样本和本机快捷方式按忽略规则留在本地。
本轮不是完全安全审计证明；遗留 P1/P2 按上表列出，不把未来方案当完成项。
