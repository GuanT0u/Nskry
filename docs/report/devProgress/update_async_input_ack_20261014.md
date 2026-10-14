# 后台更新与输入配置确认实现报告

日期：2026-10-14。按上次审查后续项实施，不自动安装用户插件、不停止用户程序、不提交/推送。

## 先核对的问题

- 原 Settings service 的 CheckForUpdates/DownloadAndStage 同步执行，WinHTTP 等待、SHA 和 tar 解压都可能阻塞 UI。
- 原下载将整个包读入 vector；包验证又完整读取一次 ZIP。
- configure_input_v1 无返回值，只能发现异常，不能证明采用了配置。
- API 2、共享编辑归属、按需模型、安装/更新/暂存边界仍符合规范，没有以本轮需求改 reserved 或增加插件前置。

## 1. 已完成：后台更新

主线程：取得 Registry/设置快照 → worker：HTTP/解析/SHA/验证/解压 →
主线程：核对当前 ID/版本/来源并提交 pending → 重启应用更新。

- UpdateTask 按 Start 创建单个线程和完成窗口；没有永久线程池、定时轮询或启动网络。
  不排无限队列；已有任务或取消清理未完成时拒绝新任务，页面提示稍后重试。
- worker 仅持有值快照、任务私有文件、stop_token；不访问活 Registry/UI、不加载或卸载插件 DLL。
- 完成消息只传 generation，不传堆指针。主线程 join 已完成 worker 后交付结果。
  关闭/取消清除回调；页面 weak lifetime 与 generation 额外拒绝迟到/旧结果，避免 HWND/页面地址复用。
- Cancel 不在设置页 join；WinHTTP 的异步请求等待可被取消事件打断，由 worker 关闭请求。
  请求 context/read buffer 保留到最后 HANDLE_CLOSING 回调。不是跨线程关闭同步请求。
- 窗口、stop state、自建线程、HTTP/CNG/process/job/event 句柄、响应/快照在任务完成后释放。
  发布后才收到取消的准备文件，使用同一短任务设施清理，不经已关闭页面。
  程序退出先 Cancel/join 更新任务，再销毁服务、Registry 和插件生命周期。
- Check 按钮在运行时显示 Cancel update；切页/选择插件后仍保留任务结果提示。
  更新过程中暂禁安装/禁用/卸载按钮及拖入安装，避免竞争；其他设置、截图、监控消息仍可处理。
- 下载必须用户逐项确认，不自动接受更新。部分源失败不宣称全部最新，后续下载提示与详情保留检查警告。
- HTTP 文本 2 MiB，包 128 MiB；传输/哈希使用 64 KiB 块，包不整份缓存在内存。
  ZIP 验证只读取不超过 64 KiB 的尾部及最多 16 MiB 的目录；最多 4096 项、声明解压总量 512 MiB。
  不支持 ZIP64；超预算模型包需要单独评估，不默默提高全体插件限额。
- 检查整体网络期限 120 秒，下载请求期限 120 秒，并保留阶段 WinHTTP 超时。
  验证/哈希分块检查取消，tar 独立 120 秒上限；取消/超时终止的仅是本任务创建并放入 job 的子进程。
  自建 worker 和解压子进程降低 CPU 优先级，优先让出交互预算。
- 准备文件由 RAII 管理；失败/取消/不匹配删除临时 archive/tree。提交成功后 pending-updates
  是重启交付的持久化数据，不作为临时资源删除。下载完再次检查版本/来源，过时结果拒绝提交。

Registry 与 pending 写入、目录重命名、清理旧暂存仍属于主线程提交点，不支持提交到一半再取消。
这段没有网络/子进程等待，但极慢磁盘、杀毒扫描或旧目录删除仍可能带来短期延迟，需要实机测量。
本地 Browse 的 Inspect/Install 兼容入口仍同步，不属于此次后台检查/下载链路。
网络/子进程期限不代表异常存储驱动、最终系统回调或进程终止的硬实时保证；不强杀用户程序或 detach 遗留线程。

## 2. 已完成：可选输入确认 v2

- 新导出 nskry_plugin_configure_input_v2，输入仍用原 NskryInputConfigV1；新 caller-owned result version=2。
  返回 Applied / Failed / Unsupported，附容量受限 UTF-16 原因，不保留 caller 指针。
- v2 优先；明确失败/不支持不悄悄 fallback。只有 v2 不存在才使用 v1。
- v1 正常返回与无局部输入导出的旧包都标为“未提供确认”，不是失败，也不宣称已确认。
  未加载包标为“未加载，尚未查询确认”；发现/查询不执行 DLL。
- Apply 只更新已加载包；结果在插件详情及 Apply 提示显示，不依赖通知开关。
- SDK helper 校验全部命令/语法后整体替换，失败保留上一有效配置。
  status 表示插件的明确声明，不是对第三方实际行为或身份的安全认证。
- OCR 0.6.2 同时保留 v1/v2，旧宿主仍可调用 v1；没有模型预热或 OCR 识别算法改动。
  Scroll 旧包/仅 launch 不需要为本轮安装额外插件。

设置持久化成功不等于所有插件接受运行态配置；拒绝/不支持/未确认会分别说明。
进程内恶意或违反契约 DLL 的访问违例、死循环并非 C++ catch 可安全隔离；不是新增沙箱。

## 3. 小范围拆分

- UpdateTask 仅负责一次工作的调度/取消/交付，更新策略仍在 PluginUpdateManager。
- PackageManager 增加值式 PreparePackage 与主线程 InstallPrepared，旧 InstallPackage 复用同一准备/提交路径。
- Settings 更新流程移到 plugin_update_flow.cpp；不增加第二个插件管理器或公共虚构服务接口。
- 保留 API 2 结构/预留位、配置兼容、安装来源政策、重启更新及未来依赖规划。

## 4. 验证

分阶段 Release 构建成功；输入扩展 v2/v1/无扩展专项回归通过。
最终 build/verify Release 完整构建成功，收尾后的完整 CTest **19/19 通过**（12.98 秒），
git diff --check 通过。网络/快捷键原生验证使用批准的普通桌面权限。

新增/扩展覆盖：

- 主线程交付、重复 Start 拒绝、Cancel 非阻塞、worker 异常、退出 join、捕获资源释放。
- 本机停滞 TLS 连接上的真实 WinHTTP 超时/取消，无证书校验绕过，不访问外部服务。
- HTTPS/响应大小/溢出策略、预取消文件清理、既有目标不覆盖、不误删、SHA 成功/不匹配。
- 设置页取消/关闭/迟到回调、没有打开页面即联网、等待任务期间基础框选与监控控制回归。
- 不变 Registry 快照、准备取消、提交时版本/来源变化拒绝、包 ID/version 不匹配、重启暂存/应用、未提交准备目录清理。
- v2 采用/失败/不支持/非法返回值/异常，v1 未确认、无扩展旧包、未加载状态查询；非法配置不部分覆盖。
- 原有 OCR、模型、拼接/编辑与采集测试保留。

测试使用 build 内独立 Registry/config 和 fixture，不安装到真实用户目录。
UI 测试在任何可能初始化 Settings singleton 的交互之前重定向配置，并核对实际路径后才允许写入。
受限环境不能稳定执行系统快捷键/WinHTTP；这些测试在批准的普通桌面权限下复测。

## 5. 交付与尚需实机确认

常用 build/Nskry.exe 链接受阻（LNK1104），未覆盖或停止旧实例；交付验证路径 build/verify/Nskry.exe。
installer/bundled/nskry-ocr.nskryplugin 为 0.6.2，生成不等于已安装；现有 v1 仍兼容。

仍需实机：真实官网/第三方 HTTPS 成功下载、代理/认证/TLS/重定向、连续取消与切页、
下载/解压期间截图和实时监控的延迟，退出时网络/磁盘压力、权限丢失/文件占用的清理效果。
官方站尚未上线，不伪造地址；没有进行真实远程更新或真实安装验收。
临时路径在正常失败/取消时清理；系统拒绝删除、崩溃/断电等情形不宣称完全无残留。
没有低配机器 CPU/RAM/工作集基准，不把释放对象/句柄等同于 OS/CRT 缓存立即归还全部物理内存。

实现依据：[WinHTTP 关闭及最终回调](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpclosehandle)、
[WinHTTP 并发边界](https://learn.microsoft.com/en-us/windows/win32/winhttp/concurrency-in-winhttp)、
[异步通知消息](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendnotifymessagew)。
