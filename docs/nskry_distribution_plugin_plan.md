# Nskry 二次规划：Distribution / Settings / Plugin Ecosystem

> 面向后续 Work 区实现与重构使用。  
> 本文基于当前 Nskry 的原生 C++ / Win32、托盘常驻、插件 DLL ABI 架构进行扩展。
>
> 核心目标：在不明显增加 idle RAM 的前提下，补齐安装器、设置中心、官方/第三方插件安装、更新、卸载与插件元数据体系。

---

## 1. 本阶段目标

Nskry 后续需要形成完整的普通用户使用链路：

```text
下载 NskrySetup.exe
        ↓
安装 Nskry
        ↓
安装过程中可勾选官方插件
        ↓
Nskry 常驻系统托盘
        ↓
托盘 → Settings
        ↓
快捷键 / 个性化 / 插件管理
        ↓
官方插件：
安装 / 更新 / 禁用 / 卸载

第三方插件：
拖入 .nskryplugin
        ↓
验证 → 安装
        ↓
进入统一插件列表
        ↓
更新 / 禁用 / 卸载
```

现有 PluginManager 不应继续膨胀为“大一统管理器”。

应逐步形成：

```text
Plugin Runtime
      ↑
Plugin Registry
      ↑
Plugin Package Manager
      ↑
Plugin Update Manager
      ↑
Settings / Plugin UI
```

---

# 2. 总体架构

建议拆分如下：

| 模块 | 职责 |
|---|---|
| `PluginManager` | 仅负责插件 DLL 的 Load / Init / Invoke / Shutdown / Unload |
| `PluginRegistry` | 保存已安装插件、版本、来源、启用状态、加载状态等 |
| `PluginPackageManager` | 插件包安装、覆盖安装、卸载、验证、staging |
| `PluginUpdateManager` | 检查更新、下载、哈希校验、暂存更新 |
| `SettingsManager` | 读取/保存用户设置 |
| `HotkeyManager` | 动态注册、注销、修改全局快捷键 |
| `CommandRegistry` | command id → 可执行动作映射 |
| `SettingsWindow` | 设置中心 UI |
| `Downloader` | 通用 HTTPS 下载 |
| `PackageVerifier` | manifest / hash / compatibility / package 安全检查 |

职责边界：

```text
PluginManager
=
运行插件

PluginPackageManager
=
安装插件

PluginRegistry
=
记录插件

PluginUpdateManager
=
更新插件

SettingsWindow
=
让用户控制以上能力
```

---

# 3. Idle-first Architecture

这是本阶段的硬性设计原则。

Nskry 的目标不是“功能越多，后台 RAM 就越高”，而是：

```text
Installed ≠ Loaded
UI Exists ≠ UI Initialized
Feature Exists ≠ Feature Running
```

## 3.1 Idle 状态允许常驻的内容

Nskry 启动后、用户没有打开 Settings、没有使用任何功能时，只应保留：

```text
Tray / message loop
HotkeyManager
SettingsManager 的少量配置数据
PluginRegistry 的少量 metadata
核心截图功能所需最小状态
必要的系统事件监听
```

禁止因为“功能存在”而启动：

```text
SettingsWindow
插件列表 UI
官方插件 catalog 网络请求
插件图标/bitmap cache
不需要后台工作的插件 DLL
OCR runtime
录屏 runtime
翻译 runtime
其他重资源 runtime
```

目标：

```text
Nskry 功能持续增加
        ≠
idle RAM 线性增加
```

---

# 4. Installed != Loaded

插件必须区分至少四个状态：

```text
Not Installed
Installed + Disabled
Installed + Enabled + Unloaded
Installed + Enabled + Loaded
```

例如：

```text
OCR

Installed: Yes
Enabled: Yes
Loaded: No
```

只有用户第一次真正调用 OCR 时：

```text
Command invoked
    ↓
PluginManager::EnsureLoaded("nskry-ocr")
    ↓
LoadLibraryEx(...)
    ↓
nskry_plugin_init()
    ↓
执行功能
```

## 4.1 Plugin load policy

插件 manifest 增加：

```json
{
  "runtime": {
    "entry": "nskry-ocr.dll",
    "architecture": "x64",
    "api_version": 1,
    "load_policy": "on_demand"
  }
}
```

第一版支持：

```text
on_demand
startup
```

默认：

```text
on_demand
```

只有确实需要后台工作的插件才能使用：

```text
startup
```

例如：

```text
clipboard monitor
global event hook
background watcher
```

普通插件：

```text
OCR
Long Screenshot
Translation
Image Processing
Screen Recording
Upload
```

默认都应 `on_demand`。

---

# 5. Settings UI 生命周期

Settings UI 不应该随 Nskry 启动。

默认：

```text
Nskry Start
    ↓
No SettingsWindow
```

用户点击：

```text
Tray → Settings
```

才执行：

```text
Create SettingsWindow
Create controls
Load page resources
Load plugin list model
必要时请求 official catalog
```

## 5.1 关闭 Settings 的行为

如果优先考虑 idle RAM：

```text
Close Settings
    ↓
DestroyWindow()
    ↓
释放 controls
释放 fonts / bitmaps / icons
释放页面 model
释放网络 response / 临时 cache
```

而不是仅仅：

```text
ShowWindow(hwnd, SW_HIDE)
```

Settings 并不是高频窗口，重新打开时重新创建 UI 的代价可以接受。

## 5.2 UI 技术选择

继续优先：

```text
Win32
Direct2D / DirectWrite（如需要）
```

不建议仅为了 Settings 引入：

```text
Electron
WebView2
大型 Web runtime
```

如果未来采用 Qt / WPF 等，也必须评估它们对后台常驻 footprint 的影响。

---

# 6. 插件目录结构

现有扁平结构：

```text
plugins/
├── abc.dll
├── ocr.dll
└── record.dll
```

应升级为：

```text
%LOCALAPPDATA%/
└── Nskry/
    ├── config/
    │   └── settings.json
    │
    ├── plugins/
    │   ├── nskry-ocr/
    │   │   ├── manifest.json
    │   │   ├── nskry-ocr.dll
    │   │   └── assets/
    │   │
    │   ├── nskry-scroll/
    │   │   ├── manifest.json
    │   │   └── nskry-scroll.dll
    │   │
    │   └── com.example.myplugin/
    │       ├── manifest.json
    │       ├── plugin.dll
    │       └── dependencies/
    │
    ├── cache/
    │   └── downloads/
    │
    ├── staging/
    │   └── pending-updates/
    │
    └── plugin-registry.json
```

核心变化：

```text
一个 Plugin
=
一个独立目录
=
manifest + entry DLL + dependencies + assets
```

---

# 7. 插件包格式

新增统一包格式：

```text
*.nskryplugin
```

本质上可以是 ZIP container，仅改变扩展名。

例如：

```text
nskry-ocr-1.2.0.nskryplugin

├── manifest.json
├── nskry-ocr.dll
├── assets/
│   └── icon.svg
└── dependencies/
    └── xxx.dll
```

以下全部统一走 `.nskryplugin`：

```text
Setup 安装官方插件
Settings UI 安装官方插件
用户拖入第三方插件
插件更新
本地 Browse 安装
```

不要维护多套安装逻辑。

---

# 8. Plugin Manifest

现有 `NskryPluginInfo` 继续用于 DLL runtime ABI。

运行时信息可继续包含：

```text
name
id
version
author
description
iconSvg
caps
```

但以下内容属于 Package Metadata，不应依赖 LoadLibrary 才能获得：

```text
下载地址
update URL
最低/最高 Nskry 版本
CPU architecture
API version
package hash
homepage
repository
签名信息
更新渠道
```

建议：

```json
{
  "manifest_version": 1,

  "id": "nskry-ocr",
  "name": "Nskry OCR",
  "version": "1.2.0",
  "author": "Nskry Team",
  "description": "OCR recognition plugin",

  "runtime": {
    "entry": "nskry-ocr.dll",
    "architecture": "x64",
    "api_version": 1,
    "load_policy": "on_demand"
  },

  "compatibility": {
    "nskry_min": "0.4.0",
    "nskry_max": null
  },

  "source": {
    "homepage": "https://example.com",
    "repository": "https://example.com/repository"
  },

  "update": {
    "type": "manifest",
    "url": "https://example.com/nskry-ocr/update.json"
  }
}
```

重要原则：

```text
读取插件名称/版本/兼容性/更新源
不应该要求执行第三方 DLL
```

---

# 9. Plugin Registry

Nskry 自己维护：

```text
plugin-registry.json
```

它是 Nskry 的本地安装数据库，而不是插件作者提供的文件。

例如：

```json
{
  "plugins": [
    {
      "id": "nskry-ocr",
      "version": "1.2.0",

      "enabled": true,
      "loaded": false,

      "source_type": "official",
      "install_path": "plugins/nskry-ocr",
      "update_url": "https://example.com/nskry-ocr/update.json",

      "installed_at": "2026-09-17T12:30:00Z",
      "trust": "official"
    }
  ]
}
```

来源至少区分：

```text
official
third_party
local
legacy
```

加载状态：

```text
loaded
```

最好是 runtime 内存状态，不一定每次都必须持久化进 JSON。

---

# 10. 官方插件 Catalog

官方插件下载地址不要硬编码在 `Nskry.exe`。

建立远程 catalog：

```text
official-plugins.json
```

例如：

```json
{
  "schema": 1,

  "plugins": [
    {
      "id": "nskry-ocr",
      "name": "OCR",
      "latest_version": "1.2.0",

      "package_url": "https://example.com/nskry-ocr-1.2.0.nskryplugin",

      "sha256": "...",
      "min_nskry": "0.4.0"
    }
  ]
}
```

Plugins UI：

```text
Installed Registry
      +
Official Catalog
      ↓
Merge
      ↓
显示：
Installed
Available
Update Available
Disabled
Incompatible
```

Catalog 只在：

```text
用户打开 Plugins 页面
或
后台 update check 到期
```

时请求。

不要因为每次托盘启动都立即请求 catalog。

---

# 11. 第三方插件更新

第三方插件 manifest：

```json
{
  "update": {
    "type": "manifest",
    "url": "https://example.com/plugin/update.json"
  }
}
```

Update URL 不建议直接指向 DLL。

推荐：

```json
{
  "id": "com.example.test",
  "latest_version": "0.6.0",

  "package_url": "https://example.com/test-0.6.0.nskryplugin",

  "sha256": "...",
  "min_nskry": "0.4.0",

  "release_notes": "..."
}
```

UpdateManager：

```text
读取 installed version
      ↓
请求 update manifest
      ↓
SemVer 比较
      ↓
发现新版本
      ↓
下载 .nskryplugin
      ↓
SHA-256 校验
      ↓
manifest / compatibility 校验
      ↓
staging
      ↓
Restart to Update
```

第一版只需要理解：

```text
HTTPS Update Manifest
```

未来再考虑专门支持 GitHub / GitLab provider。

---

# 12. 为什么更新建议重启后应用

Windows DLL 被加载后，直接覆盖更新并不适合第一版。

理论上可以：

```text
plugin_shutdown()
FreeLibrary()
覆盖 DLL
LoadLibrary()
```

但第三方插件可能：

```text
创建线程
持有 HWND
持有 COM object
持有 GPU resource
注册 callback
持有 host pointer
```

如果插件 cleanup 不完全，热更新风险较高。

第一版统一：

```text
下载更新
    ↓
放 staging
    ↓
mark pending update
    ↓
提示 Restart
    ↓
Nskry 重启
    ↓
PluginManager 加载之前
    ↓
PackageManager 应用 update
    ↓
正常启动插件
```

卸载也可采用：

```text
mark pending_delete
    ↓
Restart
    ↓
删除插件目录
```

---

# 13. Setup.exe

最终发布：

```text
NskrySetup.exe
```

安装过程：

```text
Welcome

↓

Install Location

↓

Select Components

☑ Nskry Core

Official Plugins
☑ Annotation
☐ OCR
☐ Long Screenshot
☐ Translation
☐ Screen Recording

↓

Options

☑ Start Nskry with Windows
☑ Launch Nskry after installation

↓

Install
```

推荐优先考虑：

```text
Per-user install
```

例如：

```text
%LOCALAPPDATA%\Programs\Nskry\
```

程序：

```text
%LOCALAPPDATA%\Programs\Nskry\Nskry.exe
```

用户数据：

```text
%LOCALAPPDATA%\Nskry\
```

程序与用户数据分离。

---

# 14. Setup 不重复实现插件逻辑

Setup.exe 不应该自己维护一套 Plugin Installer。

PackageManager 成熟后，可提供 CLI：

```text
Nskry.exe --install-plugin "xxx.nskryplugin" --silent
```

Setup：

```text
安装 Core
    ↓
根据用户勾选
    ↓
调用 PackageManager
```

例如：

```text
Nskry.exe
--install-plugin bundled/nskry-ocr.nskryplugin
--silent
--source official
```

从而：

```text
Setup 安装
UI 安装
拖拽安装
插件更新
```

共用完全相同的验证与安装流程。

---

# 15. 托盘与 Settings

托盘菜单：

```text
Nskry
─────────────
Capture
─────────────
Settings...
Check for Updates
─────────────
Exit
```

Settings 第一版建议：

```text
General
Hotkeys
Plugins
About
```

可后续扩展。

---

# 16. HotkeyManager / CommandRegistry

当前硬编码 `RegisterHotKey` 逻辑应从 `main.cpp` 抽离。

建议：

```text
src/core/
├── hotkey_manager.h
├── hotkey_manager.cpp
├── command_registry.h
└── command_registry.cpp
```

保存方式：

```json
{
  "hotkeys": {
    "core.capture": "Ctrl+Alt+A",
    "core.exit": "Ctrl+Alt+Q",
    "plugin.nskry-record.main": "Ctrl+Alt+R"
  }
}
```

运行流程：

```text
Load settings
    ↓
HotkeyManager RegisterHotKey
    ↓
WM_HOTKEY
    ↓
command_id
    ↓
CommandRegistry
    ↓
执行动作
```

Plugin command 第一次执行时：

```text
CommandRegistry
    ↓
PluginManager::EnsureLoaded(pluginId)
    ↓
Invoke
```

---

# 17. Plugins UI

示意：

```text
┌─────────────────────────────────────────────┐
│ Plugins                         Check Update │
├─────────────────────────────────────────────┤
│ OCR                             Official ✓  │
│ Nskry Team                        v1.2.0     │
│ Enabled · Not loaded                         │
│                     [Update] [Uninstall]    │
├─────────────────────────────────────────────┤
│ Screen Recorder                 Official ✓  │
│ Not installed                               │
│                              [Install]      │
├─────────────────────────────────────────────┤
│ My Plugin                     Third-party   │
│ Example Developer                v0.5.0     │
│ Enabled · Not loaded                         │
│                     [Update] [Uninstall]    │
└─────────────────────────────────────────────┘

              [ Install third-party plugin ]
```

点击后展开：

```text
┌────────────────────────────────────────────┐
│                                            │
│      Drop .nskryplugin package here        │
│                                            │
│               or Browse...                 │
│                                            │
└────────────────────────────────────────────┘
```

Win32 第一版可直接使用：

```text
DragAcceptFiles
WM_DROPFILES
```

---

# 18. 第三方插件安装流程

拖入：

```text
test.nskryplugin
```

流程：

```text
1. 判断扩展名
2. 解包到 temporary directory
3. 检查 manifest.json
4. 检查 manifest schema
5. 校验 plugin id
6. 校验 architecture
7. 校验 Nskry compatibility
8. 校验 Plugin API version
9. 检查 entry DLL 是否存在
10. 检查插件 ID 冲突
11. 校验 package hash/signature
12. 向用户显示安装确认
13. Move 到 plugins/<id>/
14. 写入 PluginRegistry
15. 默认保持 unloaded
```

绝对不要：

```text
用户拖入 DLL
    ↓
立即 LoadLibrary
    ↓
执行后才判断它是谁
```

---

# 19. 第三方插件安全模型

Nskry Plugin 是：

```text
Native DLL
```

因此插件拥有与 Nskry 相同的用户权限。

第三方安装时必须明确提示：

```text
Third-party native plugin

Author: XXX
Source: example.com
Version: 1.0.0

This plugin contains native code and will run
with the same permissions as Nskry.

[Cancel] [Install]
```

Trust 状态：

```text
Official
Signed Third-party
Unsigned Third-party
```

第一阶段最低要求：

```text
HTTPS
SHA-256
严格 package path 校验
```

未来增加：

```text
数字签名
publisher identity
revocation
```

---

# 20. DLL Load 安全

第三方插件增多后，不建议继续简单：

```cpp
LoadLibraryW(pluginPath);
```

应考虑：

```text
LoadLibraryEx
LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
```

并控制 dependency 搜索路径。

Plugin ID 只允许安全字符，例如：

```text
a-z
A-Z
0-9
.
-
_
```

禁止：

```text
..
/
\
:
```

所有 ZIP extraction 必须防止：

```text
path traversal
absolute path extraction
symlink/reparse point abuse
```

---

# 21. Plugin API Version

正式定义：

```text
NSKRY_PLUGIN_API_VERSION
```

Manifest：

```json
{
  "runtime": {
    "api_version": 1
  }
}
```

加载前：

```text
Plugin API 1
Host supports API 1
→ Load

Plugin API 3
Host supports API 1
→ Do not Load
→ Requires newer Nskry
```

未来修改 ABI：

```text
Plugin API v2
```

不要直接修改旧 ABI struct 并假设第三方插件会重新编译。

---

# 22. SettingsManager

统一：

```text
settings.json
```

例如：

```json
{
  "general": {
    "run_at_startup": true,
    "notifications": true,
    "theme": "system"
  },

  "capture": {
    "default_save_directory": "",
    "tray_double_click": "capture"
  },

  "updates": {
    "check_automatically": true,
    "include_prerelease": false
  },

  "hotkeys": {
    "core.capture": "Ctrl+Alt+A",
    "core.exit": "Ctrl+Alt+Q"
  }
}
```

---

# 23. Update Check 与 idle RAM

不需要 Windows Service。

Nskry 本身已常驻托盘。

建议：

```text
Startup
    ↓
读取 last_update_check
    ↓
未到周期
    → 不联网
```

到周期：

```text
执行一次轻量检查
    ↓
完成后释放 HTTP 对象和临时数据
```

同时提供手动：

```text
Settings → Check for Updates
```

检查范围：

```text
Nskry Core
Official Plugins
Third-party Plugins with update feed
```

---

# 24. 推荐源码结构

```text
src/
├── main.cpp
│
├── core/
│   ├── plugin_manager.h/cpp
│   ├── plugin_registry.h/cpp
│   ├── plugin_package_manager.h/cpp
│   ├── plugin_update_manager.h/cpp
│   ├── settings_manager.h/cpp
│   ├── hotkey_manager.h/cpp
│   └── command_registry.h/cpp
│
├── ui/
│   ├── settings/
│   │   ├── settings_window.h/cpp
│   │   ├── general_page.h/cpp
│   │   ├── hotkeys_page.h/cpp
│   │   ├── plugins_page.h/cpp
│   │   └── about_page.h/cpp
│   │
│   └── ...
│
├── update/
│   ├── downloader.h/cpp
│   ├── semver.h/cpp
│   └── package_verifier.h/cpp
│
└── ...
```

---

# 25. 推荐实施顺序

## Stage A — Core configuration refactor

完成：

```text
SettingsManager
CommandRegistry
HotkeyManager
```

将 `main.cpp` 中硬编码快捷键抽离。

---

## Stage B — Plugin Registry + Lazy Load

升级：

```text
plugins/*.dll
```

为：

```text
plugins/<plugin-id>/manifest.json
plugins/<plugin-id>/<entry.dll>
```

实现：

```text
Installed
Enabled
Disabled
Loaded
Unloaded
Version
Source
load_policy
```

这是本阶段最优先的插件架构改造。

---

## Stage C — Package Manager

实现：

```text
.nskryplugin
Validate
Install
Upgrade
Uninstall
Staging
Rollback foundation
```

---

## Stage D — Settings UI

实现：

```text
General
Hotkeys
Plugins
About
```

要求：

```text
Lazy Create
Close → Destroy
```

---

## Stage E — Third-party Drag & Drop

实现：

```text
WM_DROPFILES
Browse...
Third-party warning
Package validation
Unified install
```

---

## Stage F — Plugin Update

实现：

```text
Official Catalog
Third-party Update Manifest
SemVer
Download
SHA-256
Staging
Restart to Apply
```

---

## Stage G — Setup.exe

最后实现：

```text
NskrySetup.exe
```

此时 installer 只负责：

```text
Core install
官方插件勾选
调用 PackageManager
startup shortcut / uninstall metadata
```

不要让 Setup 自己维护第二套插件安装逻辑。

---

# 26. 双进程架构：未来预留，但当前不要求

未来可以考虑：

```text
Nskry.exe
=
Tray / Hotkeys / Capture / Plugin Runtime / Core

NskrySettings.exe
=
Settings UI / Plugin Management UI / Heavy UI resources
```

打开设置：

```text
Nskry.exe
    ↓
CreateProcess("NskrySettings.exe")
    ↓
IPC
```

关闭 Settings：

```text
NskrySettings.exe exits
    ↓
UI runtime / heap / controls / GDI resources
100% 由 OS 回收
```

当前阶段不建议立即拆分。

但架构上应该避免让 Settings UI 与 Core 强耦合，方便未来迁移。

建议从现在就遵循：

```text
Settings UI
    ↓
调用 Core service/interface

而不是：

Settings UI
    ↓
直接操作 main.cpp 全局变量
```

例如定义：

```text
ISettingsService
IPluginService
IHotkeyService
IUpdateService
```

当前实现可以是进程内函数调用。

未来双进程时，只需要把 service transport 从：

```text
Direct Call
```

替换成：

```text
IPC
```

而 UI 业务逻辑不用大改。

---

# 27. 当前阶段硬性约束

Work 区后续实现时应遵守：

1. **Installed != Loaded**
2. 普通插件默认 `load_policy = on_demand`
3. Settings UI 不得随 Nskry 启动
4. Settings 关闭后优先 Destroy，而不是永久 Hide
5. 插件 catalog 不得无条件随启动请求
6. 插件安装/更新/Setup 必须共享 PackageManager
7. 不通过执行第三方 DLL 获取 package metadata
8. PluginManager 只负责 runtime，不负责 package lifecycle
9. Native third-party plugin 必须有明确安全提示
10. 新增功能必须考虑 idle RAM，不允许随功能数量线性增加后台 footprint
11. UI 与 Core 通过清晰 service boundary 交互，为未来双进程预留迁移路径
12. 第一阶段不要为了“双进程”引入不必要 IPC 复杂度

---

# 28. 目标状态

最终 idle 架构应接近：

```text
Nskry.exe
│
├── Tray                     常驻
├── Message Loop             常驻
├── HotkeyManager            常驻
├── SettingsManager          极少量常驻数据
├── PluginRegistry           极少量 metadata
├── Capture Core             必要最小状态
│
├── Plugin DLL               默认不加载
│   └── first-use lazy load
│
└── Settings UI              默认不存在
    └── open → create
        close → destroy
```

项目核心目标：

```text
功能越来越多
但 idle Nskry 仍然保持轻量
```

这应该成为 Nskry 后续所有架构决策的默认约束。
