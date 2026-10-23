# 后续性能与构建空间优化（2026-10-23）

## 1. 按需创建共享GPU设备

Core 不再在普通启动入口创建 D3DDevice。首次监控或实际插件初始化/执行才由 UI 线程创建，创建失败不留下半初始化全局对象，下次用户操作可重试。普通截图、固定截图、基础编辑和设置元数据列表不依赖此设备。

为保持旧API2插件兼容，PluginManager 使用宿主内部 graphics provider，在进入插件 init 前准备借用设备，并为 execute 的本地context副本准备同一设备；不更改SDK结构或reserved。不对声明身份/名称特殊判断。明确选择startup加载的插件依然可能在启动阶段触发设备创建，不能为省资源违背其旧初始化契约。已创建设备保留到插件shutdown及监控清理之后，不在插件仍可能借用时回收。

回归覆盖：metadata/快捷键发现不调用provider，设备准备异常不进入init并释放DLL，失败后重试，init和execute设备身份一致、execute准备失败不调用动作，shutdown清理provider，以及显式startup插件仍在init前准备设备。现有v1/v2/无输入扩展插件三组回归通过，录屏插件生命周期回归通过；未对真实主程序启动前后私有内存单独测量，不给出未经测量的MiB/CPU收益。

## 2. 公共预编译头收敛

公共pch仅保留WinRT base，采集projection、Foundation和COM interop按实际使用放进capture模块，避免JSON、配置、更新和普通编辑文件被迫解析WGC。不禁用PCH，不删除SDK/模型/规划接口，不对产品体积或运行内存作不实声明。

同一编译器/Release配置下，`build/verify/CMakeFiles/Nskry.dir/cmake_pch.cxx.pch`从450,494,464字节（429.6MiB）降为179,044,352字节（170.8MiB），减少258.9MiB/60.3%。常用build也重建为相同大小；两套当前构建合计减少约517.8MiB的PCH占用。历史build未删除，编译速度没有同条件前后基准，不宣称具体加速比例。

移除传递include后，完整构建通过；重建影响的全部源码及真实采集、监控UI等32项回归通过（54.72秒）。常用build/Nskry.exe也已更新。
