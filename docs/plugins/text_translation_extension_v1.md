# 文字翻译交接扩展 v1（2026-10-24）

范围：用户在 OCR 等结果页主动点击 Translate，由宿主把已有文字交给一个可选翻译提供者。
不是通用跨插件服务查询、模型访问、后台自动调用、前置依赖管理或 API 3 的替代品。
不修改 API 2 的结构布局/reserved；旧插件与现有图像动作仍可用。

头文件：`sdk/nskry_text_translation.h`。

## 提供者

manifest 声明 `"extensions":{"text_translation":1}`，导出
`int32_t nskry_plugin_translate_text_v1(const NskryTextTranslationV1*, NskryTranslationResultV1*)`。
只负责接收文字、展示自身 UI，并启动异步任务。不能阻塞 UI 等推理/网络结果。
接收后必须复制原文，不保留请求、result/error 缓冲、owner 窗口或调用方数据的借用生命周期。
翻译窗口不应依赖 OCR 窗口的生命周期；关闭 OCR 不应取消已被翻译插件接受的任务。

## 调用方

manifest 声明 `"extensions":{"text_translation_client":1}`，导出
`int32_t nskry_plugin_connect_text_translation_v1(const NskryTextTranslationServiceV1*)`。
加载时复制服务表；connect 不预热、不展示 UI、不执行翻译。拒绝未知版本/过短结构/缺回调。
宿主支持接口但未安装提供者时连接仍成功，available 返回 0。
不得将调用方或提供者声明为对方的 requires_plugins 前置。

服务表仅在宿主 UI 线程使用，直到插件 shutdown；不能从 OCR/模型 worker 调用。
available 只读 metadata，不加载 DLL/模型、不联网；不能在鼠标移动或常驻定时器中反复查询。
OCR 在创建/激活结果页时刷新入口；点击时宿主重新验证启用、兼容性和扩展版本。
无可用提供者时禁用入口，不影响识别、编辑、复制或保存。

## 调用契约

- 请求 structSize/version=1，text 为 UTF-16，textLength=1..16000，不要求 NUL 终止，拒绝内嵌 NUL、损坏代理项和纯空白。
- owner 可为空，只是 UI 锚点；不转移窗口/数据所有权。
- result 可为空；非空时 structSize/version=1。errorCapacity 包含终止 NUL。
- ACCEPTED=1 仅表示异步任务已接受，**不代表翻译成功或已完成**。最终进度/错误由提供者 UI 展示。
- UNAVAILABLE=0、FAILED=-1、BUSY=-2、INVALID=-3、CANCELED=-4。错误不得泄露原文/凭证。
- 无队列；忙时可拒绝并提示用户取消/等待，不能静默覆盖正在处理的文本。
- 单提供者直接按需加载；多个提供者由宿主菜单让用户选（最多展示 100 个），不按 ID、官方身份或标签猜测功能。
- 选择菜单可能泵消息；宿主复制原文并防重入，交付前重查 Registry。调用方也需复制已有结果，不能在返回后继续访问可能已关闭的窗口对象。
- 宿主拒绝后台线程调用、未知版本、缺声明/导出、已禁用/不兼容提供者及退出期间的新请求，捕获提供者 C++ 异常。
- 提供者自己负责取消、超时、进程/线程和窗口清理；宿主不维护模型池或额外任务管理器。

## 兼容与隐私

旧 Core 不认识本扩展时 OCR 没有服务表，Translate 显示不可用，原有 OCR 功能保持。
新 Core + 旧 OCR 没有联动按钮；新 Core + 旧翻译包没有文本声明，不作为交接提供者。
使用联动须更新同轮 Core、OCR 0.6.3+、Translation 0.2.0+；语言包无需重下。
扩展版本不代表官方认证、网络上传授权或通用依赖管理。未来在线提供者须自己展示上传/授权确认；
不得因 OCR 按钮可用就自动上传文本。当前提供者仅离线，不存在联网 fallback。
