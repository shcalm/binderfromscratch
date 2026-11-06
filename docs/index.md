# 从零设计 Binder

从 `add(1, 2)` 出发，通过问题、方案比较与实验构建教学 IPC 驱动，再对照固定版本的 Android Binder。

**当前是开发阅读版。** P01–P04 和第 01–07 章已有初稿与实验；尚未完成全部教学验收，也没有正式章节发布标签。第 08–18 章只有路线与任务入口。

| 阅读顺序 | 内容 | 状态 |
| --- | --- | --- |
| 预备篇 | [P01 进程](chapters/p01-process.md) → [P02 系统调用](chapters/p02-syscalls.md) → [P03 内核模块](chapters/p03-module.md) → [P04 实验环境](chapters/p04-environment.md) | 初稿；执行模块实验前先准备 P04 环境 |
| 第一篇 | [01 IPC 动机](chapters/01-why-binder.md) → [02 字符设备](chapters/02-character-device.md) → [03 同步 RPC](chapters/03-sync-rpc.md) | 初稿；尚未正式发布 |
| 第二篇 | [04 mmap 接收映射](chapters/04-mmap.md) → [05 Parcel 请求](chapters/05-parcel.md) → [06 Transaction](chapters/06-transactions.md) | 开发初稿；尚未正式发布 |
| 第三篇 | [07 Node 对象身份](chapters/07-nodes.md) → [08–09 章路线](reading-path.md#part-3) | 07 为开发初稿；08–09 未完成 |
| 第四至六篇 | [10–18 章路线与任务](reading-path.md#part-4) | 未完成；没有对应正文 |

运行前先看[实验入口](labs/index.md)和[代码版本索引](reference/code-versions.md)。章节中的源码链接展示本次站点构建时的工作树，旧章节快照须按版本索引核对。

[教学规范](design/teaching-contract.md)说明论证标准，[范围声明](design/scope.md)说明教学简化，[M0/M1 阶段报告](reports/m0-m1-progress.md)、[映射报告](reports/m2-mapping-progress.md)、[Parcel 报告](reports/m2-parcel-progress.md)和[事务报告](reports/m2-transactions-progress.md)记录实际证据及边界。逐项完成状态以[任务总表](plans/task-backlog.md)为准。
