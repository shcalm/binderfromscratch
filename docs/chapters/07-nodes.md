# 第 07 章｜同一个方法，究竟调用哪个对象

第 06 章已经允许两个线程共用 Client fd，同时等待两笔独立事务，并按内核事务 ID 接收反序完成的结构化回复。请求槽、回复槽和事务记录分别回收，取消与回复以锁内先终结者为准。上一开发检查点为 `dev-c06-20261106`，见[版本索引](../reference/code-versions.md)。本章继续沿用这些机制。

现在 Server 要提供两个 counter 对象，各自执行相同的 increment 方法，还提供一个 add 对象。事务 ID 能回答“这是哪一次调用”，方法 code 能回答“执行什么操作”，却都没有回答“修改哪一份 counter 状态”。本章增加跨多次调用持续存在的对象身份。

[Node 设计决策](../design/nodes.md)固定本章范围；[驱动](../../driver/mini_binder.c)和 [UAPI](../../include/uapi/mini_binder.h)沿原文件演进。当前 75 项用户态和完整 TCG/KVM 通过，实际命令、日志哈希和覆盖边界见[开发报告](../reports/m3-nodes-progress.md)。开发检查点为 `dev-c07-20261106`，仍未正式发布；注册探针成功本身不能替代对象分发、失败回滚和退出矩阵。

## 1. 只看方法号会混合两份状态

设 A、B 都从 0 开始，`code=1` 都表示 increment。用户依次调用 A、B、A，希望看到：

```text
目标 A，increment：1
目标 B，increment：1
目标 A，increment：2
```

若分发器只根据 code 找到一份共享 counter，实际会得到 1、2、3。它可能每次都正确执行“加一”，每个事务也都正确收到自己的回复，但修改的状态对象已经错了。

[nodes.c](../../examples/models/nodes.c) 的 `method` 模式实际累加这一份共享状态，并逐次输出 expected 与 observed。给这个计数器加互斥锁只能使加一操作有序，不能创造另一份独立状态。增加事务 ID 也只能区分三次调用，不能说明第一、三次应该共享 A、第二次应该访问 B。

因此需要至少两个分发维度：目标对象和对象上的方法。业务参数仍放在 payload 中；对象身份不能偷偷藏成“不同对象占不同方法号”的长期约定。

## 2. 三种可行方案及本章选择

| 方案 | 在什么条件下足够 | 新约束下的取舍 |
| --- | --- | --- |
| 为每个对象分配不同方法号 | 对象集合固定、接口较小时简单直接 | 创建对象需要改方法编号规则；相同接口的不同实例与方法表耦合。 |
| 在请求中直接携带 Server 用户地址 | Server 可以在自己的地址空间解释地址 | Client 不能直接解引用；地址会复用、暴露布局，内核也不能据此确认对象仍存活。还需独立的注册、权限和保活规则。 |
| 内核 Node 身份加拥有者本地 cookie | 内核检查目标仍发布并保护元数据，Server 按自己的表查找实例 | 本章选择；增加发布、撤销、在途持有、失败回滚与退出清理。 |

第二种方案并非任何情况下都不可行；后面的真实 Binder 对照就使用拥有者地址作为本地标识的一部分。但安全性来自周围的查找、引用和进程作用域，不能只把一个裸指针塞进请求。

mini 的 cookie 刻意选为非零整数，不使用 C 指针。内核分配单调 node_id，Server 保存 `(node_id, cookie) → 本地对象` 的表；Client 通过 pipe 获取 node_id。内核不执行 increment 或 add，不检查未知业务 code 是否存在。

本章仍只有唯一全局 Server，新增独立 NODE 服务模式。同一 Server 提供多个对象已经满足需求；多拥有者同时服务会改变队列路由和退出作用域，暂不实现。没有 ServiceManager，也没有进程私有 Ref/Handle。

## 3. 四种身份各自回答一个问题

| 数值 | 产生与解释者 | 回答的问题 |
| --- | --- | --- |
| node_id | 驱动分配，CALL_NODE 指定 | 跨多次调用访问哪个已发布对象？ |
| cookie | Server 选择，驱动保存并在 RECEIVE_NODE 返回 | 拥有者如何查自己的本地对象表？ |
| 事务 id | 每次 CALL 发布时由驱动分配 | 哪一笔尚未终结的调用可以接受这次回复？ |
| 请求/回复 token | 接收区分配器产生，所属上下文 FREE | 归还哪个槽的这次使用？ |

方法 code 是第五个维度，由 Server 解释。两个 counter 可以使用相同 code；同一 counter 的多次调用有不同事务 ID；同一事务的请求、回复也有各自 token。这些数值即使偶然相等，也不能互换。

node_id 在本章是全局可猜的寻址数字，不是秘密、授权凭证或私有 Handle。设备访问者知道有效 ID 即可尝试调用，业务权限仍由应用处理。cookie 不发给 Client 当作调用凭证；它随目标描述符返回其拥有者。

## 4. 先补充本章需要的持有知识

**保存指针不等于持有对象。** `mb_node *` 只记录地址；若其他路径已经释放元数据，随后读 id 或 cookie 仍会出错。每个允许地址继续存活的责任，都必须有对应的获得和归还点。

**计数为零才释放，需要共同的同步规则。** 本章 Node 表、持有计数与事务变化都在 `mb_lock` 下进行。线程不能在锁外读到地址，再希望另一个线程暂时不要撤销它。固定上限较小，驱动用链表查找，不引入通用强弱引用框架。

**持有元数据不等于让进程不死。** Node 描述对象身份，不替用户态对象分配存储，也不无限持有 Server fd。最终文件 release 可以发生，随后 Node 可能暂时只剩等待 CALL 的内核元数据。

**最终 release 不等于某一次 close。** 执行中的 ioctl 和 VMA 都可能继续持有文件。只 close fd、仍保留映射，不能据此宣布服务死亡。测试必须控制解除映射和最终关闭，继续遵循[第 04 章](04-mmap.md)的生命周期规则。

## 5. 为什么需要三份持有

定义当前 Node 的总持有数：

```text
R = 发布持有 + transaction_refs + slot_refs
发布持有 = owner 非 NULL 时的 1，否则为 0
R == 0 才能从 Node 表摘除并释放元数据
```

| 持有 | 获得点 | 归还点 | 单独防住什么空窗 |
| --- | --- | --- | --- |
| 发布持有 | 新 PUBLISH 成功 | REMOVE 成功或 owner 最终 release | 暂无调用时，对象仍可被再次寻址。 |
| 事务持有 | CALL 验证和请求复制完成、即将发布 | CALL 观察终态并摘表 | Server 已 FREE 请求，但调用还在等待回复。 |
| 请求槽持有 | 发布的请求槽关联 Node | FREE、未交付回滚或 owner 最终 release | Client 已取消或收到回复，但 Server 仍拿着已交付请求。 |

看一条先 FREE 再回复的路径。新发布时 R=1；CALL 发布后 R=3；RECEIVE 只改变请求交付状态，不减持有；FREE 请求后 R=2；回复被接受后也不能立即假设等待线程已摘表，CALL 摘表才使 R=1。此时 REMOVE 才能撤销最后的发布持有。

| 观察点 | 发布 | 事务 | 槽 | R | 此时 REMOVE |
| --- | --- | --- | --- | --- | --- |
| PUBLISH 完成 | 1 | 0 | 0 | 1 | 可成功，但本例继续调用。 |
| CALL 发布 | 1 | 1 | 1 | 3 | EBUSY。 |
| RECEIVE 完成 | 1 | 1 | 1 | 3 | EBUSY，领取不等于释放。 |
| Server FREE 请求 | 1 | 1 | 0 | 2 | EBUSY，等待记录仍需身份。 |
| REPLY 已接受，CALL 尚未摘表 | 1 | 1 | 0 | 2 | EBUSY。 |
| CALL 摘表 | 1 | 0 | 0 | 1 | 可成功。 |
| REMOVE 成功 | 0 | 0 | 0 | 0 | 旧 ID 已不存在。 |

表中“CALL 尚未摘表”是定义明确的逻辑阶段，不保证用户态总能观察到它。唤醒线程可能立即运行并完成摘表；若测试需要固定停在某一阶段，必须有额外同步，不能凭 REPLY 返回与下一条统计之间的时间间隔推断。

再看先回复、后 FREE。CALL 摘表后事务持有已归还，但请求槽仍使 R=2，所以 REMOVE 仍须 EBUSY。槽中保存的 Node 身份不能因为一次回复完成就变成悬空指针。

回复槽不持有目标 Node。它承载已经形成的结果，有自己的 Client token；其输出与回滚继续由 C06 规则管理。持有更多资源并不自动更正确，关键是每一份持有必须对应尚未结束的责任。

## 6. EBUSY 保留对象，而非悄悄撤销

REMOVE 只允许总持有数为 1，即只剩发布持有。若仍有事务或请求槽，返回 EBUSY，**对象继续发布并可被调用**。本章没有“先标 inactive，等最后引用消失”的异步撤销语义。

这使应用可以作出明确决定：停止自己接纳的新工作，等待在途责任结束，再重试 REMOVE。它并不保证所有 Client 会配合，因此持续新调用可能使撤销一直忙；驱动没有提供关闭入口、撤销期限或公平性保证。

REMOVE 成功后旧 node_id 立即不可调用。同一 cookie 以后可以重新发布，但得到新 ID；单调 ID 不回绕复用。旧请求不能因为数值复用而误投给新实例。

PUBLISH 对同一拥有者、同一 cookie 幂等，返回原 ID，不叠加发布持有。相同 cookie 在不同打开上下文没有跨上下文身份含义。当前只允许一个 Server，不意味着 cookie 已成为全局命名系统。

## 7. 新 ABI 如何接入已有事务

协议定义见 [mini_binder.h](../../include/uapi/mini_binder.h)，能力位为 `MB_FEATURE_NODE=16`，每 Server 最多发布 16 个 Node。命令 16–22 独立于旧模式：

| 命令 | 结构或前提 | 核心语义 |
| --- | --- | --- |
| REGISTER_NODE | 无参数；已映射、无 active_calls、没有其他 Server | 注册唯一 NODE Server。 |
| PUBLISH_NODE | 16 字节 cookie/id | cookie 非零；返回已有或新分配的 ID。 |
| REMOVE_NODE | 输入 u64 id | 校验当前 Server、NODE 模式与拥有者；忙时不撤销。 |
| CALL_NODE | 56 字节 target/code/reserved/call | target 非零；内外 reserved 均为零；请求 1–4096 字节。 |
| RECEIVE_NODE | 56 字节 buffer/node/cookie/code/reserved | 返回请求槽描述符和目标身份，输出成功才交付。 |
| REPLY_NODE | 24 字节 mb_transaction_reply | 按领取的事务 id 回复，不重新指定目标或 Client。 |
| NODE_STATS | 24 字节统计 | 全局 live_nodes，以及当前上下文的 owned_nodes、事务/槽持有汇总。 |

CALL 仍要求 Client 接收映射，允许同 fd 多线程并发；回复为 0–4096 字节，空回复不申请槽。code 未知由 Server 返回业务 status，例如 -ENOSYS，不能把它误写成驱动拒绝方法。

CALL_NODE 遇到现存非 NODE Server 优先 EPROTOTYPE；没有 Server 或目标不存在、已撤销时为 ENOENT。合法目标不能自调用，返回 EDEADLK。PUBLISH/REMOVE 先检查身份和模式；错误上下文 EPERM，当前旧模式 Server 使用它们为 EPROTOTYPE。

容量满 EAGAIN，ID 耗尽 EOVERFLOW，用户复制失败 EFAULT。错误码不是资源账目的替代品，失败后还要核查原目标、事务和槽是否按协议保留或回滚。

| 失败发生处 | Node 发布 | 事务/请求槽 | 恢复出口 |
| --- | --- | --- | --- |
| 新 PUBLISH 输出失败 | 撤销本次新建；ID 已消费 | 尚无调用，不建立在途持有 | 修正输出地址后重新发布。 |
| 幂等 PUBLISH 输出失败 | 保留原 Node、原 ID | 已有调用完全不受影响 | 重新查询。 |
| CALL 请求输入失败 | 保留目标发布 | 不发布事务，临时槽回滚 | 修正输入后重新发起。 |
| RECEIVE 输出失败 | 保留 | 保持 QUEUED 和请求槽持有 | 丢弃失败输出，重新领取。 |
| REPLY 输入或目标槽失败 | 保留 | 保持 DELIVERED；不遗留临时回复槽 | 修正输入、FREE 恢复或空回复。 |
| CALL 回复输出失败 | 不额外改变发布 | 调用已终结并摘表；撤销未交付回复槽 | 不自动重发业务；请求仍按原归属处理。 |

这张表中的“保留”是协议责任。不能把“错误无遗留”解释成失败后一律清空所有 Node 和槽，那会删除原来已经成功发布或已经交付的资源。

## 8. 跟随实现核对失败和锁

[mb_call](../../driver/mini_binder.c) 在锁内查目标与模式，完成输入复制后才同时建立事务和槽的 Node 持有，再发布事务。预留槽或输入复制失败发生在建立 Node 持有之前，撤销临时槽即可，不能留下半发布记录。

RECEIVE_NODE 在同一把锁内读取 Node 的 id/cookie/code，复制输出成功才转 DELIVERED。EFAULT 可部分写用户结构，但事务仍 QUEUED，三份持有不变；用户必须丢弃失败输出并重试，不能先 FREE 从残留字段猜出的 token。

REPLY_NODE 复用 C06 变长回复处理并检查 NODE 模式。临时回复槽分配或输入复制失败保持 DELIVERED；正常回复不替 Server FREE 请求。CALL 摘表时归还事务持有，并在释放记录前保存独立结果快照；之后只使用 CALL 栈和 caller context，不再读 Node、事务或 Server。

PUBLISH 的查重、创建、输出与新 Node 失败撤销位于同一个 `mb_lock` 临界区。新建后输出失败撤销新 Node，已消费的 ID 不倒退；幂等查询输出失败则保留原发布。并发调用不能观察到一个即将因输出失败而撤销的临时 Node。

FREE 现在也先取 `mb_lock`，再取 `region.lock`，清槽时归还关联的 Node 持有。所有 `mb_region_free` 调用点都遵守这一顺序，包括 C06 的 CALL 输出 EFAULT 回滚；回复槽没有 Node，也保持统一锁契约。

用户复制期间不持有 region mutex；全局 mutex 仍跨复制，缺页可能阻挡其他调用。mmap/VMA 回调不取 `mb_lock`。这里没有将大锁移到复制外，也没有用这些静态规则宣称任意交错都无死锁。

## 9. owner 退出与本地对象生命

Server 最终 release 先终结尚未结束的事务，清理请求槽持有，再清除 Node 的 owner 并归还发布持有。仍等待 CALL 摘表的 Node 可以成为 owner=NULL 的孤立元数据；新调用拒绝旧 ID，最后的事务持有归还后才释放 Node。

这一顺序不额外保活 Server 进程，也不在 owner 释放后解引用旧 context。NODE_STATS 的 live_nodes 包括这种孤立元数据，而每上下文汇总只统计仍由该上下文拥有的 Node；退出后不能要求瞬间全局零，须等待调用者完成摘表。

另一个容易漏掉的空窗发生在用户态：Server 开始后台任务并 FREE 请求，Client 随后取消且 CALL 摘表，内核就可能只剩发布持有。REMOVE 此时可以成功，但后台任务仍在访问 counter。

因此 Server 在执行业务前必须按 node_id/cookie 查本地表并取得活动持有，结束后归还。只有 REMOVE 成功且本地活动数为零才可销毁本地对象。并发实现须用同一用户态锁协调“查表并取得持有”与“注销、销毁”；单独读取裸计数仍有竞态。

示例采用同步分发，不实现通用引用框架。借用请求映射中的数据只能使用到 FREE 前；若后台任务需要这些字节，必须先复制到自己管理的存储。Node 持有既不保活用户对象，也不保活已经 FREE 的 payload。

## 10. 普通 C 实验：把两种生命分开观察

前提是普通 C 编译器和 Python，无需设备、root 或 VM。更完整的模型说明见[Node 模型实验](../labs/node-model.md)。在仓库根目录运行：

```sh
mkdir -p build
cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic examples/models/nodes.c -o build/nodes
./build/nodes method
./build/nodes registry
python3 -m unittest discover -s tests -p test_node_model.py -v
```

`method` 的 expected 为 1、1、2，observed 为 1、2、3；这是预期反例，程序退出 0 表示成功复现错误方案，不表示错误方案正确。`registry` 为便于区分，让 A 从 0、B 从 100 开始；交错访问时同 code=1 得到独立结果 1、101、3，读取第二个仍为 101；add 得 42，未知 code 得 ENOSYS。重复 cookie 不多分配 Node，结束应无活跃 Node。

再执行一条先 FREE、后回复的 trace：

```sh
./build/nodes trace call remove deliver remove free_request remove reply reap remove destroy
```

trace 初始已发布一条 Node。前三次 remove 都须为 EBUSY：排队中、领取后、只剩事务持有时均不能撤销。reply、reap 后 remove 才成功，destroy 才能销毁本地对象。观察每行 published/tx_refs/slot_refs/refs，不能只看最终摘要。

相反顺序和本地活动持有分别运行：

```sh
./build/nodes trace call deliver reply reap remove free_request remove destroy
./build/nodes trace call deliver local_begin free_request cancel reap remove destroy local_end destroy
./build/nodes trace call deliver owner_exit reap
./build/nodes trace remove destroy publish call_stale remove destroy
```

第一条中，CALL 摘表仍不能替代请求 FREE。第二条第一次 destroy 须 EBUSY，即使内核 Node 已撤销，local_active 仍为 1；local_end 后才能销毁。第三条展示退出后暂留的事务持有，reap 后清零。第四条展示相同 cookie 重新发布后，旧 ID 不能调用新实例。

发布输出失败也要区分已有与新建：

```sh
./build/nodes trace publish_fault remove destroy publish_fault
```

第一次 publish_fault 查询初始已有 Node，虽然输出 EFAULT，发布仍在；remove/destroy 后第二次创建再失败，须撤销新 Node，最终 live_nodes 为 0。模型把故障编码成事件，并没有真正调用 copy_to_user。

失败判据包括：状态混合、busy 撤销成功、撤销失败后对象不再可用、持有负数或不守恒、旧 ID 命中新对象、本地活动未结束就销毁，以及测试异常退出或超时。trace 的预期拒绝以 status 字段表现，不能要求每个 EBUSY 都使进程退出非零。

模型保留静态账目以打印已回收对象，没有实际 free 后解引用；它没有内核锁、真实复制、VMA、线程竞争或进程死亡。最终零账目只能支持模型结论。清理手动产物用 `rm -f build/nodes`；测试临时目录由测试自行清理。

模型的 local_begin/local_end/destroy 是用户态保活策略；其中局部 ESTALE、EPIPE 等符号不是新增内核 errno 承诺。模型可能先取得临时槽持有再撤销以展示守恒，而驱动在请求复制成功后才把 Node 绑定到槽，步骤粒度并不相同。

## 11. VM 验收：由反例走到真实资源路径

前置环境沿用 [P04](p04-environment.md)，宿主只启动隔离客户机，不加载教学驱动，也不直接运行客户机专项测试。准确入口为：

```sh
make test
make vm-test
make vm-test-kvm
```

构建入口见 [build-rootfs.sh](../../tools/qemu/build-rootfs.sh)，客户机流程见 [init](../../tools/qemu/init)，专项代码见 [node_test.c](../../tests/kernel/node_test.c)。当前完整专项和旧回归已通过；下表区分 NODE 专项与共享路径回归，不以初始 REGISTER 探针替代证据。

| 问题 | 确定性步骤与预期 | 失败判据 |
| --- | --- | --- |
| 相同方法是否访问不同实例 | 两 counter 使用相同 code，交错调用并解码结构化回复；add/未知方法走用户分发 | 状态混合、只比较 code、驱动代做业务。 |
| 多对象业务是否真的跨进程 | 独立 fork Server 自行 open/mmap/publish，经 pipe 给 Client 两个 ID；两轮 4 笔同方法调用反序回复，最终 4/30 | 仅同进程不同上下文测试、只传 ID 未执行业务或结果串线。 |
| 发布与撤销是否稳定 | 同 cookie 幂等、容量 16、零 cookie、旧 ID 不复用、错误拥有者和模式拒绝 | 重复发布增加持有、EBUSY 后不可调用或权限检查失效。 |
| 两份在途持有是否各自需要 | queued、delivered、先 FREE、先回复四种同步点分别 REMOVE | 事务未摘表或请求未 FREE 就撤销成功。 |
| 持有是否错误落到别的 Node | A 有在途调用时 A REMOVE EBUSY，闲置 B 可成功 REMOVE | 按 owner 整体忙碌或把 A 的持有错误计给 B。 |
| 回复是否不必要地保活目标 | Client 保留回复 token 时撤销空闲目标，Node 归零；之后仍可解码并 FREE 回复 | 回复槽使 Node 持续忙碌，或移除 Node 破坏旧回复。 |
| 取消后请求身份是否保留 | delivered 后取消并等待 CALL 退出，REMOVE 仍忙，Server FREE 后才成功 | Client 取消替 Server FREE，或迟到回复复活记录。 |
| 失败是否有准确回滚 | 新/幂等 PUBLISH 输出故障；CALL 输入、RECEIVE 输出、REPLY 输入、CALL 输出故障 | 新 Node 残留、原发布丢失、重试失败、槽或持有增加。 |
| 双向缓冲是否继承 C06 | 同次 C06 回归测回复区满后 FREE 重试/空回复、Client munmap 后 ENXIO/空回复；NODE 专项测回复输入与输出 EFAULT | 失败提前终结、空回复仍要求槽、输出 EFAULT 遗漏未交付槽。未把共享路径回归称为 NODE 模式逐项穷举。 |
| 最终退出是否释放正确责任 | Server queued/delivered 最终释放；独立 Client queued/delivered SIGKILL | 保留继承 fd/VMA 掩盖 release，或 Node/事务/槽最终残留。 |
| 原路径是否继续成立 | 同次 VM 完成所有旧 RPC、映射、Parcel、Transaction 回归 | 只看 Node 新测试通过而忽略旧模式失败。 |

用 pipe、领取报告及计数建立同步，不靠 sleep。先等待 CALL/join/waitpid 完成，再检查 Node、事务、槽、region、context；需要观察孤立 Node 时应明确所处同步点，不能要求调度中的瞬时值固定。

运行器非零退出、超时、缺全局 MINI_LAB_PASS、内核告警或资源账目不符都属于失败。记录构建版本、TCG/KVM、完整日志和哈希。全矩阵未完成前不更新为已验收；没有 KASAN/lockdep、真实 kmalloc ENOMEM 或 64 位耗尽注入时，应逐项说明未覆盖。

清理先终结或取消剩余调用，回收线程/子进程，归还已交付请求和回复 token，REMOVE 空闲 Node，再解除映射、关闭 fd、核对最终账目并卸载模块。失败客户机销毁后重建，不复用半失败状态；零计数也不是通用泄漏或任意竞态证明。

## 12. 回到固定版本的真实 Binder

驱动阅读固定上游 Linux **v6.6**；用户态固定 AOSP **android-14.0.0_r1**，`frameworks/native` 提交 **bfcf75076e562945bae131a4929f29a90d0d2481**。获取和哈希分别见[驱动基线](../reference/android-binder.md)、[用户态基线](../reference/aosp-userspace.md)。下表核对本地缓存源码，只讨论 kernel Binder 分支，不表示已经构建运行该 Android 系统。

| 教学问题 / mini 机制 | 固定源码入口 | 相同点与不同点 |
| --- | --- | --- |
| 同一拥有者如何查 Node / 整数 cookie 注册表 | v6.6 [binder_get_node_ilocked，685 行](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L685)、[binder_init_node_ilocked，724 行](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L724) | 真实按 owner proc 内 ptr 查红黑树，另存 cookie，并有临时持有；mini 按全局 ID 查链表、按 owner/cookie 查重，不是同一个键或引用协议。 |
| 对象传到另一进程如何表示 / 本章尚未转换对象 | v6.6 [binder_translate_binder，2129 行](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L2129)、[binder_translate_handle，2182 行](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L2182) | 本地对象转换为接收进程 Ref/Handle；回到 owner 可还原 ptr/cookie。真实还检查 cookie 与转移权限，mini 普通 payload 不执行这种转换。 |
| 请求如何保持目标 / 事务与槽各自持有 | v6.6 [binder_get_node_refs_for_txn，2857 行](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L2857)、[buffer.target_node 赋值，3266 行](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L3266)、[buffer release，1935 行](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L1935) | 真实取得目标强持有及构造期间的临时持有，buffer 释放时归还目标强持有；不能把 mini 三个计数与真实字段逐一等同，也不能说回复一到就释放全部持有。 |
| 对象身份如何回到拥有者 / RECEIVE_NODE 的 id/cookie | v6.6 [binder_thread_read，4638 行](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L4638) | BR_TRANSACTION 的 target.ptr/cookie 来自目标 Node；驱动交付标识而非调用用户函数。mini 返回非指针 node_id/cookie。 |
| 用户态本地标识究竟是什么 / 整数 cookie | 固定 AOSP [Parcel::flattenBinder，272 行](https://github.com/aosp-mirror-neo/platform_frameworks_native/blob/bfcf75076e562945bae131a4929f29a90d0d2481/libs/binder/Parcel.cpp#L272) | 本地对象的 binder 字段为 `local->getWeakRefs()`，cookie 为 `local` 的 BBinder 地址，两者不是同一个指针；mini 刻意不传地址。 |
| 如何进入本地业务 / 本地活动持有 | 固定 AOSP [IPCThreadState，1417 行](https://github.com/aosp-mirror-neo/platform_frameworks_native/blob/bfcf75076e562945bae131a4929f29a90d0d2481/libs/binder/IPCThreadState.cpp#L1417) | 先通过 target.ptr 的弱引用控制块 attemptIncStrong，再经 cookie 调用 BBinder::transact，结束 decStrong；实际业务在用户态，不是驱动解引用 cookie。 |

`binder_init_node_ilocked` 还分配 debug_id，但普通远程事务在 [3016 行](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L3016)按发送进程的 target.handle 查 Ref；AOSP [writeTransactionData，1231 行](https://github.com/aosp-mirror-neo/platform_frameworks_native/blob/bfcf75076e562945bae131a4929f29a90d0d2481/libs/binder/IPCThreadState.cpp#L1231)也填 target.handle。**真实 debug_id 不是远程 target Handle**，不能用日志编号替代进程私有引用。

mini 主动 PUBLISH/REMOVE、EBUSY 撤销、全局 ID、唯一 Server 和三种简化计数都是教学选择，不是 BC/BR 兼容接口。真实强弱引用通知、对象表转换、死亡通知和引用持有仍有更多协议；这里仅核对当前设计问题所需片段。

## 13. 练习与下一步

1. 在先 FREE 后回复与先回复后 FREE 两条路径中，逐步写出 R 的三项；删去哪一项会产生什么空窗？
2. Client 已取消、内核 REMOVE 成功，后台任务为什么仍可能访问本地对象？把查表、取得活动持有、注销和销毁标出各自同步点。
3. 设计一种“立即停止新调用、等待旧调用退出”的撤销协议。它与本章 EBUSY 后仍可调用有何差别，需要新增哪些状态和等待规则？
4. Client B 将某个数字直接交给 C：本章全局 node_id 为什么仍能寻址？若 C 没有被授权持有引用，数字本身为何不足以表达这个约束？

下一章从第四题继续：将可猜的全局身份换成每个上下文私有的 Ref/Handle，让跨域传递经过明确的引用转换。对象是谁、谁持有它、哪个调用正在使用它，将继续是三个分别核对的问题。
