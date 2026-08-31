# Perfacet 架构图（Archify）

交互式 HTML，用浏览器直接打开。两张**运行时 / 进程组装**图的节点带 `SRC n` 徽章：点击可跳到 GitHub 上对应实现（提交 `004495c`）。

| 图 | 打开 | 讲什么 |
|---|---|---|
| [运行时架构](runtime.html) | 请求主路径 + 源码证据（28 处） | Agent → HttpMcp → Pipeline → 上游 |
| [进程组装](composition.html) | Gateway / netlib / Catalog | CLI 启动、IO loop、worker、探活 |
| [tools/call 时序](tools-call.html) | 一次成功调用 | 认证 → 切面 → InFlight → Permit → 上游 |
| [方法分流](request-route.html) | 工作流 | discover/list/call/tasks 与拒绝点 |
| [Call 生命周期](call-lifecycle.html) | 状态机 | Permit、升格 Task、重试、取消 |
| [目录切面](catalog-facet.html) | 数据流 | YAML → Catalog → Index → FacetView |

Viewer：`/` 搜索节点，`R` 探路径，点击节点看 Semantic Passport（含源码链接）。源码钉在 [Cabmy/perfacet@004495c](https://github.com/Cabmy/perfacet/tree/004495c5f2ac7188aa4a13011c1ddd4c6c78e0c1)。
