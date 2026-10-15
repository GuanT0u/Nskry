# 插件快捷键与输入扩展 v1 / v2

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
- 2026-10-15：显式声明 `extensions.commands=1` 后可用 `command`，仅控制已加载任务，见
  [可选命令扩展](recording_extensions_v1.md)。不替代 capture/launch，不加载闲置 DLL，也不传入局部配置导出。
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
宿主防御性捕获更新异常并报告失败插件；v1 成功返回只标为“未提供确认”，不是失败或已确认生效。
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

## 可选的 v2 确认（2026-10-14）

API 2 和 NskryInputConfigV1 的布局保持不变，新增独立导出：

```cpp
extern "C" NSKRY_API int32_t NSKRY_CALL
nskry_plugin_configure_input_v2(const NskryInputConfigV1* config, NskryInputResultV2* result) {
    return nskry::input::ConfigureConfirmed(config, result,
        {{L"copy", nskry::input::Kind::Key}, {L"pan_x", nskry::input::Kind::Wheel}});
}
```

`result` 是 caller-owned 描述符：structSize、version=2、UTF-16 error 指针和包含终止符的容量。
只在调用期借用；零容量允许，禁止保留指针、超过容量写入或抛出异常。

| 返回值 | 宿主解释 |
| --- | --- |
| NSKRY_INPUT_APPLIED | 插件明确确认采用了整份局部配置 |
| NSKRY_INPUT_FAILED | 失败；提供适当原因，保留上一份有效配置 |
| NSKRY_INPUT_UNSUPPORTED | 当前描述符/局部命令不支持；不是 v1“未确认” |
| 其他值 / 异常 | 协议或调用失败；不宣称采用 |

宿主优先 v2，只有 v2 导出不存在时才调用 v1；v2 明确失败/不支持后不悄悄转调 v1。
作者建议同时保留 v1 导出以兼容旧宿主。SDK helper 校验后整体 swap，非法配置不部分覆盖。
无导出且无局部快捷键元数据的旧包照常运行，状态也是“未提供确认”；有局部元数据却没有任一配置导出仍拒绝加载。
未加载包显示“未加载，尚未查询确认”，GetInputReport 不执行 DLL；Apply 只更新已加载包。
状态可在 Plugins 详情查看，Apply 的失败/不支持/未确认说明不依赖通知开关。
这只是插件的协议声明，不是对第三方 DLL 实际行为、发布者身份或安全性的认证。
