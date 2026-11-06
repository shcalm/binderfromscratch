# 事务身份、一次终结与独立缓冲

对应 C06-01、C06-02。源码是 [transactions.c](../../examples/models/transactions.c)，验收是 [test_transaction_model.py](../../tests/test_transaction_model.py)，完整内核方案见[事务设计](../design/transactions.md)。这是确定性普通 C 模型，不调用设备、不启动线程、不创建实际映射，也不修改驱动。第 03 章已有请求 ID 和独立调用记录；本实验从它们继续推导同一连接的并发逻辑调用与双向缓冲责任，不宣称直到本章才出现事务。

## 为什么单个结果槽不够

同一个 Client 连接有两笔逻辑调用：第一笔期望 3，第二笔期望 42。两笔都使用业务 `sequence=7`。Server 先完成第二笔、再完成第一笔，而 Client 在两个回复都到达后读取结果。

若连接只保留一个共享 result 槽，第二笔的 42 会被第一笔的 3 覆盖。`slot` 模式真正执行这两次槽赋值，再分别为两个调用读同一个槽：

```text
event=slot_write logical_call=2 client=1 sequence=7 result=42 overwritten_call=none
event=slot_write logical_call=1 client=1 sequence=7 result=3 overwritten_call=2
event=slot_read logical_call=1 client=1 sequence=7 expected=3 observed=3
event=slot_read logical_call=2 client=1 sequence=7 expected=42 observed=3
```

第一笔碰巧读对，不能抵消第二笔关联丢失。这里的“并发逻辑调用”指两笔记录同时未完成，不是同时运行了两个 C 线程；反序完成由固定语句顺序控制。

每连接最多一个在途调用仍是合理协议：它减少资源和关联逻辑，但调用方须等待前一笔结束才能发下一笔。也可以为每笔调用建立独立连接，以连接身份关联结果，代价是更多连接及其生命周期。新增场景允许同一打开上下文多笔在途请求，因此本例采用独立事务 ID、每笔结果记录和一次终结规则。业务 sequence 可以重复、可能来自不可信输入，不足以替代内部身份。调用上下文用于确定“结果属于哪个调用”，Server 领取资格还需独立检查；有 ID 不代表任何 Server 都有回复权限。

## 运行与预期

前置条件为普通 C11 编译器和 Python 3，无需 root、VM、网络或 mini-binder 模块。在仓库根目录运行：

```sh
mkdir -p build
cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic examples/models/transactions.c -o build/transactions
./build/transactions slot
./build/transactions reorder
./build/transactions matrix
./build/transactions trace deliver reply reap output_ok free_request free_reply
python3 -m unittest discover -s tests -p test_transaction_model.py -v
```

所有上述命令返回 0；十一项测试必须全部通过。模型里的“拒绝某事件”是预期结果，日志用 `accepted=0` 表示，不使整个正确实验返回非零。内部不变量或固定情景结果错误则返回 1。未知模式、未知事件、缺参数返回 2，stderr 含 `usage:`，stdout 为空；trace 会先检查全部事件名，再开始执行。

`reorder` 创建事务 101、102，两者的 `client=1`、`sequence=7` 相同，`logical_call` 分别为 1、2。回复通过 ID 查找仍活跃的记录，不按数组中的“下一项”或 sequence 猜测归属。它先拒绝未交付的 101、伪造的 999 和错误 Server，再依次接受 102、101 的回复，并拒绝重复回复和成功后的取消。关键输出为：

```text
event=client_result id=102 logical_call=2 client=1 sequence=7 result=42
event=client_result id=101 logical_call=1 client=1 sequence=7 result=3
event=summary records=0 request_slots=0 reply_slots=0 terminal_total=2
```

中间还有完整 `event=step` 记录。`matrix` 恰好输出 52 条 step；`trace` 从事务 101 的 QUEUED 初始状态执行给定事件，最后输出资源汇总。trace 可以故意停在在途状态，因此汇总非零不自动算失败，需按所选情景判断。没有真实堆对象或内核资源随 trace 留在后台。

## 状态与事件契约

事务状态只有 QUEUED、DELIVERED、REPLIED、FAILED。前两个是非终态，后两个是终态。每笔已终结事务的 `terminal_count` 必须恰为 1，在途事务为 0；终态不得回到非终态，也不得被另一种终态覆盖。

下表的“保持”是事件适用而不改变事务状态，“拒绝”是 `accepted=0`。资源条件是契约的一部分，不能仅看事务状态决定是否允许 FREE 或交付输出。

| 事件 | QUEUED | DELIVERED | REPLIED | FAILED |
| --- | --- | --- | --- | --- |
| `deliver` | 转 DELIVERED，请求槽归 Server | 拒绝 | 拒绝 | 拒绝 |
| `receive_fault` | 保持，模拟描述符输出回滚 | 拒绝 | 拒绝 | 拒绝 |
| `reply` | 拒绝未领取 | 转 REPLIED，准备有数据回复槽 | 拒绝重复 | 拒绝迟到回复 |
| `reply_empty` | 拒绝 | 转 REPLIED，无回复槽 | 拒绝 | 拒绝 |
| `reply_fault` | 拒绝 | 保持，临时回复槽分配后撤销 | 拒绝 | 拒绝 |
| `cancel` | 转 FAILED，归还未交付请求槽 | 转 FAILED，请求槽仍归 Server | 拒绝 | 拒绝 |
| `server_exit` | 转 FAILED，归还请求槽 | 转 FAILED，最终清理请求槽 | 保持，清理 Server 请求槽 | 保持，清理 Server 请求槽 |
| `wrong_reply` | 拒绝错误 Server | 拒绝错误 Server | 拒绝 | 拒绝 |
| `output_ok` | 拒绝 | 拒绝 | 若输出 PENDING，交付回复描述符及槽 | 拒绝 |
| `output_fault` | 拒绝 | 拒绝 | 若输出 PENDING，撤销未交付回复槽 | 拒绝 |
| `free_request` | 拒绝，尚未交付 | 若槽仍归 Server 则归还 | 同左 | 同左 |
| `free_reply` | 拒绝 | 拒绝 | 若槽已交给 Client 则归还 | 拒绝 |
| `reap` | 拒绝，在途记录仍需保留 | 拒绝 | 若记录仍活跃则回收 | 同左 |

`server_exit` 表示最终服务释放，不是只 close 一个仍被 VMA 持有的 fd；每条账目只能执行一次，重复退出拒绝。终态上的退出只处理仍属于 Server 的资源，不再次终结事务，也不撤销已经接受的回复。记录回收后，再通过事务 ID 进行 deliver/reply/cancel 等操作会因 `unknown` 拒绝；独立的缓冲归还与已缓存输出的交付仍可以完成。

`receive_fault`、`reply_fault` 的 `accepted=1` 意味着这个受控失败事件在当前状态适用，**不表示真实 ioctl 返回成功**；原因分别记录为 `receive_rollback`、`reply_rollback`。`reply_fault` 特指先准备一个临时槽后输入复制失败，不能把它当成槽分配失败或真实 kmalloc ENOMEM 的实测。

matrix 的四行初始样本用真实事件前缀构造：QUEUED 为新发布记录；DELIVERED 由 deliver 达到；REPLIED 由 deliver、reply 达到；FAILED 由 deliver、cancel 达到。因而 FAILED 样本仍持有 Server 请求槽，REPLIED 样本持有 PREPARED 回复槽。矩阵覆盖全部状态×事件，并非枚举所有资源子状态的笛卡尔积；trace 另行检验已归还、已回收、空回复等资源条件。

## 三份不同的账目

每条 step 都输出事务状态、`terminal_count`、请求槽 owner、回复槽 owner、输出阶段和 record_live。

请求槽的 `DRIVER` 表示排队事务拥有，`SERVER` 表示已领取，`NONE` 表示已归还。领取后 Server 可以在回复前 FREE；正常回复和 Client 的交付后取消都不会代替 Server FREE。只有排队取消自动归还未交付槽，或服务最终释放统一清理。`request_frees` 至多为 1。

回复槽的 `PREPARED` 表示驱动已接受非空回复，但输出描述符尚未成功交给 Client；`CLIENT` 表示输出成功、由 Client 独立归还；`NONE` 表示没有槽或已经归还。`reply_empty` 可以成功终结但不申请回复槽，随后输出 `result=none`，不能凭空得到一个业务结果。每次 reply_fault 的临时申请和撤销分别增加 `reply_allocs`、`reply_frees`，即使事务仍在 DELIVERED。它们之差必须等于当前回复槽是否存在。

事务记录用 `record_live`、`record_reaps` 单独计数。`reap` 只允许终态且仍活跃的记录；它不自动 FREE 任一缓冲。为了打印回收后的状态，本程序保留静态观察账目，ID 查询不再把 record_live=0 的条目视为活跃记录。这是模型中的 tombstone，**没有实际 free C struct 后再解引用它**。输出阶段和缓冲账目代表真实实现中分别持有的已缓存控制信息与 token，不意味着真实驱动可以继续访问已经释放的事务指针。

因此，以下顺序合法：先领取并归还请求槽，之后回复、回收事务记录，最后向 Client 交付回复槽并由 Client 归还：

```sh
./build/transactions trace deliver free_request reply reap output_ok free_reply
```

最后必须为 `records=0 request_slots=0 reply_slots=0 terminal_total=1`。仅看到 state=REPLIED 或 record_live=0，都不足以推断缓冲已经释放。

## 竞争顺序与失败回滚

两种 cancel-vs-reply 顺序直接运行：

```sh
./build/transactions trace deliver reply cancel reap output_ok free_request free_reply
./build/transactions trace deliver cancel reply free_request reap
```

第一种先接受回复，取消必须拒绝，最后仍是 REPLIED；第二种先取消，迟到回复必须拒绝，最后是 FAILED，且不申请回复槽。两种都恰好终结一次并可独立收齐资源。这只展示串行化后的两种决定顺序；并未通过线程竞争证明锁实现正确。

输出描述符失败的情景：

```sh
./build/transactions trace receive_fault deliver reply_fault reply reap output_fault free_request
```

receive_fault 后仍 QUEUED、请求归 DRIVER；reply_fault 后仍 DELIVERED、回复槽为 NONE；重试 reply 才转 REPLIED。reap 后 output_fault 将输出阶段设为 FAULT，撤销 PREPARED 槽，但状态继续为 REPLIED、terminal_count 继续为 1。最终必须没有活跃记录或槽。这里区分“驱动接受回复”与“CALL 的控制输出成功”；失败用户不得读取部分控制结构，更不能自动重新提交已经可能执行的业务。本模型只记账，不模拟 `copy_to_user` 的部分写入。

下列序列分别检验服务先退出和回复先接受：

```sh
./build/transactions trace deliver server_exit reply reap
./build/transactions trace deliver reply server_exit reap output_ok free_reply
```

前者 FAILED，后者 REPLIED；服务最终退出始终清理其请求槽，不能把已经接受的成功回复改成失败。重复 output/FREE/reap 和终态上的 deliver/reply/cancel 均不得再次增加终结或回收计数。

## 验收方法、失败判据与清理

Python 测试使用显式契约表作状态转移 oracle，不读取 C 中的枚举、switch 或函数生成预期。它逐项检查 4×13 矩阵，并从 DELIVERED 开始遍历八种核心事件的全部 64 个有序事件对，检验终态单调性；再以独立情景断言资源 owner、反序结果、取消赢家、各次回滚与重复释放。每个 step 同时检查一次终结、请求释放至多一次、回复分配减归还等于存量，以及 record_reaps 与 record_live 的守恒。

测试在临时目录用严格警告选项编译，编译 timeout 为三十秒，每次模型运行 timeout 为五秒。未知输入必须在输出前拒绝；缺状态组合、结果与 ID 不匹配、计数变化两次、合法重试不能进行、故障后遗留模型槽或意外成功都使测试失败。超时和异常信号不是通过，不以运行耗时推断复制次数。

模型没有真实 fd、映射、进程或动态分配，不需要额外资源清理；临时编译目录由测试删除，手动产物可用 `rm -f build/transactions` 删除。最终零账目是这个事件模型的正确性证据，不是内核真实页、引用或事务记录零泄漏的证明。

## 模型边界

这里没有队列并发、互斥锁、线程等待、信号递送、真实用户复制、region/VMA 引用或内核内存回收。事务 ID 101/102 在情景中固定且不重复，没有实现真实全局 ID 分配、耗尽或回绕处理；wrong_reply 是显式的不合格角色事件，不能替代设备权限测试。每个 trace 只有一条事务账目，server_exit 表示最终释放对这一条依赖的效果，没有模拟多条记录同时清理的遍历。

请求与回复内容只用预设业务结果表示，没有编码 Parcel、传送特殊对象或建立对象引用。空回复只表示不占槽，不模拟完整业务 status。模型没有验证内核所有 interleaving、死锁、在锁外复制的安全性、真正 ENOMEM 注入或线程池行为。真实 Binder 的同步调用栈、线程绑定、BC/BR 命令也不是本程序接口；与 Linux v6.6 的逐段源码核查和 VM 实测由后续章节承担，不能用本次普通 C 通过结果替代。
