# 酷狗音乐 8 SMTC 插件

[![构建与发布](https://github.com/AshenAshes/kugou8-smtc-plugin/actions/workflows/build.yml/badge.svg)](https://github.com/AshenAshes/kugou8-smtc-plugin/actions/workflows/build.yml)

这是一个专用于酷狗音乐 `8.3.97.21592` 的 32 位进程内插件。插件随酷狗自动加载，将当前歌曲、歌手、封面和播放状态发布到 Windows System Media Transport Controls（SMTC），供 FlowTrak 等程序读取，并支持通过 SMTC 播放、暂停、切换上一首和下一首。

本项目不修改酷狗可执行文件，也不包含或分发酷狗及 Windows 的原始二进制文件。

## 功能

- 发布实时歌名、歌手、播放和暂停状态。
- 发布酷狗本地缓存中的歌曲封面。
- 响应 SMTC 的播放、暂停、上一首和下一首命令。
- 由酷狗主 UI 线程执行原生按钮回调，避免从插件工作线程直接操作界面对象。
- 多个酷狗进程通过共享内存协作，仅由播放进程创建 SMTC 会话。
- 过滤切歌期间的空文本、缓冲提示和其他中间状态。
- 缓存 WASAPI 音频会话并限制全进程内存扫描频率。
- 日志达到约 2 MiB 后自动轮转。

## 兼容性

仅支持：

- 酷狗音乐：`8.3.97.21592`
- `KuGou.exe` SHA-256：`0BD20B6497B75A5DD2F560D7A4E6D82D4EFE2689E6A763965964A48718B5CBB2`
- Windows 10 或 Windows 11
- 32 位酷狗进程

插件使用了该版本酷狗的固定内部结构和函数地址。其他版本即使能够启动，也可能出现控制失效、界面异常或崩溃，请勿混用。

## 下载

从仓库的 [Releases](https://github.com/AshenAshes/kugou8-smtc-plugin/releases) 页面下载 `version.dll`。

Release 的自定义附件只有插件本体 `version.dll`。GitHub 自动显示的 Source code ZIP/TAR 是对应版本的源码归档，不是安装包。

## 安装

1. 完全退出酷狗，包括系统托盘中的酷狗进程。
2. 将下载的同一个 `version.dll` 分别复制到以下两个目录：

   ```text
   C:\Program Files (x86)\KuGou\KGMusic
   C:\Program Files (x86)\KuGou\KGMusic\8.3.97.21592
   ```

3. 启动酷狗并播放音乐，然后在 Windows 媒体面板或 FlowTrak 中检查歌曲信息。

这里的 `version.dll` 是代理 DLL：Windows 会优先从酷狗目录加载它，插件再通过系统目录的绝对路径加载并转发到 Windows 原版 `version.dll`。不需要复制或修改系统 DLL，也不再需要 `version_original.dll`。

如果上述目录原本存在不是本插件的同名文件，请先备份，不要直接覆盖。插件本体的文件说明为 `KuGou SMTC Plugin proxy`。

## 卸载

1. 完全退出酷狗及其托盘进程。
2. 从上述两个目录删除插件的 `version.dll`。
3. 如果安装前备份过其他同名文件，将备份恢复到原位置。

不要删除或修改 `C:\Windows\SysWOW64\version.dll`。

## 日志与排查

日志位置：

```text
%APPDATA%\KuGou8\SmtcPlugin.log
```

通常对应：

```text
C:\Users\你的用户名\AppData\Roaming\KuGou8\SmtcPlugin.log
```

上一份轮转日志为同目录下的 `SmtcPlugin.log.old`。

正常运行时通常可以看到：

```text
Process role: SMTC publisher
Process role: metadata provider
SMTC session initialized
SMTC metadata updated: 歌手 - 歌名
SMTC cover updated
SMTC status: Playing
SMTC control queued to KuGou UI thread
SMTC control executed: Play/Pause/Previous/Next
```

暂停后保留最后一首歌曲的标题属于正常行为，播放状态会单独变为 `Paused`。彻底关闭歌曲或酷狗后，会话才会停止或消失。

## 从源码构建

需要：

- Visual Studio 2022 Build Tools
- “使用 C++ 的桌面开发”组件和 Windows 10/11 SDK
- CMake
- PowerShell 7

在仓库根目录执行：

```powershell
./build.ps1
./test.ps1
```

生成的插件位于：

```text
build\Win32\Release\version.dll
```

自动化测试只验证 DLL 转发、元数据解析、共享内存、运行策略、封面处理、SMTC 主机和控制策略。与真实酷狗界面的播放控制、切歌、封面及长期稳定性仍需人工验证。

## 自动构建与发布

GitHub Actions 会在以下情况下执行 Win32 Release 构建和全部测试：

- 推送到 `main` 分支。
- 向 `main` 创建 Pull Request。
- 手动运行工作流。

发布新版本前，先修改 `resource.rc` 中的文件版本，然后创建并推送对应标签。例如 DLL 版本为 `0.4.5.0` 时：

```powershell
git tag v0.4.5
git push origin v0.4.5
```

工作流会校验标签与 DLL 版本是否一致，测试通过后自动创建 GitHub Release，并且只上传 `version.dll` 作为自定义附件。

## 项目结构

```text
src/                         插件源码
tests/                       自动化测试和人工诊断工具源码
third_party/libwebp/         固定版本的 libwebp 源码
.github/workflows/build.yml  GitHub Actions 工作流
build.ps1                    Win32 Release 构建脚本
test.ps1                     测试入口
resource.rc                  DLL 版本资源
version.def                  version.dll 导出表
```

## 第三方组件

封面解码使用 Google libwebp，其许可文本位于 [`third_party/libwebp/COPYING`](third_party/libwebp/COPYING)。

本仓库不会上传酷狗程序、酷狗资源文件、Windows 系统 DLL、个人日志或本机构建产物。
