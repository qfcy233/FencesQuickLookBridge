# Fences QuickLook Bridge

[English](README.md)

这是一个轻量的 Windows 原生桥接程序，让 Stardock Fences 6 的 **Folder Portal**
也能使用 [QL-Win QuickLook](https://github.com/QL-Win/QuickLook) 的空格键预览。

普通桌面栅栏和文件资源管理器仍完全使用 QuickLook 原版逻辑。本程序只接管已经确认属于
Folder Portal 的选中项，避免重复预览，以及从 Portal 切换到普通栅栏时闪烁旧文件。

本项目解决的是 QuickLook 中长期存在的
[issue #332](https://github.com/QL-Win/QuickLook/issues/332)。

## 功能特点

- 根据“当前选中的文件”判断，不依赖鼠标位置。
- 支持 Folder Portal 内的多层子文件夹。
- Fences 在目录跳转时重建列表窗口后，会自动重新绑定。
- 纯 Win32、事件驱动；没有轮询、CLR、自动联网或遥测。
- 只保留一个带托盘图标的进程，不需要管理员权限。
- 不注入、不补丁、不修改 Fences 或 QuickLook。

## 运行要求

- Windows 10 或 Windows 11 x64
- Stardock Fences 6，并已创建 Folder Portal
- [QL-Win QuickLook](https://github.com/QL-Win/QuickLook)

Fences 没有为此提供公开 API，本程序需要识别其窗口实现细节，因此未来的 Fences 或
QuickLook 更新有可能需要同步适配。

## 安装

1. 从 Releases 下载 `FencesQuickLookBridge-v3.4.2-win-x64.zip`。
2. 解压全部文件。
3. 右击 `install.ps1`，选择“使用 PowerShell 运行”。
4. 在 Folder Portal 里选中文件，然后按空格键。

如果 Windows 阻止脚本，可在解压目录打开 PowerShell 并运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
```

安装脚本会把程序复制到当前用户的 Local AppData，添加当前用户自启动项、创建桌面快捷方式，
然后启动托盘进程；全程不需要管理员权限。

目前发布的 exe 没有数字签名，Windows SmartScreen 可能会显示提醒。仓库公开了完整源代码和
可复现的构建命令，便于自行审计和编译。

## 托盘菜单

- 双击托盘图标：显示版本、进程 ID 和检测到的 Portal 数量。
- 右击并选择“刷新”：更改 Folder Portal 后刷新映射。
- 右击并选择“退出”：停止程序。

程序有单实例保护，正常情况下只运行一个进程。

## 卸载

运行 `uninstall.ps1`。它会停止本程序，移除当前用户自启动项和快捷方式，并且只删除
`%LOCALAPPDATA%\Programs\FencesQuickLookBridge`。Fences 和 QuickLook 不会被修改。

## 从源码构建

安装 [Zig 0.16.0](https://ziglang.org/)，把 `zig` 加入 `PATH`，然后运行：

```powershell
.\build.ps1
```

也可以指定编译器路径：

```powershell
.\build.ps1 -ZigPath C:\path\to\zig.exe
```

x64 程序会生成到 `build\FencesQuickLookBridge.exe`。

## 工作原理

程序监听 Windows 文件列表的选择变化事件，记录当前选择属于哪一个列表。只有确认当前选中项
属于 Folder Portal 时，无修饰键的空格才会交给休眠工作线程解析路径，并通过 QuickLook 已有的
当前用户本地命名管道发起预览；其他空格会立即交给原版 QuickLook。

进入多层目录后，程序每次请求只打开一次 Explorer 进程并复用远程列表缓冲区。小型缓存只保存
项目数量和三个精确锚点，重复预览通常无需重新完整匹配目录。文件名不会离开本机。

## 隐私与安全

- 不联网，没有更新检查、分析或遥测。
- 不需要管理员权限，不安装服务、驱动，不注入 DLL 或修改其他进程。
- 只读取解析本地选中路径所必需的 Folder Portal 列表数据。
- 只与当前用户的本地 QuickLook 命名管道通信。

## 许可证与商标

源代码采用 [MIT License](LICENSE)。

这是独立的社区项目，与 Stardock Systems, Inc. 和 QL-Win QuickLook 项目没有隶属或背书关系。
Fences、QuickLook 及相关名称和商标归各自权利人所有。
