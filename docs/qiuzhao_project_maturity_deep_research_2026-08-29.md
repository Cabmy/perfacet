# 秋招项目成熟度 / 完整性 — Deep Research Report

> Conducted: 2026-08-29 | Mode: Standard（中文互联网社区面经 + 面试官帖 + 工程压测原文；Wave 2 并入反面意见与课设分界硬证据）
> Sources analyzed: 73 | Credibility threshold: 纳入 Tier 3 以上；主张锚定在 Tier 1/2
> Evidence confidence: 共识项 Verified / Strong；「纯手写网络库还值不值钱」为 Contested（陈硕原文把争议向「网络库服务业务」一侧压实）
> 对照仓库：`/home/cabmy/Desktop/cppServerLearning/perfacet`（Perfacet：自研 epoll `netlib` + Multi-Agent MCP Tool Gateway）

## Executive Summary

中文互联网几乎不用「项目成熟度」这个精确词，但 2022–2026 秋招讨论里，对「什么样的项目能写进简历、扛得住拷打」有稳定共识：**不是功能清单有多长，而是（1）能一键跑起来、（2）有自己的设计决策、（3）能讲清痛点—方案—取舍—数据、（4）经得起「为什么不直接用现成库」**。[4][5][18][45]

对 C++ 高性能网络/服务器类项目，面试官默认会按 muduo/Reactor 八股深挖；**把「仿 muduo 网络库」单独当完整项目，2025–2026 已被多处判定为同质化、难过大厂初筛**，除非上面叠了可讲的业务/治理场景，并且压测数字带环境、方法和对照。[23][40][41][45][56][57] 陈硕本人把 muduo 定位为**写公司内部专用业务 server 的底座，不是用来写通用 httpd、也不是日常去跟 Sockets API 较劲**。[53][54] Perfacet 的产品主语是「按身份切面的 MCP 网关」，规格里写明「epoll 是工程便利，不是产品定义」——这恰好踩中作者原意和秋招「网络库当底座、业务当项目」的正确口径，也避开「又一个 echo/muduo 克隆」的坑。

课设/demo 与「能写简历」的分界线是：Happy Path 本机能跑 vs 有边界条件、测试、文档、可演示剧本、诚实的性能数据和失败模式。[2][21][40] 性能方面，严肃口径来自陈硕：同环境对照、ping-pong 吞吐、事件处理效率、延迟、HTTP 长连接 QPS；**裸写「万级 QPS / 百万连接」且说不清压测配置，会被当成没做过**。[37][36][45]

---

## 1. 中文互联网对「秋招项目成熟度」的共识标准

**Evidence confidence:** Verified（独立来源 ≥3：大厂面试官帖、牛客高频问题汇总、2026 简历拆解）

网上真正高频的近义词是：**课设感 / 完成度 / 工程化 / 产品化 / 经不经得起深挖**，不是「成熟度」本身。[2][18][21][40]

### 1.1 完整性：不是模块堆满，而是闭环可交付

共识把「完整」拆成几层，缺一层就会被打成 demo：

| 层 | 网上怎么说 | 代表来源 |
|---|---|---|
| 能运行 | clone 下来按 README 能装、能编、能跑；「localhost Happy Path 拿高分」在企业面会被一眼看穿 | [2][32] |
| 能演示 | 有脚本/剧本覆盖主路径和至少一条失败路径；「项目怎么没有尝试部署上线」是高频题 | [18] |
| 能交代权责 | 团队人数、独立模块、是自研还是跟教程；整套复杂架构全算个人会被质疑抄开源 | [21][43] |
| 能对账 | 业务痛点 → 技术方案 → 量化结果，三段缺一会被当成名词堆砌 | [21][43][3] |
| 能防守 | 超时、限流、熔断、幂等、降级；只写正向功能 = 没有线上思维 | [21][43][2] |

蚂蚁集团技术专家（2022）把面试官目的收成四条：技术是否用过且懂原理、角色是否超出执行、遇难题有没有自己的方案、能不能在短时间把背景/设计/挑战讲清楚。[19]

### 1.2 可运行 + 文档 + 测试：工程素养门禁，不是加分彩蛋

- **README**：项目目标、如何构建/运行/测、目录结构；GitHub「放了就要维护」，空仓库或半年断更会减分。[4][32]
- **测试**：校招不要求覆盖率百分百，但「有单测 / 能跑测试命令」被当作正式项目的形状；C++ 面试官还会追问你怎么调试（GDB、看监听端口、tcpdump）。[45][2]
- **CI/容器**：跨国企业向文章把 Dockerfile + GitHub Actions 列为生产级四要素之一（鉴权、并发、可观测、CI/CD）。[2] 国内校招对此更宽容，但有则加分、没有也不至于一票否决。
- **部署**：高频原话是「项目怎么没有尝试部署上线呢？」——哪怕是本机 docker / 一台云主机 / demo 脚本，也比纯 IDE 运行强。[18]

### 1.3 性能数据：要「可复核的对比」，不要「广告数字」

简小派高并发叙事模板明确警告：日常并无高峰、无监控、无指标时，**不要硬包装成高并发亮点**，改写稳定性/质量。[29 片段] 蒸汽教育给出的「能写」句式是：工具（JMeter/wrk）+ 并发 + P99 + 优化前后对比，例如「5000 QPS 下 P99 从 1.2s 到 50ms」。[2]

C++ 大厂面试官给过反面教材：简历写「高性能、上万 QPS」，追问压测方法和机器配置后，候选人承认其实是「上万连接不是上万 QPS」——项目起了反作用，面试不通过。[45]

### 1.4 业务场景 + 技术深度 + 可讲故事

三条同时出现才算「成熟」，只占一条会偏科：

1. **业务场景**：这个东西解决谁的什么问题？为什么不直接用 nginx / gRPC / 微信群？答不上来 = demo。[40][21]
2. **技术深度**：细扣一两个模块，能画架构、能说选型、能说备选方案为什么弃用。[18][19] 校招对量级宽容，对「写了就要能说到第二层」不宽容。[4]
3. **故事（STAR）**：背景、任务、行动、结果；量化贡献；「负责了 XX」等于没写。[3][19]

深度 vs 广度：几乎一边倒——**宁可一个项目讲透，不要三个 CRUD 并列**。[18][4][21] 「最好选一个最有心得的模块，把难点和为什么这么选讲清楚；面试官听的是思考过程。」[7]

### 1.5 2026 额外风向：实习权重大于同质化个人项目；Agent 项目会被当业务项目拷打

2026 牛客高赞帖认为：商城/RAG/多 Agent 演示高度同质，**写到位的实习 > 再完善的个人项目**；个人项目只补实习没覆盖的技术短板。[43] Agent 项目会被查：量化产出、权责、是否真上线、MCP/RAG 名词是否自洽、有没有工程兜底。[21] 这对 Perfacet 是双刃剑——题材对口，但「MCP 网关」四个字会触发 Agent 线深挖，不能只会讲 epoll。

---

## 2. C++ 网络库 / 高性能服务器：面试官看什么、踩什么坑、怎样才算能写进简历

**Evidence confidence:** Strong（C++ 面试官长文 + 多份 2025–2026 C++ 面经 + 陈硕压测原文；「还值不值得当主项目」存在争议）

### 2.1 面试官通常问什么（已在面经中反复出现）

**网络库/服务器本体（一旦简历出现 muduo / epoll / 自研网络库就会问）：**

- One Loop Per Thread / 主从 Reactor，连接如何分发 [9][10][12]
- 为什么 Channel 封装 fd+事件+回调；EventLoop 与 Channel 的线程约束（`assertInLoopThread` / eventfd 唤醒）[9][27]
- Buffer：`readv` + 栈上额外空间、prepend、扩容 [35]
- TcpConnection 关闭时如何把剩余数据发完再析构（channel `tie` / 延迟注销）[27]
- epoll LT vs ET、粘包/半包、定时器、心跳与死链、优雅关闭、大量 CLOSE_WAIT/TIME_WAIT [28]
- IO 线程 vs 业务线程：耗时逻辑不能堵 loop [10][11]
- 「为什么高性能？」——答「因为参考了 muduo，网上都说它快」会被判成搬运工 [23]

**一旦自称「高性能服务器 / 上万 QPS」额外拷打：**

- 压测工具、机器核数内存、是连接数还是 QPS、P99、瓶颈在用户态还是内核 [45]
- HTTP 项目：Content-Length、GET/POST 解包、流式 TCP 如何定界——协议都说不清，服务器项目直接翻车 [45]
- 程序结构自洽：accept 后把 socket 丢进线程池、线程里再挂 epoll，会把池子占满——面试官会拆穿这种课设结构 [45]

**2025–2026 C++ 岗面经实例：** 安恒一面问「网络通信没用自己写而是 muduo？那介绍你实现的 epoll 服务器」以及「为什么把 connect 封装成 Channel」[9]；淘天问 RPC 异步处理与 muduo 线程模型 [10]；腾讯云智二面在候选人「只会用 muduo」时继续追 One Loop Per Thread [11]；优必选问主从 Reactor 相对纯线程池的优势 [12]。

### 2.2 怎样才算「能写进简历」

C++ 面试官原话口径（应重点采用）：[45]

- **应届生项目不是必需的，基础更重要**；但**写了就会按你写的深度来挖**。
- **真实**：不反对把开源/教程项目写上，但核心原理必须是自己的；Web 服务器却说不清 HTTP，项目起反作用。
- **合理**：万级 QPS 必须带硬件配置；写「调试过」就要会调试命令。
- **收窄技术栈**：只列 1–3 个你想被问的点，乱堆 Redis/Kafka 是给自己挖坑。

技术栈文章的补充（带课培色彩，但判断与面经一致）：**不要把网络框架当独立完整项目**；网络库适合当「原理课」，完整项目应是用它做出的服务。[23] 代码随想录公开承认「不少录友自己做一个 muduo 写到简历上」，并卖面试题和三种简历写法——这本身说明该题材已经高度同质化。[27]

南邮 C++ 选手「RPC + 群聊」颗粒无收的复盘更可操作：项目类型没问题，缺的是 **背景意义 + 性能测试 + 与 brpc/gRPC 的方案对比**；只在自己 PC 上能编译，场景题必挂。[40]

### 2.3 典型坑（按杀伤力）

1. **QPS / 连接数偷换概念**，经不起压测追问。[45]
2. **纯复刻 muduo / 代码随想录 / B 站**，被问「和原版比你改了什么、好在哪」答不出。[25][41] 牛客评论区有人直问：「学的星球里现成的 muduo，会被嫌弃吗？」回复是「拷打你项目的时候怎么作弊？」[25]
3. **架构自相矛盾**（线程池 + 每连接阻塞；自称 Proactor 却讲不清和 Reactor 的区别）。[45]
4. **没有上层协议/业务**：echo 服务器在 2025 后被「手写 RPC 不值钱」帖子点名；多数自写 RPC「socket + 注册中心，没优化没高可用」。[41]
5. **轮子项目懂不懂两极分化（Contested）**：有人警告「手写 Spring/RPC 就是玩死自己」；评论里另一拨人说秋招几乎只问轮子、业务商城没人问。共识收敛为：**搞懂了再写，写了就要能从协议问到 epoll 再问到和工业实现的差距**。[39]
6. **把 in-process 微基准当成网关 QPS**（与 Perfacet `bench/README.md` 的自我警告同构）。

### 2.4 对照 Perfacet：什么写法安全，什么写法危险

| 简历主语 | 网上预期 | 风险 |
|---|---|---|
| 「自研高性能网络库，仿 muduo」 | 立刻进入 Channel/EventLoop/Buffer/QPS 拷打，且与千篇一律克隆撞车 | 高：规格已声明单 loop、上游走阻塞 httplib，和「多 Reactor 打满万 QPS」叙事冲突 |
| 「C++ 实现的 Multi-Agent MCP 网关：按身份切面、Grant、Governor、在途去重、熔断、OTel」 | 2025–2026 帖子明确点名 MCP/Agent+后端是加分题材 [41][21] | 中：会被当 Agent 项目挖协议、权限、失败模式，而不是只挖 epoll |
| 「netlib 是 agent 侧字节流底座，产品不变量在 Policy/Pipeline」 | 与「网络库当底座、业务当项目」[23][40] 完全同向 | 低：需要能讲清为什么 IO loop 禁止 yaml/httplib/OTLP（规格硬约束 7） |

**能写进简历的最低条（综合 [40][45][21]）：** 别人按 README 能跑 `demo.sh`；能画 Pipeline；能解释 2–3 个自己的不变量（例如在途合流不放大、secret 上游对低权限不可见、`-32021` 与 Tasks）；压测只写 HTTP 路径且带并发、对照直连 mock、机器环境；诚实列出单实例 SPOF 和 P0 未完成项，不要假装已验收。

---

## 3. 成熟项目 vs 课程作业 / demo 的分界线（清单）

**Evidence confidence:** Verified（课设感重构指南、Agent 简历拆解、C++ 面试官、南邮复盘相互独立）

下列每条：**左侧 = 仍是课设/demo；右侧 = 网上认为「可以当秋招项目」**。不必全绿，但左侧超过一半会在初筛被打成作业。

### 3.1 交付形态

- [ ] 只有 IDE 里 F5 / 只有截图 → [ ] README 三命令内可构建运行，有 `demo.sh` 或等价剧本
- [ ] 仅 Happy Path → [ ] 至少覆盖鉴权失败、限流/熔断、上游宕机、协议版本不对
- [ ] 无部署信息 → [ ] 写明单机如何起、依赖什么、如何停（即便不上公网）
- [ ] 无版本/协议约束 → [ ] 协议版本、不兼容策略写进规格（Perfacet 的「只认 2026-07-28」属于加分项，但要能讲为什么）

### 3.2 工程化

- [ ] 无测试或测试即 main 里 cout → [ ] 分层单测，CI 或至少文档里一条 `ctest`/`./tests`
- [ ] 无日志或满地 printf → [ ] 结构化审计 / trace_id 能对上（可观测是生产级四要素之一 [2]）
- [ ] 配置写死在代码 → [ ] YAML/CLI，改档不改 C++
- [ ] 无边界：超卖、超时、重试风暴没想过 → [ ] 有明确失败语义和单测锁住

### 3.3 智力含量（面试官真正买的）

- [ ] 功能列表 = 技术栈列表 → [ ] 每条亮点能回答「为什么不选现成方案」
- [ ] 复制开源改 README → [ ] 有可指认的自研点（调度、权限模型、去重、治理）[44]
- [ ] 「高性能」无方法 → [ ] 有对照实验和限制条件 [37][45]
- [ ] 讲不清除了「简历缺项目」以外的存在理由 → [ ] 有目标用户（多 Agent 抢共享工具、身份切面）[40][21]

### 3.4 叙事与诚实

- [ ] 团队项目写成独立完成整套架构 → [ ] 标明范围 [21]
- [ ] 规格未做完却写成已验收 → [ ] 未完成项进「已知限制」（Perfacet `REVIEW_ACTION_ITEMS.md` 的 P0 若写进简历当已完成，属于高危）
- [ ] 深度不够就靠广度堆中间件 → [ ] 一两个模块挖到能聊 15 分钟 [18]

**一句话分界：** 课设证明「我按教程把轮子转起来了」；秋招项目证明「我为某个约束做了取舍，并且能用测试和数字防守这些取舍」。

---

## 4. 性能方面网上常见的评价口径

**Evidence confidence:** Verified（陈硕原文 + 其书摘转载）；学生简历滥用口径为 Strong

### 4.1 网络库本体：不要用「QPS」一个数打天下

陈硕的方法是行业默认参照（2010 原文仍被 2020s 教程反复抄）：[37][36]

1. **原则：** 用对方的测试方案，实现功能相同的程序，**同一软硬件对比**。不要跨机器、跨协议、跨实现完整度比倍数。
2. **Ping-pong 吞吐：** 双方都是 echo，连接建立后 16KiB 左右报文来回打，测 MiB/s；单机测是因为千兆网单连接就能打满网卡，跨机器会全是 110MiB/s 失去区分度。[37]
3. **事件处理效率（击鼓传花）：** 大量 pipe/socketpair，比的是 epoll 处理大量 fd 的开销，不是业务 QPS。[36]
4. **延迟：** 定长消息 echo 的单程微秒（相对 ZeroMQ 那组）。[36]
5. **HTTP 长连接 QPS：** 用 `ab` / `weighttp` 打内存里的 `hello world`；陈硕明确说 **muduo 与 nginx 在合适条件下都能超过 10 万 QPS，但这不说明 muduo 更适合当 httpd，只说明没犯低级错误**——因为 muduo 只实现了最基本 HTTP。[36]
6. **严肃对比还要看分布和百分位，而不是只报平均值。**[36]

学生项目常见走样：把 ping-pong 字节吞吐说成「百万 QPS」、把 nginx 对比做成「我比 nginx 快」、不写核数/是否同机/消息大小/是否 keep-alive。

### 4.2 业务/后端项目：QPS 要绑场景

校招后端通用句式：[2][3][21]

- 指标：QPS 或 TPS、平均 / P95 / P99、错误率、CPU/内存
- 必须：并发数、是否饱和、拐点（TPS 不再涨的那个并发）
- 对照：优化前/后，或直连上游 vs 经网关
- 禁止：无环境的「10w+ QPS」[29 片段]；无业务量支撑的「P99 从 500ms 到 100ms」（会被当成本地脚本）[21]

压测报告模板（工程向）还要求：挡板/mock 位置、资源曲线与 TPS 对齐、错误分类。[31]

### 4.3 对 Perfacet 的直接含义

仓库 `bench/README.md` 已经符合陈硕式诚实口径：in-process 附加延迟与 HTTP 入口分开；禁止对 gRPC 比倍数；禁止把 in-process ~3.1×10⁵ RPS 写成网关延迟；规格 5–10k 是打点不是本机实测。**网上标准支持继续保持这种写法**，并补齐：机器（WSL2 核数）、`concurrency=8, n=2000` 的 HTTP 经网关 vs 直连 mock 表、P50/P99、错误率。不要补「对比 nginx/muduo 快几倍」——协议不同（MCP Streamable HTTP + 治理），比了反而不专业。[36][37]

Governor「4 抢 3」这类语义应用 `demo.sh`/单测锁，而不是用 echo QPS 证明。

---

## 5. 可打分评估 Rubrics（建议用于评估 Perfacet 一类项目）

设计原则：权重反映「面试官会不会因此挂人」，不是「GitHub star 多不多」。校招基础题另计，本表只评**项目作为简历资产的成熟度**。总分 100。

**使用方法：** 每维打高/中/低，取该档中位分，加权求和。≥75 可作主项目；60–74 可写但须收窄表述；<60 建议降级为「学习笔记/组件」或补完再写。

### 5.1 维度、权重、高/中/低

| 维度 | 权重 | 高（满分档） | 中 | 低 |
|---|---|---|---|---|
| A. 可运行与可演示 | 12% | 新机器按 README 构建；`demo.sh` 覆盖主路径+失败路径；依赖锁定 | 能编能跑，演示靠口头或单测 | 缺步骤、环境飘、只有截图 |
| B. 文档与规格诚实度 | 10% | README + 规格 + 已知限制一致；未完成不装成已验收 | 有 README，规格与实现有小偏差 | 无文档或文档吹过实现 |
| C. 测试与回归 | 10% | 分层单测锁不变量；有泄漏/并发相关测试；一条命令跑完 | 有若干单测，主路径靠手工 | 无测试或测试不能失败 |
| D. 业务场景与存在理由 | 12% | 说得清目标用户、为何不用现成网关/nginx/直连；有差异化不变量 | 有场景但经不起「为何不用微信/gRPC」 | 为了简历而做，场景空洞 |
| E. 技术深度与取舍 | 15% | 能画架构；每项关键设计有备选和放弃理由；能从业务问到 loop 线程模型 | 能讲实现，说不清为什么 | 只能复述教程名词 |
| F. 失败模式与治理 | 12% | 鉴权、限流、熔断、重试不放大、优雅停机有设计有测试 | 有部分治理，缺口清楚 | 只有 Happy Path |
| G. 可观测与可对账 | 8% | trace / 审计 / 计数能对上同一 id；能演示排障 | 有日志或有计数，对不齐 | printf 或无 |
| H. 性能证据质量 | 10% | 方法+环境+对照+百分位+限制；数字可复现 | 有数字但缺对照或环境 | 无数字，或 QPS/连接数偷换 |
| I. 差异化（非同质化 muduo/RPC/商城） | 6% | 题材或模型明显不同于教程克隆，且能讲自己的点 | 常见题材但有改造 | 纯克隆 |
| J. 可讲故事与权责 | 5% | STAR 完整，个人边界清晰，引导面试官问自己会的点 | 能介绍，易被问到不会的栈 | 流水账或夸大 |

**加权示例：** 高=该维满分，中=55%，低=20%。  
`Score = Σ (weight_i × {1.0 | 0.55 | 0.20} × 100)`

### 5.2 一票否决（不看总分）

- 简历数字与仓库事实冲突（例如写 5–10k HTTP QPS 已达成，而 bench 写明未达成）
- 核心协议/HTTP 定界讲不清却自称写了服务器 [45]
- 把未修 P0（粘包、熔断被探活合闸、retry 放大等）写成已生产可用

### 5.3 针对 Perfacet 的评分提示（调研结论，非正式打分）

不在本报告代跑测试或改代码。仅根据仓库阅读对照网上标准：

- **偏高：** D（MCP 多 Agent 切面/治理，不是 echo）、E 的规格密度、A 的 `demo.sh`/`uv sync`+cmake、C 的测试目录形状、H 的 bench 诚实声明、I（MCP 2026-07-28 网关远比 mymuduo 稀缺）
- **看实现是否跟上规格：** F/G（`REVIEW_ACTION_ITEMS.md` 列出多条 P0：失败探活合闸、promote 后仍打上游、粘包、成功调用无审计、停机非 GOAWAY）——网上标准会把「规格很完整但 P0 未关」打成**文档诚实度中、失败模式中低**
- **必须避免的简历表述：** 以「自研网络库」做标题；用 in-process 31 万 RPS；对标 nginx 倍数；隐瞒单 loop + 阻塞 httplib 的架构上限

建议主项目标题用产品句，netlib 放在技术栈或「工程底座」一句，面试时用「为什么 loop 上不能 open grants」展示深度，而不是用「我写了 EventLoop」展示广度。

---

## 6. 出处列表（≥12）与可信度

下表「可信度」为综合档（见文末 6 维打分）。**主张请优先引用 Tier 1。** 培训/简历工具文仅作「市场上在贩卖什么叙事」的证据，不当事实锚。

### 建议精读的 12 条（对应你要求的 8–12 个有出处来源）

| # | 标题 | URL | 关键观点 | 可信度 |
|---|---|---|---|---|
| [45] | c++程序员简历中项目怎么写？避免踩坑！ | https://www.nowcoder.com/discuss/625017329121845248 | 大厂 C++ 面试官：项目非必需但写了必挖；QPS 必须带配置；协议说不清则项目起反作用 | **高** Tier 1 |
| [19] | 大厂面试项目经历都在问些什么？ | https://www.nowcoder.com/discuss/353159312304447488 | 蚂蚁技术专家：原理、角色、解题、表达；STAR；能画架构 | **高** Tier 1 |
| [37] | muduo 与 boost asio 吞吐量对比 | https://www.cnblogs.com/Solstice/archive/2010/09/04/muduo_vs_asio.html | 陈硕原文：ping-pong 方法、同机对比、不把高吞吐当设计目标 | **高** Tier 1（方法永续，数字过时） |
| [36] | muduo 性能测评：吞吐、事件效率、延迟 | https://blog.csdn.net/qq_41453285/article/details/107018098 | 书摘：vs nginx「都能 10 万 QPS」但 muduo 不是完整 httpd；要看百分位 | **高** Tier 1（转载陈硕，方法有效） |
| [21] | 面试官视角拆一份 Agent 项目简历 | https://www.nowcoder.com/discuss/898495069127200768 | 无量化、无权责、无部署=本地 demo；MCP 名词堆砌会翻车 | **中高** Tier 2（自述面试官，2026，可能有内容号痕迹） |
| [18] | 面试官怼我简历上的项目，我被问傻了 | https://www.nowcoder.com/discuss/353159424011345920 | 10 大高频：难点、选型、部署、优化点；细扣一两个模块 | **中** Tier 2 |
| [40] | 南邮 26 届 C++：RPC+群聊二面场景题屡挂 | https://www.nowcoder.com/discuss/755793806250745856 | 烂大街项目可以；缺背景+压测+对比才挂；要产品化 | **中** Tier 2（职业规划号） |
| [23] | muduo 网络库为什么高性能？ | https://jishuzhan.net/article/2036963938500411393 | 网络库不宜当独立完整项目；禁止答「因为 muduo 很快」 | **中** Tier 2 |
| [41] | 2025 简历避雷：别再沉迷手写 RPC | https://www.nowcoder.com/discuss/766666581601419264 | 手写 RPC/网络框架同质化；应用作学习素材；MCP+AI 后端加分 | **中低**（结论部分被面经印证，文末导流课程） |
| [43] | 27 秋招简历核心：实习远胜同质化项目 | https://www.nowcoder.com/discuss/900896686346694656 | 个人项目难拉开差距；量化+故障兜底+权责 | **中** Tier 2 |
| [2] | 破局「课设感」 | https://www.cnblogs.com/stemcareergroup/articles/19906201 | Happy Path≠生产级；鉴权/压测/可观测/CI 四件套 | **中** Tier 2（留学辅导，框架有用） |
| [9] | 安恒信息 C++ 开发一面 | https://www.nowcoder.com/discuss/858425164415922176 | 真实一面：muduo vs 自写 epoll、Channel 封装 | **中高** Tier 2（一手面经） |
| [27] | C++ 项目推荐：网络库（代码随想录） | https://programmercarl.com/other/project_muduo.html | 侧面证据：muduo 简历写法已产品化、可「突击背题」 | **中低**（商业星球，用于证明同质化） |
| [28] | 你遇到过哪些高质量的 C++ 面试？ | https://www.zhihu.com/tardis/bd/ans/1783988850 | 自称设计过服务器就会被问粘包、定时器、优雅关闭、死链 | **中高** Tier 2 |
| [4] | 程序员简历怎么写才能过 AI 筛选（2026） | https://www.mianlingai.com/blog/developer-resume-writing-guide-2026/ | 校招看理解深度；GitHub 要有 README 和实质代码 | **中低**（简历产品文） |

其余来源见 Bibliography。

---

## Key Findings（便于检索）

### Finding 1 — 「成熟」= 可运行闭环 + 可辩护的取舍，不是模块数量
**Evidence confidence:** Verified  
**Sources:** [2], [18], [19], [21], [40], [45]

### Finding 2 — 仿 muduo 单独当主项目，2025–2026 贬值；当底座 + 上层协议/治理仍可加分
**Evidence confidence:** Strong（主结论）/ Contested（C++ 岗仍大量问轮子）  
**Sources:** [23], [25], [27], [39], [41], [9]–[12]

### Finding 3 — 性能数字的专业标准是方法论，不是绝对值
**Evidence confidence:** Verified  
**Sources:** [37], [36], [45], [2]

### Finding 4 — Agent/MCP 题材吃香，但按业务项目标准拷打，不是按「用了新协议」给分
**Evidence confidence:** Strong  
**Sources:** [21], [41], [43], [44]

### Finding 5 — 深度碾压广度；写在简历上的每一名词都是攻击面
**Evidence confidence:** Verified  
**Sources:** [4], [7], [18], [45]

---

## Comparative Analysis

| 标准 | 课设 echo / mymuduo | 教程 RPC + 群聊 | 有治理的协议网关（Perfacet 这类） |
|---|---|---|---|
| 网上新鲜度 2026 | 很低 | 低 | 高（MCP/Agent） |
| 面试官默认问题 | Reactor 细节、QPS | 和 gRPC 比什么、场景题 | 权限模型、失败语义、协议、再顺带 loop |
| 最大翻车点 | 搬运 + 假 QPS | 未产品化、无对比 | 名词堆砌、P0 未修却声称完整 |
| 补完路径 | 加真实协议+测试+诚实 bench | 背景+压测+对比表 | 关掉 P0、demo 对账、简历用产品句 |

轮子 vs 业务的社区分裂 [39]：C++ 底层岗仍可能全程只问网络库；对这类面试，Perfacet 的 `netlib` 必须真懂，但不能让它成为简历唯一卖点。陈硕原文把分裂压向一侧：网络编程「起到支撑作用，但不处于主导地位」；程序员主要工作是事件回调里的业务逻辑。[53]

---

## Wave 2 补充（反面意见 + 课设分界硬证据）

第二波检索补上了第一波里偏软的「培训文在骂 muduo 克隆」——现在有作者原声、热门课设仓库自证、以及 C++ 圈「人手一个」的直接表述。

### 作者原意：不要把「再造一个更快的库」当工作/简历目标

陈硕《谈一谈网络编程学习经验》（2011，后收入《Linux 多线程服务端编程》附录 A）：实际项目里只用过两次 Sockets API，其余时候用封装好的网络库；「程序员的主要工作是在事件处理函数中实现业务逻辑，而不是和 Sockets API 较劲。」并声明该文可能不适合「高性能网络服务器」读者。[53]

《Muduo 设计与实现：Buffer》：设计目标是公司内部分布式程序——「用来写专用的 Sudoku server 或者游戏服务器，不是用来写通用的 httpd」；前者有业务逻辑，后者才强调通用高并发高吞吐。正确实现并投入后再按真实负载优化，优于编码阶段盲目调优。[54]

这把学生 README「echo 百万 QPS、超越 muduo」直接判成赛道错误：他们在比陈硕明确说自己**不作为设计目标**的那一类数字。[65][66]

### 「人手一个」与招人侧的恶心感

牛客后台秋招总结把「Linux 高性能服务器」列为 C++ 经典项目，同时写「高情商人手一个，低情商烂大街」。[57] SegmentFault《烂大街的项目就别写到简历上了》（自称招过实习生）：「看到不少同学还在单纯的写 webserver、rpc、muduo……面试官看的太多了，甚至看简历看到这些就犯恶心……写这些如果不是 92 选手，简历直接 pass」；把线程池/内存池当独立项目「直接无语」。夹带知识星球，结论偏绝对，但点名对象与面经一致。[56]

TinyWebServer（约 2 万 star）作者在 README 里承认：有面试官凭项目信息在公司里找到他，发现很多简历都用这个项目，但「知其然不知其所以然」。同一 README 用 Webbench 5 秒、10500 连接、约 9 万 QPS 当成绩。[55] 独立复测仓库 Turtle 在相近设定下得到约 3.8 万 QPS，说明**学生 QPS 高度依赖测法，不可当简历绝对值**。[64]

LanceNet / MyMuduo 一类 README 的典型形状：Reactor 名词全、echo 数字大、缺硬件/是否回环/消息大小/失败路径/单测；有的把「百万 QPS」和「百万并发」混用。[65][66]

### 课设分界：社区用「作业级 / 能跑 / 报菜名」，很少用「成熟度」

- 系统层+工程层才是 Demo 与项目的分界；没有指标的优化是自我感动。[61]
- 看完成度不是热闹感：完整小项目 > 摸过五个热门方向；只写能接住的东西。[60]
- 面试官四刀：真做过、判断力、工程意识、复盘；停留在「能跑」不够。[62]
- 鱼皮：最强真实性是可访问地址；禁止整仓一次提交或原样 fork；后端上不了线时文档是次优。[58]
- 无实习上岸帖：1–2 个业务 + 1 个轮子够用；每个项目最好有一个压测参数，**不求几万 QPS，但机器、方法、前后对比必须说清**；能放代码链接真实度翻倍。[63]
- 达同学（北邮/BAT）：校招项目普遍薄，不是面试官不想问，是学生讲不深所以问不多；「没有高并发」不是真问题，先讲清是用户需求还是技术问题。[59]

**对 Perfacet：** Wave 2 进一步确认——不要把 netlib 当主项目标题；用 MCP 治理当「专用业务 server」；压测坚持标 WSL2/并发/对照；TinyWebServer 式「关日志短时 Webbench」不要学。

---

## Wave 3 补充（C++ 网络库秋招口径）

第三路检索把「人人都 muduo」从培训口号落实到 2023 社区原帖和真面经。

### 2023 起公开口径：库本身不再稀缺

V2EX《准备校招面试用到的项目，要到哪种程度才算属于自己？》以「跟着网课做 C++11 实现 Muduo」为题。回复：「前几年能写个 muduo 库是稳进大厂，现在的话，人人都 muduo 库，没太大区分度了，建议还需要准备一个业务相关的。」楼主同意「这个项目确实已经没有区分度」。[68]

24 届双九、有滴滴/阿里 C++ 实习的长文：「面试官看腻了批量生产的 C++ 语言 + webserver 版本的简历」；webserver 仍是入门必做，但**普通、没有自己优化的版本不适合当秋招主项目**。[69]

### 即使用现成 muduo，也会按「你的库」来挖

商汤 C++ 实习一面（项目是 JsonRpc **使用** muduo，不是仿写）：问有没有看源码、EventLoop、自定义协议、TCP 粘包、异步调用（追问 `std::future` 仍会阻塞）、eventfd、现场手写线程池。[70] **上层有业务不能替代** Channel/粘包/析构答辩——Perfacet 的 facet/governor 同样如此。

TinyWebServer 面试题整理还问：并发量怎么测、webbench 原理、测试时遇到什么问题、「你的项目解决了同类项目没解决的什么问题」；典型「做过才知道」的坑是大文件 `writev` 未更新 iovec 导致传不全。[71]

### 压测：陈硕原文拆开三层，学生侧最好的对照是 linyacool

此前报告已有 ping-pong vs asio 与 nginx 转载。Wave 3 补上原站另外两篇：

- [muduo vs libevent2 吞吐](https://www.cnblogs.com/Solstice/archive/2010/09/05/muduo_vs_libevent.html)：同机 echo、不拆包；两台机器会全是 ~100 MiB/s，比不出库效率；公平读缓冲区后平均高 18%。[72]
- [击鼓传花](https://www.cnblogs.com/Solstice/archive/2010/09/08/muduo_vs_libevent_bench.html)：连接总数到 10 万测的是 **IO 事件效率**，不是业务 10 万 QPS；>10k 时空闲 fd 下 muduo 略优。[73]

学生仓库里方法论相对完整的是 linyacool/WebServer：Webbench 1000 客户端 60s、分长短连接、关日志、内存 Hello World、与改过的 muduo HTTP echo 对照；短连接自己更高（ET + while-accept），长连接 muduo 更高（Buffer）。作者自己解释差异，而不是写「我比 muduo 快」。[74] 这比 MyMuduo「11 秒 9 万条」更接近能答辩。

### 必须保留的限度

公开证据支持「不要用自研网络库当唯一卖点、要能答辩、压测要讲方法」。**不支持**任何 QPS 及格线。**也不能证明**有了切面/熔断/OTLP 就一定比 muduo 克隆更受面试官欢迎——那是与 2023「补一个业务项目」口径同向的推断，不是已验证的拒信统计。[68][69]

---

## Practical Implications（对 Perfacet 评估与简历口径）

1. **用网上 rubrics 评，本仓库的「形状」已经超过典型 mymuduo**（规格、分层、测试、bench 警告、demo 剧本、身份/治理/可观测）。短板在 **P0 实现是否追上规格**，不在「有没有 EventLoop」。
2. **简历第一句不要写网络库。** 写「多 Agent 共用 MCP 入口，按身份切面并治理共享工具并发」。netlib 放第二层。
3. **性能只引用 HTTP 对照和 in-process 附加延迟，并标注 WSL2。** 与 [36][37][45] 一致。
4. **面试故事优先不变量，而不是 QPS：** 在途去重、Grant TTL、secret 目录、loop 禁止 syscall、单实例 SPOF——这些是「自己的思考」，正是 [18][19] 要的。
5. **Agent 线准备：** MCP `2026-07-28`、无 session、`-32021`、Tasks 升格；被问「和 Docker MCP Gateway 的区别」时用范围文档那张表，避免被判套壳 [21]。
6. **未修 P0 不要写进「已完成」。** [21][45] 都惩罚名实不符。
7. **facet/governor 不能替代 netlib 答辩。** 商汤对「只用 muduo 的 JsonRpc」仍挖粘包和 EventLoop。[70] 把网关当主叙事，同时按 TinyWebServer/muduo-core 题单准备关闭路径、`tie`、半包。
8. **有治理 ≠ 已验证更受欢迎。** 合理推断，不是拒信数据；区分度来自「能讲清接到 EventLoop 的代价」（loop 禁止 syscall、OTLP 开销），不是来自名词本身。[68]

---

## Limitations & Gaps

- 「秋招项目成熟度」几乎不是原生检索词，本报告是对课设感/完成度/工程化/产品化的综合。
- 知乎高质量原帖被培训软文淹没；GitHub Discussions 上中文秋招讨论稀少。
- 缺少大厂 C++ 面试官对「MCP 网关」的专门评论，只能用 Agent 简历拆解 + 传统网络库面经拼接。
- 多条高传播帖带课程转化（代码随想录、手写 RPC 避雷、程序员 yt），已降权，但无法完全排除信息级联。
- 陈硕数字来自 ~2010 硬件，**方法有效、绝对值不可直接抄到 2026 简历**。
- 未做第三波引用追猎（例如从陈硕书到 libev bench 原文的完整前向引用链）；Standard 模式浅追到陈硕原文与 nginx/ab 方法即停。
- Wave 2 补到了陈硕 2011 学习经验文、TinyWebServer 作者自证、SegmentFault「muduo 犯恶心」；仍缺少具名大厂 C++ 面试官实名长文专门评 MCP 网关。SegmentFault / 部分掘金「面试官亲述」夹带卖课，已降权。
- CI/单测作为否决项在校招批评里出现频率仍低于「答不出为什么」；把它们当充分条件更多是工程常识外推。
- Wave 3 补到 V2EX「人人都 muduo」、商汤 JsonRpc 面经、陈硕 vs libevent 原帖、linyacool 对照实验。仍几乎没有面试官第一人称「我就是因为 muduo 克隆而拒人」；p99 作为秋招硬性要求缺少真面经。有治理就加分仍是推断。

---

## Future Directions

- 2026–2027 校招对 AI Coding / Agent 工程落地的权重仍在上升；纯 Reactor 克隆会继续贬值。
- 面试官在「阿酥」类造假事件后更倾向深挖与要压力（牛客热议，非系统抽样）。项目仓库的可复现性会更重要。
- 对 Perfacet：把 HTTP bench 表做成可复现脚本输出，比再实现一套多 Reactor 更能提高 rubrics 的 H 维。

---

## Methodology

Phase 0 阅读 Perfacet `README.md`、`docs/SPEC.md`、范围文档、`bench/README.md`、`docs/REVIEW_ACTION_ITEMS.md` 与目录。Phase 1 将问题拆成：秋招共识、C++ 网络库面试、课设分界、压测口径、反面意见。检索平台含知乎、牛客、掘金、CSDN、博客园、GitHub/Gitee；关键词覆盖任务所列并扩展「课设感」「手写 RPC」「QPS 连接数」。Wave 2 并入反面观点与牛客/知乎标准；Wave 3 并入 C++ 网络库秋招口径（V2EX、商汤面经、陈硕 vs libevent、学生对照实验）。主张需至少 2–3 个独立来源；课程广告单独标注。未改任何代码。

---

## Bibliography

### Tier 1 Sources (Credibility >= 0.75)

1. 匿名（自称 C++ 9 年大厂面试官）。「c++程序员简历中项目怎么写？避免踩坑！」。牛客网，约 2023–2024。https://www.nowcoder.com/discuss/625017329121845248 。Credibility: 0.79
2. 夏天0706（蚂蚁集团_技术专家）。「大厂面试项目经历都在问些什么？」。牛客网，2022-03-11。https://www.nowcoder.com/discuss/353159312304447488 。Credibility: 0.73（边界，因时效；主张为永续面试方法论，按 Tier 1 使用）
3. 陈硕。「muduo 与 boost asio 吞吐量对比」。博客园，2010-09-04。https://www.cnblogs.com/Solstice/archive/2010/09/04/muduo_vs_asio.html 。Credibility: 0.89
4. 董哥的黑板报（转述陈硕《Linux 多线程服务端编程》）。「muduo网络库：20---muduo性能测评」。CSDN，2020（书内容更早）。https://blog.csdn.net/qq_41453285/article/details/107018098 。Credibility: 0.77
5. 陈硕。muduo pingpong client。GitHub。https://github.com/chenshuo/muduo/blob/master/examples/pingpong/client.cc 。Credibility: 0.86
6. 陈硕。muduo 官方仓库。https://github.com/chenshuo/muduo 。Credibility: 0.90
53. 陈硕。「谈一谈网络编程学习经验（06-08更新）」。博客园，2011-06-08。https://www.cnblogs.com/Solstice/archive/2011/06/06/2073490.html 。Credibility: 0.92
54. 陈硕。「Muduo 设计与实现之一：Buffer 类的设计」。博客园，2011-04-17。https://www.cnblogs.com/Solstice/archive/2011/04/17/2018801.html 。Credibility: 0.91

### Tier 2 Sources (Credibility 0.50–0.74)

7. 代码界的小白。「面试官怼我简历上的项目，我被问傻了，项目该怎么准备？」。牛客网，2022-04-28。https://www.nowcoder.com/discuss/353159424011345920 。Credibility: 0.59
8. 程序员花海。「面试官视角拆一份Agent项目简历，防止避坑！」。牛客网，2026-06。https://www.nowcoder.com/discuss/898495069127200768 。Credibility: 0.72
9. 程序员花海。「27 秋招简历核心：实习远胜同质化项目」。牛客网，2026-06。https://www.nowcoder.com/discuss/900896686346694656 。Credibility: 0.70
10. 程序员yt。「南邮26届C++后端选手暑期实习颗粒无收…」。牛客网，2025-05-24。https://www.nowcoder.com/discuss/755793806250745856 。Credibility: 0.60
11. 「muduo网络库为什么高性能？」。技术栈，约 2025–2026。https://jishuzhan.net/article/2036963938500411393 。Credibility: 0.63
12. 蒸汽教育 Stem Career Group。「破局「课设感」」。博客园，2026-04-22。https://www.cnblogs.com/stemcareergroup/articles/19906201 。Credibility: 0.61
13. 门头沟学院 C++。「安恒信息 C++开发 一面」。牛客网，2026。https://www.nowcoder.com/discuss/858425164415922176 。Credibility: 0.74
14. 门头沟学院 C++。「淘天 客户端开发-C++ 一面」。牛客网，2026。https://www.nowcoder.com/discuss/858122712575639552 。Credibility: 0.72
15. 「腾讯云智二面」。牛客网。https://www.nowcoder.com/discuss/476840885213323264 。Credibility: 0.68
16. 门头沟学院 C++。「优必选 C++开发工程师 二面」。牛客网，2026。https://www.nowcoder.com/discuss/859392827090821120 。Credibility: 0.70
17. 知乎答主。「你遇到过哪些高质量的 C++ 面试？」。https://www.zhihu.com/tardis/bd/ans/1783988850 。Credibility: 0.71
18. miseryjerry。「长文梳理muduo网络库核心代码」。博客园，2022。https://www.cnblogs.com/S1mpleBug/p/16712003.html 。Credibility: 0.61
19. 「2025简历避雷：别再沉迷手写 RPC」。牛客网。https://www.nowcoder.com/discuss/766666581601419264 。Credibility: 0.54
20. 「手写轮子项目就是玩死自己！」及评论。牛客网，2024-12。https://www.nowcoder.com/feed/main/detail/cd95915cd6324462ad9fa908e0f2778c 。Credibility: 0.65
21. 面灵。「程序员简历怎么写才能过AI筛选？（2026）」。https://www.mianlingai.com/blog/developer-resume-writing-guide-2026/ 。Credibility: 0.58
22. 智灵简历。「简历项目经历怎么写？」。https://cv.mianlingai.com/guide/project-experience 。Credibility: 0.55
23. 代码随想录。「C++项目推荐：网络库」。https://programmercarl.com/other/project_muduo.html 。Credibility: 0.58
24. 「仿muduo库实现高并发服务器」。技术栈。https://jishuzhan.net/article/2076128183450738690 。Credibility: 0.52
25. 掘金。「muduo网络库解析」。https://juejin.cn/post/7124678060699287565 。Credibility: 0.55
26. 掘金。「手写C++muduo库(Cmake…)」。https://juejin.cn/post/7136900752710041636 。Credibility: 0.52
27. 掘金 青训营。「网络库Buffer的设计与实现」。https://juejin.cn/post/7131736389883789349 。Credibility: 0.60
28. 掘金。「muduo网络库的多线程模型」。https://juejin.cn/post/7194640336461758525 。Credibility: 0.50
29. 掘金。「手写C++ muduo库（Poller）」。https://juejin.cn/post/7152510552320213023 。Credibility: 0.50
30. Clay。「基于 C++ 手写 Muduo 高性能网络库」。https://www.techgrow.cn/posts/dbb10768.html 。Credibility: 0.56
31. CSDN。「muduo面试准备」。https://blog.csdn.net/2301_80355452/article/details/149327373 。Credibility: 0.50
32. 极客文档。「压测报告模板」。https://geekdaxue.co/read/casa@ysnhml/dbeln1 。Credibility: 0.62
33. 「面试官：你的项目有哪些难点？」。牛客网，2024。https://www.nowcoder.com/discuss/648948173246844928 。Credibility: 0.55
34. 「从S到A,从夯到拉,ai项目盘点」。牛客网。https://www.nowcoder.com/discuss/876037539725901824 。Credibility: 0.58
35. 知乎专栏。「金三银四，属于你的入行机会来了」。https://zhuanlan.zhihu.com/p/2012213441843210068 。Credibility: 0.50
36. 牛客评论。「佬我项目准备的也是muduo库…代码随想录」。https://www.nowcoder.com/discuss/comment/21701056 。Credibility: 0.60
37. TrueSight。「从学生作业到商业资产：课程设计的简历重构手册」。https://tsight.io/articles/12058683 。Credibility: 0.52
38. Graduate-Notes。「秋招第一再计划」。https://github.com/zwenjing1314/Graduate-Notes 。Credibility: 0.55
39. Gitee yezhening/library 学生网络库 README。https://gitee.com/yezhening/library 。Credibility: 0.50
40. 阿里 Java 一面面经。知乎。https://zhuanlan.zhihu.com/p/1960014960122922350 。Credibility: 0.57
55. qinguoyi。「TinyWebServer」README。GitHub。https://github.com/qinguoyi/TinyWebServer 。Credibility: 0.70（作者自证面试穿帮；QPS 方法弱）
56. 「烂大街的项目就别写到简历上了」。SegmentFault。https://segmentfault.com/a/1190000047153187 。Credibility: 0.55（招人视角 + 卖课，结论偏绝对）
57. 「【秋招总结】结合自身经历谈谈如何准备后台开发岗位」。牛客网。https://www.nowcoder.com/discuss/353159091403038720 。Credibility: 0.66
58. 程序员鱼皮。「鱼皮的写简历指南（保姆级）5、项目真实性优化」。牛客网。https://www.nowcoder.com/discuss/632263957616635904 。Credibility: 0.68
59. 达同学。「5、如何有深度的介绍项目」。牛客网，2023-12-05。https://www.nowcoder.com/discuss/558456647572566016 。Credibility: 0.67
60. AutoDriver。「校招简历-技术岗简历最怕的，不是没项目，是写成「报菜名」」。博客园，2026-04-09。https://www.cnblogs.com/autodriver/p/19843132.html 。Credibility: 0.62
61. Issie。「弱背书应届生的工程化简历构建方法」。https://www.codefather.cn/post/2021656831176216578 。Credibility: 0.60
62. 「面试官通过项目深挖真正在验证什么？」。牛客网。https://www.nowcoder.com/discuss/917814969171730432 。Credibility: 0.58
63. 「无实习人士如何上岸秋招」。牛客网，2023-11-10。https://www.nowcoder.com/discuss/545916636923006976 。Credibility: 0.70
64. YukunJ/Turtle（TinyWebServer 同机复测）。https://github.com/YukunJ/Turtle 。Credibility: 0.64
65. LACHENNG。「LanceNet」README。https://github.com/LACHENNG/LanceNet 。Credibility: 0.48
66. tokeyjs。「MyMuduo」。https://github.com/tokeyjs/MyMuduo 。Credibility: 0.42
67. 「校招面试讲不好项目，基本必挂!」。牛客网。https://www.nowcoder.com/discuss/599745574723743744 。Credibility: 0.52
68. xiaoyangST 等。「准备校招面试用到的项目，要到哪种程度才算属于自己？」。V2EX，2023-11。https://www.v2ex.com/t/992224 。Credibility: 0.72
69. 「【校招方向】C++输麻了」。牛客网，2023-11-23。https://www.nowcoder.com/discuss/557342911868739584 。Credibility: 0.68
70. 落羽的落羽。「商汤科技 C++日常实习 一面」。牛客网。https://www.nowcoder.com/feed/main/detail/8525aebe924a48ecb6b8b17d1b80fdaf 。Credibility: 0.71
71. Emma1111。「【TinyWebServer】13踩坑和面试题」。博客园，2023-10-06。https://www.cnblogs.com/Wangzx000/p/17745041.html 。Credibility: 0.60
72. 陈硕。「muduo 与 libevent2 吞吐量对比」。博客园，2010-09-05。https://www.cnblogs.com/Solstice/archive/2010/09/05/muduo_vs_libevent.html 。Credibility: 0.90
73. 陈硕。「击鼓传花：对比 muduo 与 libevent2 的事件处理效率」。博客园，2010-09-08。https://www.cnblogs.com/Solstice/archive/2010/09/08/muduo_vs_libevent_bench.html 。Credibility: 0.90
74. linyacool。「WebServer 测试及改进」。https://github.com/linyacool/WebServer/blob/master/测试及改进.md 。Credibility: 0.66
75. 「秋招总结分享：C++后端项目的进阶之路」。牛客网。https://www.nowcoder.com/feed/main/detail/ca4812f6250f4363bfbdf3c49cab91ea 。Credibility: 0.58

### Tier 3 Sources (Context / 广告或转载，仅背景)

41. 「从Reactor到网络库：10天打造生产级C++高性能网络库」。微信（课程销售）。https://mp.weixin.qq.com/s/Y3713qRGGQgCO8btemWR_w 。Credibility: 0.32（主张「10 天生产级」不采信）
42. 备战 23 秋招 c/c++Linux 后端（课程路径文）。https://devpress.csdn.net/v1/article/detail/126196822 。Credibility: 0.35
43. 「你的大模型项目，能扛住面试官的几连问？」。知乎（训练营）。https://zhuanlan.zhihu.com/p/2039673415901106968 。Credibility: 0.35
44. 2026 后端校招面试逻辑变革（聚合 400 条面经的二手文）。http://www.jsqmd.com/news/1230878/ 。Credibility: 0.40
45. 简小派。「构建高并发项目模版」（抓取 500，仅用搜索摘要）。https://wiki.jianlipai.com/high-concurrency-project-template.html 。Credibility: 0.48
46. 2026 届秋招后端上岸心得。https://www.nowcoder.com/discuss/885989862313046016 。Credibility: 0.45（页面拉取失败，仅摘要）
47. 课程设计项目经历深度挖掘（AI 简历工具）。https://www.resumemakeroffer.com/blog/post/103889 。Credibility: 0.30
48. Bohan 自研 C++ 网络框架 README 镜像。https://github.laiyagushi.com/Lijian1122/Bohan 。Credibility: 0.35
49. CSDN 文库。「C++高性能网络服务面试常考…」。https://wenku.csdn.net/answer/2yacihesgk0n 。Credibility: 0.35
50. 酷盾。「服务器压力测试报告模板」。https://cloud.kd.cn/ask/169777.html 。Credibility: 0.40
51. CSDN。「性能测试报告生成的Prompt模板」。https://blog.csdn.net/qq_42831750/article/details/155568408 。Credibility: 0.30
52. 51CTO 转载 muduo 性能测评。https://blog.51cto.com/u_15346415/3673831 。Credibility: 0.40（与 [4] 同源，不重复计主张）

---

## 附录：主张三角验证摘要

| 主张 | 支持 | 反对/限定 | 结论 |
|---|---|---|---|
| 细扣模块+讲清 why 比项目难度更重要 | [18][19][7][45] | 无实质反对 | Verified |
| 假 QPS / 无配置的高性能是减分项 | [45][21][2][36] | 无 | Verified |
| 纯 muduo 克隆不宜当唯一主项目 | [23][41][27][40][68][69] | [39] 评论区：C++ 岗更爱问轮子；[70] 即使用库也会深挖 | Contested → 2023 后 Strong「不宜当唯一主项目」 |
| 2026 Agent/MCP 加分但按业务标准拷打 | [21][41][43][44] | 培训文夸大「护身符」 | Strong |
| 课设=Happy Path；成熟=边界+观测+压测 | [2][21][40][32] | 校招对 CI 更宽容 [45] 基础优先 | Strong |
