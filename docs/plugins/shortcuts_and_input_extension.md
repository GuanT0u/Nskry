# 插件快捷键与输入扩展 v1

2026-10-12。API 2 的 context/info 布局和 reserved 不变，采用独立的可选版本化导出。

## 元数据

manifest v1 可声明最多 32 个快捷键：

```json
"shortcuts": [
  {"id":"launch","label":"Recognize a region","scope":"capture","default":"","general_jump":true},
  {"id":"copy","label":"Copy selected text","scope":"key","default":"Ctrl+C"},
  {"id":"pan_x","label":"Horizontal pan","scope":"wheel","default":"Shift+Wheel"}
]
```

- 局部 `id` 安全、唯一且稳定，存储键是 `plugin.<插件 id>.<局部 id>`；label 为最多 160 字符的纯文本。
- `capture` 只允许 `launch` 且必须声明 toolbar_action：宿主框选后调用主 execute，
  不是任意全局命令/独立插件页面注入接口。
- `key` / `hold` / `wheel` 是插件自行实现的窗口按键、按住键、滚轮操作。
- `default` 可以为空；用户显式保存空值是禁用，不能恢复默认值。
- `general_jump` 默认 false，决定是否贡献 General 页跳转入口，与用户是否设键无关。
- 设置只读元数据，不为展示快捷键加载 DLL；子分区标题沿用插件名。
  所有已安装插件（包括 OCR/Scroll）按 Registry 来源分组；未安装的 OCR/Scroll 仅提供官方产品预留入口。
  Local/unverified 来源提示仍保留，官方认证/catalog 不由 ID 或作者字符串替代。

## 配置导出

声明可配置局部输入的包必须导出以下接口，否则宿主拒绝加载，避免只显示可编辑而实际不生效：

```cpp
#define NSKRY_PLUGIN_EXPORTS
#include "nskry_plugin.h"
#include "nskry_input.h"
extern "C" NSKRY_API void NSKRY_CALL
nskry_plugin_configure_input_v1(const NskryInputConfigV1* configuration) {
    nskry::input::Configure(configuration);
}
// 自己的 UI 消息中：
// if (nskry::input::PluginMatches(L"copy", L"Ctrl+C", UINT(wp))) CopySelected();
```

版本为 1，structSize 校验；字符串只在调用期间有效，必须复制。
宿主成功 init 后、第一次 execute 前传入自己的局部 id；capture/launch 全局绑定不传入 DLL。
Apply 更新已加载插件，不加载未使用 DLL。配置导出和 SDK 输入匹配辅助方法在 UI 线程使用，
插件自行将所需不可变快照交给 worker，不从后台并发访问 SDK 配置缓存。导出函数不得抛出异常；
宿主防御性捕获更新异常并报告失败插件，但没有返回值的 v1 无法证明插件内部实际接受了配置。
旧包不强制导出，旧 OCR 元数据只显示 launch 兼容入口；OCR 0.6.1 起支持完整局部输入配置。
旧宿主可能忽略扩展，插件仍应保留未配置 fallback。
不要直接读取宿主私有设置、操作宿主 HWND、注册未声明的全局键或截获其它应用输入。
多命令与独立服务仍需其它版本化契约，不占用 reserved 或依赖 OCR/Scroll 私有窗口。

## 输入边界

- 普通/全局组合为修饰键加一个主键；按 Windows VK、精确修饰键匹配。
  支持字母、数字、F1–F24、方向键、Esc、Enter、Insert、PrintScreen、命名标点/数字键盘等。
- 按住键例：Alt、Space、Ctrl+Space；滚轮例：Wheel、Shift+Wheel。
- 录制收集开始按下到全部抬起期间的键集合，实时展示，忽略重复，左右修饰键独立跟踪、统一显示。
  多主键不会悄悄丢弃，但 Apply 明确拒绝本版不能注册的组合。
- 同上下文冲突及全局遮蔽局部键拒绝；不同窗口上下文可以复用 Esc。
  全局交换/删除/占用失败回滚，不承诺其它程序抢占期间的跨程序原子性。
- 录制不装全局键盘 hook；失焦/离页/关闭就取消并恢复 Nskry 注册。
  Windows 保留组合、其它应用全局键不保证能够录制/注册。
- 系统窗口快捷键、原生文本框编辑习惯及鼠标右键取消不属于可替换应用键。
  OCR 右侧文本框保留原生复制/粘贴，额外接受配置的复制组合。

扩展不改变插件资源生命周期、OCR 推理或滚动采集链路。
