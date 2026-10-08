# Offline OCR plugin 0.6.0

精确模式默认使用 PP-OCRv4 检测/识别模型与 ONNX Runtime CPU，快速模式保留
Windows OCR 的按需 A/B/C 流程（最多三轮）。本次没有在线 OCR、LLM、词典纠错，
没有将预期测试文本用于识别。模型的字符标签表仅用于 CTC 解码。

## 安装和使用

构建生成 `installer/bundled/nskry-ocr.nskryplugin`。在 Settings → Plugins 导入
完整包，确认更新后完全退出并重启 Nskry。不能只替换 DLL：包内的 `engine/`
含 EXE、运行库、两个模型及第三方声明。本次开发不会自动安装到用户目录。

结果页的 Accurate/Fast OCR 按钮可重新识别原图；旋转后保留所选模式。
选择仅在当前运行会话中保留。精确识别时显示可取消的进度窗；引擎缺失或失败会
明确显示错误，并可切换 Fast OCR，不会无提示降级。显示、复制、编辑使用原尺寸图。

## 资源生命周期和限制

- 只有触发 OCR 时启动隐藏的原生识别进程，不预热、不常驻、不在监控循环中识别。
- 每次任务结束进程退出；取消、新请求替代、插件关闭会终止并等待该任务的进程。
- 默认超时 20 秒；Job Object 限制一个子进程、1 GiB 进程提交内存。
- 输入最多 16 百万像素，任意边不超过 8192；大图识别栅格最长边缩至 2000。
- ORT 使用 1–4 个线程、关闭空转；OpenCV 单线程。没有 GPU 要求。
- 图像以未命名共享内存传输，结果通过限定大小的管道返回，不写临时截图文件。
- 父进程 DLL 不加载 ORT/OpenCV；结果窗保留原图与文字属于必要 UI 内存，关闭释放。

模型不保证完全识别；图标、标点、小字及过大图缩小后的字仍可能漏/错。
字符高亮边界由 CTC 时间轴近似映射，不是精确的字符像素分割。
不能保证低配置机器 1–2 秒，当前实测和未验证项见开发报告。

## 构建

需要 x64 MSVC、CMake/Ninja，准备依赖需要 Python 3.11+：

```powershell
python tools/prepare_ocr_dependencies.py
cmake -S . -B build/verify -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/verify --parallel 2
ctest --test-dir build/verify --output-on-failure
```

在已加载 MSVC 环境的终端执行。下载与源码仅位于工作区 `build/ocr-deps/`；
不安装 Python 包，生产包不携带 Python。固定版本/哈希见脚本和第三方声明。
`-DNSKRY_OCR_MODEL_ENGINE=OFF` 可构建不依赖模型的 Windows-only 版本。
运行仍需要常规 MSVC x64 运行库（含 Concurrency Runtime）。

调试设置 `NSKRY_OCR_ENGINE=fast` 或 `accurate` 可选择初始引擎；
`NSKRY_OCR_TRACE_FILE` 可启用本地文字/坐标/耗时日志。日志含识别内容，默认关闭，
请勿在公开发布中无意泄露。发布带模型的版本前须核对
[第三方声明](THIRD_PARTY_NOTICES.md) 中准确模型权重的许可归属。
