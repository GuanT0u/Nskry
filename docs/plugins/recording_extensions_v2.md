# 按需采集 v2、额外 capture 动作 v1、设置 UI v1（2026-10-21）

三个独立的可选扩展，不改变 `NskryPluginInfo` / `NskryHostContext` API 2 布局、不使用 reserved。
旧插件和 region_capture v1 保持可用。standalone 和跨插件服务依然未实现。

## 采集 v2

声明 `"extensions":{"region_capture":2}`，导出 `nskry_plugin_connect_capture_v2`。
加载时宿主复制服务表给插件；缺失/不兼容明确拒绝，不降级成别的采集语义。
旧宿主不能运行新录屏包，须配合同轮构建 Core；版本1仍接受原先1..30fps。

头文件 `sdk/nskry_capture.h`：`NskryCaptureRequestV2` 包含 structSize/version=2、物理选区、1..60fps、
可空 followWindow、最长/短边输出限制（0为1920/1080）。输出描述沿用独立 v1 结构。
open/read/bounds/close 必须在同一个消费者自有MTA线程串行调用；所有帧池、D3D device/context归此会话，
不向后台开放宿主共享immediate context。read复制像素到调用方容量受限的缓冲，S_FALSE表示无新帧。
bounds返回最后一次read检查后的实际物理选区；不能从UI线程并发读取会话。

followWindow为空：单显示器固定屏幕坐标。非空：选区必须完整落在初始DWM可见窗口边界内，
转为窗口相对裁剪，使用窗口WGC源（不是跟着坐标偷偷录桌面）。只跟随同屏、同DPI、同尺寸移动。
窗口最小化、关闭、尺寸改变、跨屏、DPI/设备/显示器改变均返回失败。插件负责收尾/通知。
暂停关闭采集；继续时校验原窗口线程/PID、大小，并按原相对偏移重开，不跳到别的源。

输入尺寸和全屏帧池上限不变；输出最长边≤1920、短边≤1080、偶数对齐。目标60fps不是交付保证。
设置仅在Windows支持时限制生产间隔，老Windows只限制消费。系统采集边框仍保留。

## 额外 capture 动作 v1

头文件 `sdk/nskry_actions.h`，示例：

```json
{
  "extensions":{"capture_actions":1},
  "capture_actions":[{"id":"gif","label":"Record GIF"}]
}
```

每包最多8个额外动作，ID使用安全ID规则且包内唯一，标签1..40字符。
宿主只读manifest建立工具栏；点击才EnsureLoaded，交付 `NskryCaptureActionV1` 给
`nskry_plugin_capture_action_v1`。返回非零表示接受，不应阻塞UI；context/id仅调用期间借用。
主execute/旧快捷键不变；额外动作携带独立actionId，不编码进pluginId、不根据Nskry包ID硬编码。
未声明动作、禁用包、未知扩展版本、缺导出均拒绝。图像post_capture及standalone不自动获得这些动作。
后续跨surface多动作、图标、排序及快捷槽仍须另外设计，不把此扩展当作完整API3。

## 设置 UI v1

声明 `"extensions":{"settings_ui":1}`，导出 `int32_t nskry_plugin_show_settings_v1(HWND owner)`。
Settings → Plugins 的 Plugin settings 按钮只由metadata决定是否可用；用户主动点击才加载DLL。
在宿主UI线程调用，非零表示已展示。插件负责自己的设置窗口/关闭生命周期，不能修改宿主控件或Registry管理器。
init/展示设置不得预热采集、编码/模型或常驻worker。禁用/不兼容包不新开设置；更新任务期间入口禁用。

录屏默认参数属于插件，使用其私有HKCU key，仅写入已实现的版本化VideoPreset值，未知值不覆盖。
不是Core UserSettings的新录屏专用字段，第三方插件也可以声明相同通用设置入口。
