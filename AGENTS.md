# ListenFree Desktop：Codex 工作入口

本仓库用于将 LX Music Desktop 的能力重新设计为面向 Windows 11、兼容 Windows 10 的 Qt Quick/QML 桌面音乐应用。

## 每次开始工作前必须执行

1. 完整阅读 `docs/design/README.md`。
2. 按索引顺序阅读其中标记为“必读”的设计文档。
3. 检查 `docs/design/01-decisions.md`：只有状态为“已确认”的条目才是约束。
4. 检查 `docs/design/02-questionnaire.md`：不得擅自替用户回答尚未决定且会改变架构的问题。
5. 只有 `docs/design/99-master-plan.md` 状态变为“已批准”后，才可开始批量搭建和迁移。

## 长期约束

- Windows 11 是首要开发和验收平台；Windows 10 是后续兼容平台。
- UI 使用 Qt Quick/QML，并保留流畅动画能力。
- 降低内存、启动时间和后台开销是核心目标，性能结论必须由可复现基准支持。
- 旧版 LX Music 自定义 JS 音源插件必须 100% 兼容；兼容性必须通过参考脚本契约测试证明。
- UI 不直接访问数据库、网络实现、音频解码器或插件运行时。
- 音频 PCM 等高频大数据不得经过 QML 或通用 JSON IPC。
- 优先复用成熟开源组件，但正式引入前必须记录来源、版本、维护状态、修改范围和许可证。
- 不得直接整包复制旧 Electron 项目；按批准后的服务边界迁移可验证的行为和数据。
- 架构变化应先更新设计文档和决策记录，再实施代码。

## 给后续 Codex 的启动提示

> 你正在维护 ListenFree Desktop。不要先写代码。请先完整阅读 `AGENTS.md` 与 `docs/design/README.md` 指定的全部必读文档，汇总已确认决策、未决问题和当前阶段，然后只执行总纲允许的工作。如果总纲尚未批准，继续问答和技术验证，不要自行定案。

