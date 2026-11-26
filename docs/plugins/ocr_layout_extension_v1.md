# 本地布局 OCR v1

Nskry 0.5.1 起提供可选的 API 2 扩展。入口见 `sdk/nskry_ocr_layout.h`。
已有 `NskryHostContext`、纯文本 OCR v1 与文字翻译交接 v1 保持原有 ABI。

- Provider 的 manifest 声明 `extensions.ocr_layout = 1`，采用 `on_demand`，
  不声明工具栏动作、截图后动作或快捷键。导出 `nskry_plugin_info` 与
  `nskry_plugin_ocr_layout_v1`。宿主验证 DLL 元数据中的 API、ID 和版本。
  走布局入口时无需调用插件 `init`、`execute` 或 `shutdown`。
- Feature 的 manifest 声明 `extensions.ocr_layout_client = 1`，导出
  `nskry_plugin_connect_ocr_layout_v1` 保存宿主提供的获取函数。功能安装时需要
  已启用的兼容 Provider。已有的功能缺少 Provider 时应显示修复路径。
- 在 UI 线程调用 acquire。显式 ID 仅选择该组件。默认优先启用的官方
  `nskry-ocr-provider`，其次选择唯一已安装并启用的兼容候选。用于替换的第三方插件
  遵循原生第三方插件信任规则。手工复制到目录的 Provider 默认禁用，需用户启用。
- 成功取得一个 DLL 引用。Feature 在工作线程同步调用，位图及取消上下文
  在返回前有效，`receive` 只在成功时调用一次。调用结束并确认无代码执行后，
  由取得引用的一方调用 `FreeLibrary`。Feature 卸载前先停止并等候自身任务。
- UTF-16 偏移及长度以代码单元计。矩形使用输入位图像素。词条按阅读顺序
  关联到行索引，范围指向同一份输出文本。`joinPrevious` 表示选区复制时与前一个
  相邻词条拼接。宿主回调内复制需要保留的任何数据。
- `fast=0` 请求精确模式，`fast=1` 是显式快速模式偏好。Provider 在能力不足时
  应返回明确状态和可选错误文字。空白、取消、超时、繁忙和资源缺失有独立状态。
  官方实现每次启动独立受限识别工作进程，并行重任务返回繁忙，由用户明确重试。
  退出前等待当前工作进程结束。Provider 不使用截图 UI，也不上传截图。

官方伴随包可放在功能包同目录。手动导入时先确认安装，安装器和 CLI 可用
`--with-ocr-provider`。更新渠道如需添加此前缺少的 Provider，应先安装 Provider，
再更新功能。待重启的操作会再次检查依赖。卸载最后一个消费者后，用户可以独立
移除 Provider。第一方界面使用默认解析，SDK 支持指定 ID。
