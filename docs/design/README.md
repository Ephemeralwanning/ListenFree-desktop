# ListenFree Desktop 设计文档中心

本目录是项目设计的唯一真源（single source of truth）。后续 Codex、其他 AI Agent 和人工开发者都必须先阅读这里，再修改架构或代码。

## 当前阶段

- 阶段：Backend Bootstrap 实施
- 总纲状态：已批准
- 允许的工作：按总纲进行分阶段实现、测试、性能测量和文档维护
- 暂不允许：整包复制旧项目、修改 Pixso/Figma 生成文件中的业务逻辑
- 最后更新：2026-08-30

## 必读顺序

1. `00-project-charter.md`：目标、范围与不可妥协约束。
2. `01-decisions.md`：已确认决策、候选方案及被否决方案。
3. `02-questionnaire.md`：问答进度与下一批待确认问题。
4. `03-figma-and-rust-bridge.md`：Figma → QML 边界及 Rust → Qt 选型记录。
5. `04-mvp-compatibility-and-release.md`：MVP、格式、旧音源契约、在线服务、性能和发布候选。
6. `05-backend-bootstrap-architecture.md`：Backend Bootstrap 的功能区、依赖方向、核心接口与进程边界。
7. `90-codex-execution-guide.md`：AI 自动化实施纪律。
8. `99-master-plan.md`：汇总实施总纲；当前等待用户批准。

## 文档状态规范

- **已确认**：用户明确认可，可作为实现约束。
- **候选**：值得验证，但不得直接据此展开正式实现。
- **待确认**：必须继续问答或以 PoC/基准提供证据。
- **已否决**：除非产生新证据，否则不得重新采用。
- **已批准**：总纲整体获得用户确认，可以进入实施阶段。

## 更新规则

1. 每轮关键问答后更新 `01-decisions.md` 和 `02-questionnaire.md`。
2. 技术选型必须记录选择理由、替代项、验证方法和回退方案。
3. 性能目标必须包含场景、设备、测量指标和通过阈值。
4. 引入开源项目时，新增依赖清单；不得仅凭 Star 数或 README 宣称可靠。
5. 总纲批准后，每个实施阶段都应有输入、输出、测试、性能门禁和回滚点。
