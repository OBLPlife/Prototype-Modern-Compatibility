[简体中文](README.md) | [English](README_EN.md)

# 虐杀原形（2009）现代兼容补丁

适用于 PC 版《虐杀原形》（Prototype 2009）的非官方现代兼容补丁，重点改善启动兼容性、现代分辨率、显示模式、运行稳定性及高帧率下的部分游戏机制。

**当前版本：** [v0.3.3 预览版](https://github.com/OBLPlife/Prototype-Modern-Compatibility/releases/tag/v0.3.3)

本补丁与汉化、高清项目相互独立，不包含游戏本体或汉化内容。

## 安装与还原

**安装**

1. 退出游戏及 Steam。
2. 从 [Releases](https://github.com/OBLPlife/Prototype-Modern-Compatibility/releases) 下载 [中文安装器](https://github.com/OBLPlife/Prototype-Modern-Compatibility/releases/tag/v0.3.3)（`Prototype_Modern_Compatibility_CN_Setup.exe`）或英文安装器（`Prototype_Modern_Compatibility_EN_Setup.exe`）。
3. 将安装器放入游戏根目录，与 `prototypef.exe` 位于同一文件夹。
4. 运行安装器，点击「安装」。
5. 安装完成后重新启动 Steam，按照安装器提示启动游戏。若提示使用启动辅助程序，请运行游戏目录中的 `PrototypeModern_Play.lnk`。

**还原**

退出游戏及 Steam，再次运行安装器，点击「还原」。

安装器会备份和核对修改涉及的文件；请保留其创建的备份目录。在适用的 Steam 安装方式下，安装器还会配置游戏启动选项，并在还原时恢复原有设置。

## 主要功能

### 启动与稳定性

- 修复部分环境中游戏启动后只有进程、没有窗口的问题。
- 修复游戏已经播放声音，但主窗口无法显示的问题。
- 修复原版网络工作线程退出时的内存生命周期竞态。
- 修正部分音频输出格式与音频队列等待问题。
- 减少部分重复的引擎及图形资源操作。

### 现代分辨率与显示模式

- 解除原版菜单最高 1920×1200 的分辨率选择限制。
- 根据当前显示设备枚举可用分辨率，并支持在游戏菜单中选择、应用和保存。
- 增加三种显示模式：**Fullscreen（独占全屏）**、**Borderless（无边框窗口）** 和 **Windowed（窗口）**。
- 支持显示模式确认、取消与超时回退。
- 优化超宽屏暂停菜单、地图显示及部分加载界面。
- 修复部分标志、界面元素及加载动画横向拉伸的问题。
- 改善窗口模式下的鼠标指针和画面比例处理。

### 现代硬件适配

- 改善显卡、显存与显示输出设备识别。
- 根据当前显示输出选择图形适配器。
- 使用 D3D9Ex 兼容桥接处理部分原版 Direct3D 9 设备操作。
- 优化部分资源查询、纹理绑定和设备操作。

### 高帧率与游戏机制

- 调整原版渲染派发及模拟更新中的时间门槛。
- 保留原版计时、暂停和慢动作机制。
- 改善不同模拟更新频率下的刚体运动一致性。
- 修复高帧率下坦克、装甲车和直升机劫持操作异常困难的问题。
- 改善劫持操作的有效按键响应。
- 修复高帧率下鼠标微小位移丢失造成的能力轮盘切换困难。
- 改善部分特殊能力的动作计时。

## 性能诊断

补丁包含本地性能诊断功能。在符合条件的 64 位 Windows 环境中，新安装时会自动配置采集组件，随游戏记录帧时间、CPU/GPU 样本及部分引擎事件。

诊断日志目录：`%LOCALAPPDATA%\PrototypeModern\Performance`

日志按文件设置大小上限。反馈问题时可以提供相关日志，分享前请先检查其中是否包含个人信息。

## 从源码构建

仓库提供兼容模块、安装器、构建工具，以及构建安装器所需的 Python 嵌入式运行环境。

**构建环境：** Windows、Visual Studio C++ x86 构建工具（默认 MSVC 14.44.35207）、Windows SDK（默认 10.0.26100.0）、.NET Framework 4.x C# 编译器及 PowerShell。

在仓库根目录运行：

```powershell
.\build.ps1
```

构建结果位于 `build/`：

- `PrototypeModern.dll`：核心兼容模块
- `PrototypeModern_Start.exe`：启动辅助程序
- `Prototype_Modern_Compatibility_CN_Setup.exe`：中文安装器
- `Prototype_Modern_Compatibility_EN_Setup.exe`：英文安装器

### 仓库目录

| 路径 | 内容 |
| --- | --- |
| `src/` | C++ 兼容模块、显示、输入、计时与游戏机制修复 |
| `installer/` | 图形安装器、备份还原及 Steam 启动集成 |
| `tools/` | 构建辅助、资源生成、性能诊断与检查工具 |
| `vendor/` | Python 嵌入式运行环境 |
| `build.ps1` | Windows 构建脚本 |

## 注意事项与反馈

**当前版本为预览版，尚未完成全面测试，可能仍存在未知问题。**

如遇到闪退、画面异常、操作问题或其他错误，欢迎通过 [GitHub Issues](https://github.com/OBLPlife/Prototype-Modern-Compatibility/issues) 反馈。
