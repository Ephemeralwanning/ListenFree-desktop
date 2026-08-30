# 00 — 项目章程

## 项目身份

- 项目名：ListenFree Desktop
- 本地目录：`F:\player\lx-music-desktop-master\ListenFree-desktop`
- 旧项目参考目录：`F:\player\lx-music-desktop-master\lx-music-desktop-master`
- GitHub：`https://github.com/Tabris-Ayanami/ListenFree-desktop`
- 当前仓库：私有

## 总目标

在不受旧 Electron 实现约束的前提下，重新设计一个 Qt 技术栈的桌面音乐应用。项目优先降低常驻内存、启动开销、后台 CPU 和不必要的数据复制，同时保留具有品质感的前端动画、逐字歌词和现代桌面交互。

项目将大量借鉴或组合成熟开源项目，以缩短开发周期并提高可靠性；在正式集成前仍需进行维护状态、接口边界和许可证审计。

## 已确认的产品边界

1. 首要平台是 Windows 11，本地开发环境即 Windows 11。
2. 只要求 Windows 10/11；第一阶段先保证 Windows 11，随后验证 Windows 10。
3. 前端使用 Qt Quick/QML。
4. 应用逻辑至少分为：前端交互、音源 API 插件系统、后端数据库与播放器。
5. 旧版 LX Music 自定义 JS 音源插件必须 100% 兼容，否则项目失去核心意义。
6. 网易云 API 与 Apple Music 风格歌词允许寻找原生替代方案，不要求保留原库实现。
7. 可以抛开旧项目的内部架构与历史实现；迁移的是必要行为、数据与兼容契约。
8. 项目计划主要由 AI 自动化实施，设计文档必须足够明确、可验证、可分阶段执行。
9. 播放器首个正式目标为 B 级：在基础播放之上实现 Gapless、Crossfade、ReplayGain、EQ 与高解析音频；若关键依赖接入明显拖慢首个可用版本，可暂降为 A 级，稳定后恢复 B 级，最终再演进到 C 级发烧能力。
10. 前端以 Figma 为视觉设计真源，并优先使用 Qt 官方 Figma to Qt 工作流导入 QML；生成物不直接承载业务逻辑。
11. 主应用后端使用 C++20 + Qt；Rust 不作为基础技术栈，只在出现不可替代的成熟模块时通过窄接口接入。
12. 不迁移旧应用设置、历史、缓存、歌单数据库或插件配置；旧项目目录只作为行为与实现细节参考。本地音乐文件由新项目重新扫描建库，不复制或改写原音频文件。
13. 网易云首版需要登录、每日推荐和“我的歌单”；排行榜、搜索和歌单是在线服务数据，不是 Qt 自带功能。
14. 发布渠道使用 GitHub Releases，首版只提供 Windows x64 `Setup.exe` 安装版，不提供便携 ZIP，也不实现应用内自动更新。

## 核心质量属性

优先级从高到低暂定如下，最终顺序仍待用户确认：

1. JS 音源兼容性与可用性。
2. 播放可靠性和数据安全。
3. 常驻内存与启动性能。
4. UI 流畅度与动画品质。
5. 可维护性、可测试性和 AI 可执行性。
6. Windows 10 兼容性。

## 当前候选系统边界（未批准）

```text
Figma Design Source
        │ Figma to Qt（可再生成的 UI 资产）
        ▼
Qt Quick / QML UI
        │ Qt Models / Signals
        ▼
Qt Application Facade（C++20 / Qt）
        ├── Windows 宿主能力
        ├── 数据模型与业务编排
        └── 可替换的播放接口

Native Services
        ├── SQLite 音乐库
        ├── 播放与下载
        └── 在线服务适配器

SourceHost 独立进程
        ├── Node/V8 兼容运行时
        ├── globalThis.lx 兼容层
        └── 后续可选 QuickJS 运行时
```

该结构只是讨论基线，不等于最终技术选型。

## 明确排除

- 为继续使用 AMLL DOM 组件而把完整 QWebEngine/Chromium 重新放回主 UI。
- 让 QML 直接执行 SQL、控制解码器或处理音频 PCM 数据。
- 在没有参考脚本测试集的情况下宣称 JS 音源“100% 兼容”。
- 在没有同机基准的情况下宣称某语言或框架必然更省内存。
- 直接修改 Figma to Qt 的生成文件并把业务逻辑写入其中，导致下一次导出无法安全覆盖。
- 为采用 Rust 而引入 Beta 桥接层；只有窄接口依赖存在明确、可测量收益时才允许例外。
