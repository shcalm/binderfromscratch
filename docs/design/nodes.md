# 第 07 章设计决策：对象身份不能由方法号代替

状态：开发实现、75 项用户态及完整 TCG/KVM 通过，证据见[报告](../reports/m3-nodes-progress.md)，尚未正式发布；上一开发检查点 `dev-c06-20261106`。本决策对应 C07-01 至 C07-06，不预先宣称支持 Ref、服务发现或多服务进程。

## 1. 新需求和候选方案

一个 Server 同时提供两个 counter 对象，各自执行同一个 increment 方法，却保持独立状态；另有 add 对象。第 06 章的事务 ID 标识一次调用，业务 code 标识方法，都不能标识跨多次调用持续存在的对象。

| 方案 | 能做到什么 | 代价与决定 |
| --- | --- | --- |
| 将每个对象编码为不同方法号 | 小型固定服务能完成分发 | 对象创建、销毁和方法表耦合；相同方法的对象仍需另一个身份字段。 |
| 直接把用户地址发给其他进程 | 拥有者可按地址查本地对象 | 地址空间不同、地址复用、地址暴露；内核不能解引用它执行业务，也不能据此确认对象还活着。 |
| 拥有者内部 cookie 加内核 Node 身份 | 内核检查目标仍发布并持有元数据，Server 按自己的表分发 | 本章选择；增加注册表、重复发布规则、在途引用、撤销与退出清理。 |

暂用全局单调 node_id 寻址，通过 pipe 把 ID 交给 Client；不增加 ServiceManager。ID 可猜，不是私有 Handle、秘密或授权凭证。设备访问者知道有效 ID 即可调用，方法权限仍须应用检查；第 08 章讨论私有引用。cookie 是拥有者选的非零不透明整数，本章刻意不使用指针。相同 cookie 在不同打开上下文不代表同一对象。

保留唯一全局 Server，新增独立 NODE 模式；同一 Server 多对象已满足本章需求。多拥有者同时服务需要重做目标队列和退出作用域，留待明确需求产生时演进。所有 C06 命令与测试保留。

## 2. 最小 ABI

新增 `MB_FEATURE_NODE=16`，每 Server 至多 `MB_MAX_NODES=16`。命令号 16–22 依次为 REGISTER_NODE、PUBLISH_NODE、REMOVE_NODE、CALL_NODE、RECEIVE_NODE、REPLY_NODE、NODE_STATS。

| 命令 | 定宽结构与语义 |
| --- | --- |
| REGISTER_NODE | 无参数；与 REGISTER_TRANSACTION 同样要求已映射、无 active_calls、没有其他 Server，注册唯一 NODE Server。 |
| PUBLISH_NODE | 16 字节 `{u64 cookie; u64 id;}`，cookie 非零、id 为输出。同一拥有者/cookie 幂等返回相同 id，不增加发布持有；新对象输出 EFAULT 必须撤销新 Node，幂等查询输出失败保留原 Node。 |
| REMOVE_NODE | 输入 u64 id；仅 Node 拥有者可移除，无此 ID ENOENT、其他上下文 EPERM。持有数不止发布持有时 EBUSY，失败不改变可调用状态。 |
| CALL_NODE | 56 字节 `{u64 target; u32 code; u32 reserved; struct mb_transaction_call call;}`。两个 reserved 均须 0；target 非零。普通数据、映射、回复大小及信号语义沿用 C06。code 由 Server 解释，驱动不拒绝未知业务方法。 |
| RECEIVE_NODE | 56 字节 `{struct mb_buffer_request buffer; u64 node; u64 cookie; u32 code; u32 reserved;}`，内核附加持有目标的身份、owner cookie 和方法号。输出成功才交付；EFAULT 保留 QUEUED。 |
| REPLY_NODE | 沿用 24 字节 mb_transaction_reply，但独立命令隔离服务模式。目标来自原事务，Server 不能重新指定 Client 或 Node。 |
| NODE_STATS | 24 字节 `{u64 live_nodes; u32 owned_nodes; u32 transaction_refs; u32 slot_refs; u32 reserved;}`。live_nodes 为全局实际分配数，其余为当前上下文拥有的 Node 汇总；退出后孤立 Node 只计全局。 |

PUBLISH/REMOVE 先校验上下文身份和模式。NODE_STATS 可由 observer 使用。CALL_NODE 在有非 NODE Server 时优先 EPROTOTYPE；无 Server 或当前 NODE Server 中目标不存在/已移除为 ENOENT，合法目标不能自调用仍 EDEADLK。达到 Node 上限 EAGAIN；ID 达 U64_MAX 时 EOVERFLOW、永不回绕复用。copy 失败 EFAULT，错误无持有/槽遗留。

## 3. 三份持有为什么都需要

所有 Node 表、持有及事务变化在 mb_lock 下；固定上限允许简单链表。本章不用一般强弱引用协议，三种内部持有是具体资源责任。

| 持有 | 获得 | 归还 |
| --- | --- | --- |
| 发布持有 1 | 新 PUBLISH 成功 | REMOVE 成功或 owner 最终 release。 |
| 事务持有 | CALL 完成全部验证与请求复制、即将发布 | CALL 取得终态、摘表；失败发布不得遗留。即使 Server 先 FREE 请求，也能保护仍等待回复的 Node 元数据。 |
| 请求槽持有 | 成功发布的请求槽关联 Node | 对应 FREE、未交付请求回滚或 owner 最终 release。Client 在 DELIVERED 后取消也不能让 Server 手中的对象身份失效。 |

Node 总持有为三者之和，计数归零才摘表释放。REMOVE 只允许总数为 1，因此成功后旧 node_id 立即失效，重新发布同 cookie 得到新 ID。不是将 busy Node 悄悄标为 inactive；EBUSY 后仍可调用。

不把缓冲 token、node_id、cookie 或事务 ID 相互替换。请求槽 FREE 不终结事务，回复也不隐式 FREE 请求；回复槽不持有目标 Node，因为它仅承载已经形成的结果。Client 输出失败仍走 C06 回复槽回滚。

Server 最终 release 时，先终结其未结束事务、清理所有请求槽的 Node 持有，再摘除拥有关系并归还发布持有。仍由 CALL 记录持有的 Node 可短暂成为 owner=NULL 的孤立元数据，CALL 摘表后归零释放；任何路径不解引用已释放 owner。release 完成后不能以旧 ID 发起调用。close 与 VMA 最终引用继续遵守 C04/C06 语义。

FREE 新增 mb_lock 外层，统一锁顺序 `mb_lock → region.lock`，以便安全归还槽的 Node 持有。mmap/VMA 回调不取 mb_lock。发布与复制仍持大锁，缺页阻塞的成本继续存在。

## 4. 用户态对象由谁保活

内核持有只能保护 `mb_node`，不能延长用户态 counter 的 C 对象生命。Server 按 node_id/cookie 联合查本地表，执行业务前取得本地活动持有，完成后归还；只有 REMOVE 成功且本地活动数为零才可销毁本地对象。并发实现必须以同一用户态锁协调查表/取得活动持有与注销/销毁，不能只读一个裸计数。示例同步分发并在业务读取完成后 FREE，请求借用的数据随后不可再访问。

特别地，Server 可能先 FREE 请求、Client 又取消，此时内核 busy 归零，却仍有用户后台任务在运行。不能将 REMOVE 成功单独当作后台任务完成证明。本章不实现通用用户态引用框架；模型要明确展示本地活动持有与内核持有是两种责任。

## 5. 实验矩阵与实现顺序

1. 普通 C 先执行“只按方法号，两 counter 状态混合”的反例，再用 Node registry 对照；测试重复 cookie、同方法独立状态、事务/槽各自阻挡撤销、旧 ID 与 cookie 复用、本地活动仍在的销毁限制。
2. 先以计划命令号在 C06 驱动运行 Node 注册探针，明确 ENOTTY；实现后再完成全部行为，探针通过不等于矩阵通过。
3. VM 中两个 counter 使用相同 code，交错调用和结构化回复证明状态独立；add/未知方法属于 Server 分发，驱动不执行业务。
4. 重复发布同 cookie 不增数；零 cookie、容量 16、错误用户指针/只读输出、旧新模式隔离、错误拥有者撤销、未知 Node、旧 ID 不复用均验收。
5. queued、delivered、请求已 FREE 但回复未完成、回复已完成但请求未 FREE 分别 REMOVE EBUSY；DELIVERED 取消后槽仍阻挡，FREE 后可撤销；撤销失败仍可调用。
6. 新 CALL 输入复制、RECEIVE 输出、REPLY 输入和 CALL 输出故障回滚，最终 Node/事务/槽账目恢复；继承同一传输逻辑的回归与新模式专项结果分别记录。
7. Server queued/delivered 最终退出、Client queued/delivered SIGKILL、Node 发布输出失败和重复查询输出失败分别核对资源；待调用者退出后全局 live_nodes=0。
8. 完整 make test、W=1、TCG/KVM、旧回归与网站检查。实际 kmalloc ENOMEM、64 位耗尽、KASAN/lockdep 和任意交错没有注入就保持未覆盖。

文件分工：`include/uapi/mini_binder.h` 定义协议，`driver/mini_binder.c` 连续演进，`examples/models/nodes.c`、`tests/test_node_model.py`、`docs/labs/node-model.md` 为 C 模型，`tests/kernel/node_test.c` 为 VM 专项，`docs/chapters/07-nodes.md` 以问题和实验组织正文，`docs/reports/m3-nodes-progress.md` 记录实际证据。根 Makefile/rootfs/init 只接入对应程序，网站与任务状态待验证后更新。

## 6. 固定版本 Android 对应

核查 Linux v6.6 `binder_get_node_ilocked`/`binder_init_node_ilocked`：真实按 owner proc 内的 ptr 查 Node，另存 cookie；它们是返回拥有者用户态的标识，不是驱动可直接调用的函数。RECEIVE 中 trd.target.ptr 与 cookie 来自 target_node。`binder_get_node_refs_for_txn` 获取目标持有，buffer.target_node 保存目标，`binder_transaction_buffer_release` 归还目标强持有；不能说回复一到即释放所有目标持有。

mini 主动 PUBLISH/REMOVE、全局 node_id、非指针 cookie、撤销 EBUSY、三种计数和唯一 Server 均为教学设计。真实远程目标通常经进程私有 binder_ref/handle 寻址，不是直接传全局 Node debug_id。第 08 章才引入这个差异；正文必须写出固定源码链接和限制，不能把本章 registry 叫作完整 Binder 对象网络。
