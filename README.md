# ListenFree

Windows 10/11 x64 原生音乐播放器，使用 Qt Quick、Qmmp、QuickJS-ng 和 TagLib。
安装版及便携版见 [Releases](https://github.com/Tabris-Ayanami/ListenFree-desktop/releases)。运行成品无需安装 Qt、Node.js 或 Python。

## 构建输入

- Qt 6.11.2 MinGW x64，包含 Quick、Multimedia、ShaderTools、Sql 和 Test；MinGW 13.1、CMake 3.28+、Ninja。
- 使用根目录 vcpkg.json 固定版本安装 QuickJS-ng、TagLib 及 zlib，依赖前缀为 .vcpkg_installed/x64-mingw-dynamic。
- 构建 Qmmp 2.4.1 及 FFmpeg、libcurl 等依赖。先应用 patches/qmmp/2.4.1/0001 和 0003，再按编号应用 patches/qmmp/0004 至 0020；不应用已撤销的 0002。
- Qmmp 的 FFmpeg、HTTP、WASAPI、Crossfade 插件及 libqmmpui 需与补丁后的 libqmmp 一起构建。Qt Multimedia 修复脚本为 scripts/build-patched-qtmultimedia.ps1。

第三方源码、SDK、依赖安装目录和二进制不纳入 Git。默认外部依赖布局如下，也可通过 CMake cache 参数覆盖：

```text
../_vendor/
  qmmp-2.4.1/                 # 已应用补丁的源码
  qmmp-stage-qt/              # libqmmp.dll、导入库和 Input/Output/Transports/Effect
  qmmp-build-qt/src/qmmpui/   # libqmmpui.dll 和导入库
  qmmp-build-qt/src/plugins/Transports/http/  # 新版 http.dll
  qmmp-vcpkg-installed-qt/x64-mingw-dynamic/ # FFmpeg/libcurl 等头文件、库和运行库
```

## 编译与部署

在 PowerShell 中执行，可根据 SDK 位置调整参数：

```powershell
./scripts/windows/package-portable.ps1 -QtRoot 'F:/QT/6.11.2/mingw_64' -QtToolsRoot 'F:/QT/Tools' -VendorRoot '../_vendor'
```

脚本通过 CMake 构建 Release，部署到 dist/ListenFree-Portable 并做隔离启动检查。重新部署保留该目录的个人 data。CMakePresets.json 提供当前工具链的 portable 预设，可用 CMakeUserPresets.json 覆盖本机路径。

安装 Inno Setup 6 和 7-Zip 后生成干净的安装版与便携 ZIP：

```powershell
./scripts/windows/package-release.ps1 -Version 0.3.2
```

只生成便携 ZIP 时追加 `-PortableOnly`，无需安装 Inno Setup。0.3.2 的本地整合内容见 [更新说明](packaging/release-notes-0.3.2.txt)。

使用已经验收的运行库目录可传入 `-RuntimeDirectory <目录>`；追加 `-SkipChecksums` 可只生成安装包和便携 ZIP，不生成校验文件。

发行脚本只收集程序、运行库、使用说明和许可，排除个人数据与用户音源；输出目录必须是尚未生成过的版本目录，也可指定 OutputDirectory。

##页面样式
<img width="1599" height="1064" alt="屏幕截图 2026-09-14 223758" src="https://github.com/user-attachments/assets/43368130-ad05-42b8-bece-bd64ffb560d3" />
<img width="1599" height="1064" alt="屏幕截图 2026-09-14 223742" src="https://github.com/user-attachments/assets/13e8b7aa-8c05-4bf8-8363-b178deaf20e4" />

<img width="1599" height="1064" alt="屏幕截图 2026-09-14 223737" src="https://github.com/user-attachments/assets/b216891c-4633-48a8-be52-014b6e3437f9" />

<img width="1599" height="1064" alt="屏幕截图 2026-09-14 223728" src="https://github.com/user-attachments/assets/f378a956-d530-4ef4-8daf-dcb61616f5d8" />
<img width="1599" height="1064" alt="屏幕截图 2026-09-14 223717" src="https://github.com/user-attachments/assets/2125e502-2304-4652-b453-5d31210e7b36" />
<img width="1599" height="1064" alt="屏幕截图 2026-09-14 223642" src="https://github.com/user-attachments/assets/1ed30d86-36f5-4fa5-90b0-905e4089a490" />


src/、music_player_desktop/、ui/ 是应用与资源；tests/ 和 tools/ 保留构建引用的验证代码；patches/、scripts/、packaging/ 和 licenses/ 为依赖修改及打包输入。个人文档、开发规划、生成物和测试结果不进入仓库。组件归属与许可见 licenses/THIRD-PARTY-NOTICES.txt。
