# 区域采集与已加载插件命令扩展 v1（2026-10-15）

API 2 的 `NskryPluginInfo` / `NskryHostContext` 及 reserved 完全不变。
这不是 API 3 多动作/跨插件服务，也没有启用 standalone 预留位。

## 元数据与协商

manifest 可声明 `"extensions":{"commands":1,"region_capture":1}`，每项可独立省略。
显式声明未知版本会拒绝，不静默当成功；普通未知描述字段的兼容规则不变。
新功能需要本轮宿主支持；旧宿主即使忽略 extensions，插件 init 仍会拒绝没有采集服务的执行。
旧插件不声明扩展，继续原加载/输入流程，无需重编译。

`commands:1` 允许 shortcuts 新增 `scope:"command"`（id 不能是 launch）。
例如 `stop` 是 `plugin.nskry-record.stop`，默认空、用户在现有 Hotkeys 页面设置。
`scope:"capture",id:"launch"` 仍是宿主框选后 execute，不是独立命令。
General 跳转、冲突检查、录制/输入/删除和持久化沿用原机制。
全局配置不传入插件本地 input v1/v2；本地输入确认协议没有变化。

宿主加载声明 commands 的 DLL 时要求 `nskry_plugin_command_v1` 导出。
传入 `NskryCommandV1`（structSize/version=1/id）和 caller-owned `NskryCommandResultV1` 错误缓冲。
返回 OK / FAILED / UNSUPPORTED；错误缓冲不能保留或越界，导出不能抛异常。
命令在 UI 线程执行，必须非阻塞；只允许 manifest 声明的 command ID，且只向已加载 DLL 交付。
闲置 stop 不加载 DLL。禁用后仍允许已加载实例收到显式清理命令，不能因此启动新的 execute。
这不提供原生 DLL 沙箱，也不承诺恶意插件遵守清理语义。

## 区域采集

头文件：`sdk/nskry_capture.h`。声明 region_capture 的插件必须导出
`nskry_plugin_connect_capture_v1`，在 init 前接受并复制宿主服务表；不保留描述符指针。
init 失败仍执行 shutdown。没有服务或协商失败时拒绝加载；不退回 GDI 截图循环。

- open/read/close 在同一个插件自有 MTA worker 上串行调用，不能在 UI 上启动/读回。
- open 创建专用 D3D11 device/context、monitor WGC pool（2 帧）、会话；无宿主共享 immediate context。
- RECT 是物理屏幕坐标，只支持同一个 monitor 内的固定区域。不追踪 sourceHwnd。
- 跨屏拒绝，负坐标屏幕允许；窗口移动/关闭/最小化后该位置的桌面内容就是下一帧。
- 输入边长 2..3840，总像素不超过 3840×2160；显示器总像素不超过 16,777,216。
- 输出最长边不超过 1920、短边不超过 1080，近似等比缩小并偶数对齐；极细缩小后不足 2 px 的区域拒绝。
- read 返回 caller-owned top-down BGRA，stride=width×4；S_OK 新帧、S_FALSE 无新帧、失败终止。
  不返回 WGC frame、借用 texture 或共享 context。read 返回后该 CPU 数据完全由插件持有。
- 帧池最多取两帧选择较新的一帧。读回/缩放限于消费者调度频率，不形成回调/帧队列。
  新系统尝试 MinUpdateInterval，旧系统仅限制消费；不宣称旧系统 WGC 生产本身已限为 20fps。
- closed 回调仅持有 shared atomic 标志，不捕获 owner，不调用插件/UI。
  read 同时检查 item、device、显示器身份/边界/DPI；变化返回失败，不自动选择另一块屏幕。
- close 关闭会话、帧池并释放私有 GPU/COM；shutdown 必须等待 worker close 后才卸载 DLL。

本表没有额外后台线程、启动采集、全局轮询或 Registry 操作。指针与句柄仅在当前进程有效。
未来窗口跟随/多屏/音频等必须单独定义来源与时间轴，不悄悄改变此 v1 语义。

## nskry-record 0.2.0 的内部编码隔离

宿主 v1 采集仍在插件自有 MTA 线程调用，公开 ABI/所有权/固定屏幕语义没有改变。
插件可将已拥有的 CPU 像素交给包内私有编码 worker；本实现转为 NV12 后通过单槽共享内存与事件交付。
worker 不接收宿主函数指针、窗口回调、借用纹理或 immediate context；编码结果与文件发布由插件负责。
命令/采集 v1 本身不代表跨进程调用，私有 `record_worker_protocol.h` 不是新 SDK 或跨插件服务。
缺少 worker 需要重装完整包，不依赖另一个插件安装。此隔离仅涵盖 MF 编码，不涵盖宿主 WGC 原生调用。
