# 方法号、对象身份与三份持有

对应 C07-01、C07-02，以及 C07-03 的抽象资源规则。完整规范见 [Node 设计](../design/nodes.md)，模型见 [nodes.c](../../examples/models/nodes.c)，自动验收见 [test_node_model.py](../../tests/test_node_model.py)。本实验是普通 C 程序，不访问设备、不创建线程、不运行 VM；它从第 06 章的事务与独立缓冲继续推导长期对象身份。

## 1. 方法相同，状态不一定相同

一个 Server 提供 counter A、counter B 和 add。两个 counter 都支持 code=1 的 increment，但 A/B 应保留各自状态。只用 code 查找一个共享 counter，会执行：

```text
event=method_call object=A code=1 expected=1 observed=1
event=method_call object=B code=1 expected=1 observed=2
event=method_call object=A code=1 expected=2 observed=3
```

`method` 模式实际递增同一个 C 变量，产生上述结果。第二次和第三次结果错误，不是不同线程竞争造成，而是根本没有保存“哪个 counter”的身份。给 A.increment、B.increment 各分配一个不同方法号也能服务于固定小程序，但对象创建/销毁会与方法表耦合；本实验要求同一方法作用于不同、持续存在的状态。

全局 ID 可以区分对象，但编号可猜，也可交给其他进程，不能因此宣称授权。方案选择为拥有者内部的非零 cookie 加驱动分配的 Node 身份；cookie 不采用地址，内核只存储和比较它。真实系统的私有 Ref/Handle 与对象传递属于下一章。本实验没有 ServiceManager，也不通过服务名称查对象。

## 2. 运行和成功标准

前置条件为普通 C11 编译器和 Python 3。在仓库根目录执行，无需 root、网络、mini-binder 或 VM：

```sh
mkdir -p build
cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic examples/models/nodes.c -o build/nodes
./build/nodes method
./build/nodes registry
./build/nodes trace call deliver free_request reply reap remove destroy
python3 -m unittest discover -s tests -p test_node_model.py -v
```

上述命令应退出 0，十五项自动测试全部通过。`method` 产生错误业务结果是预期反例；程序确认这种差异后仍退出 0。`registry` 必须通过实际注册表与本地对象表查找，依次得到：

| Node/cookie | code 与参数 | 结果 |
| --- | --- | --- |
| 101/11，初值 0 的 counter A | increment(1)，code=1 | 1。 |
| 102/22，初值 100 的 counter B | increment(1)，code=1 | 101。 |
| 101/11 | increment(2)，code=1 | 3，保持 A 原来的状态。 |
| 102/22 | get，code=2 | 101，B 未被 A 的第二次调用改变。 |
| 103/33，add | add(19,23)，code=3 | 42。 |
| 101/11 | 未知 code=999 | ENOSYS，value=none，由用户分发逻辑拒绝。 |

registry 先发布这三个对象，再次发布 owner=1/cookie=11 时返回 Node 101、`created=0`，实际分配数仍为 3。完成所有调用后撤销注册并结束本地对象，最后输出：

```text
event=summary live_nodes=0 published=0 local_active=0
```

本模式没有传输或等待；`dispatch()` 是模型中的用户态对象分发，不代表内核按 code 执行 counter/add。一个事务 ID 只标识一次调用，Node 101 则能跨多次调用保存目标身份；两种编号不能互换。

## 3. trace 的初始条件和公开输出

每次 trace 都重新开始：owner=1 活着，已发布 Node 101/cookie=7，一个对应本地 counter 存在，没有事务或请求槽。Node 初始只有一份发布持有。原始目标 ID 固定为 101，后续实际发布分配 102、103 等；失败的新发布也消耗一次 ID。本模型用 unsigned int 计数演示顺序，不模拟内核 64 位耗尽或跨模块重载持久身份。

每个事件输出一条 `event=step`，包含以下稳定字段；字段间用空格分隔，每项为 key=value：

| 字段 | 含义 |
| --- | --- |
| operation/status/accepted | 事件、符号结果和是否成功；仅 status=OK 时 accepted=1。预期 EBUSY/EFAULT 等不令整个 trace 失败。 |
| node/cookie/created | 最近观察的 Node 身份，cookie 固定 7；本事件是否新分配。失败新发布的 node 是内部观察值，不表示 ioctl 已向 Client 成功交付该 ID。 |
| published/tx_refs/slot_refs/refs | 发布、事务记录、请求槽三份持有；refs 为它们的和。 |
| live_nodes/node_allocs/node_frees | 当前仍存活 Node 数和累计申请/回收数；allocs-frees 必须等于 live_nodes。 |
| state/tx_live/terminal_count | 最近一笔事务的 NONE/QUEUED/DELIVERED/REPLIED/FAILED、记录是否仍在，以及本笔终结次数。新 CALL 重置此账目。 |
| request | NONE、DRIVER 或 SERVER；QUEUED 由驱动管理，成功 deliver 后归 Server。 |
| tx_acquires/tx_releases/slot_acquires/slot_releases | 累计持有与归还次数，分别相减必须等于 tx_refs、slot_refs。 |
| descriptor_node | 最近一次成功领取时的 Node 快照；跨 cookie 复用仍保留旧 ID，以检验陈旧描述符。 |
| local_alive/local_active/owner_alive | 本地对象是否存在、独立的本地活动持有、拥有者是否活着。 |

每步核对一次终结、持有守恒、请求归属及 owner 退出条件。模型最多同时观察一笔事务和一个请求槽；上一请求槽仍未归还时，新 call 返回 EBUSY 是**模型容量限制**，不是驱动允许 16 槽却只容纳一个 CALL。trace 可以停在合法在途状态，最终汇总非零不自动算失败。

## 4. 事件契约

| 事件 | 前提与效果 |
| --- | --- |
| publish | 已发布时幂等返回相同 ID，不增加持有。已撤销时须先完成本地 destroy，才创建同 cookie 的新代际；这个前置约束属于模型的本地生命周期管理。 |
| publish_fault | 模拟输出 EFAULT：已存在 Node 保留；新 Node 的发布回滚、申请和回收各加一，不留下持有。 |
| remove | 已发布且 refs=1 才成功，归还发布持有；否则 EBUSY，不改变可调用状态。已移除为 ENOENT。 |
| wrong_remove | 使用错误 owner 尝试移除已有 Node，得到 EPERM。 |
| call/call_stale | 分别按当前 ID 或初始 ID 101 查已发布目标；不存在为 ENOENT。成功则建立 QUEUED 记录和请求槽，各取得一份持有。 |
| call_fault | 模拟预留请求槽后输入复制失败，临时 slot_ref 取得后撤销，不发布事务、不保留槽，结果 EFAULT；这是终局资源模型，真实驱动在复制成功后才把 Node 绑定到槽。 |
| deliver/receive_fault | 只适用 QUEUED 活跃记录。deliver 交付请求槽与 node/cookie 快照，转 DELIVERED；receive_fault 为 EFAULT，状态与槽归属保持。 |
| reply | 只有活跃 DELIVERED 可成功，转 REPLIED；不归还事务记录或请求槽。不再可回复时 ENOENT。 |
| cancel | 活跃非终态转 FAILED；QUEUED 请求槽自动归还，DELIVERED 槽仍归 Server。终态或已摘表 ENOENT。 |
| reap | 活跃终态记录摘除，归还事务持有；不自动归还 Server 请求槽。 |
| free_request | 只有已交付请求槽可归还；QUEUED 返回 EBUSY，已经归还返回 ENOENT。 |
| local_begin | 按领取的 node_id/cookie 联合验证本地对象，并在请求仍由 Server 持有时取得活动持有。陈旧/缺失身份 ESTALE；同一描述符重复开始活动 EBUSY。 |
| local_end | 完成活动并归还本地持有，不改变 Node 三份内核持有；没有活动则 ENOENT。 |
| destroy | 仅在 REMOVE 已成功且 local_active=0 时销毁本地对象；否则 EBUSY。 |
| owner_exit | 表示最终退出，非仅 close 一个仍被 VMA 持有的 fd：终结非终态事务、清理槽、取消发布并清除 owner；已 REPLIED 不改为 FAILED。本地对象随进程一起消失。 |

status 是模型的符号事件结果，不是真实 ioctl 的 errno 实测。ESTALE、local_begin 的 EBUSY 及 destroy 的 EBUSY 是本地对象管理规则，不是新增驱动返回码。旧本地对象未 destroy 时 publish 返回 EBUSY，同样是示例用户态的保守策略：真实内核在 REMOVE 成功后允许重新发布 cookie，不检查 local_active 或本地对象是否已销毁。模型不会对 cookie 指向的地址做解引用。owner_exit 后 call 为 ENOENT、publish 为 EPIPE；它们仅帮助区分“目标已不存在”和“拥有者不能再发布”，不能作为对应内核 ioctl 错误码的承诺。

## 5. 两份内核忙碌原因，分别观察

先 FREE 请求，仍等回复：

```sh
./build/nodes trace call deliver free_request remove reply remove reap remove destroy
```

第一次 remove 时 `tx_refs=1 slot_refs=0`，必须 EBUSY。reply 后记录尚未 reap，第二次 remove 也 EBUSY；reap 后最后一次 remove 才成功。这证明请求槽归还不等于事务记录可以回收。

反过来，记录已经回收，请求槽仍在：

```sh
./build/nodes trace call deliver reply reap remove free_request remove destroy
./build/nodes trace call deliver cancel reap remove free_request remove destroy
```

两个序列第一次 remove 时均为 `tx_refs=0 slot_refs=1`，必须 EBUSY。正常回复和 DELIVERED 后取消都不替 Server FREE；归还请求槽后才能撤销。取消先终结时，后续 reply 必须 ENOENT。若取消发生于 QUEUED，则自动清理尚未交付槽：

```sh
./build/nodes trace call cancel remove reap remove destroy
```

取消后 slot_refs=0，但记录仍在，第一次 remove 仍 EBUSY；reap 后第二次成功。EBUSY 不是把 Node 标成 inactive：`call remove cancel reap call` 的最后一次 call 仍可进入 QUEUED，说明失败撤销保持发布。

## 6. cookie 复用不复活旧身份

```sh
./build/nodes trace call deliver free_request reply reap remove destroy publish call_stale local_begin
```

remove/destroy 后再 publish，cookie 仍为 7，Node 必须变成 102。call_stale 使用旧 ID 101，返回 ENOENT；local_begin 使用保存的旧 `descriptor_node=101`，尽管 cookie 相同，仍返回 ESTALE，不能投到新 counter。只按 cookie 查本地对象会丢掉这一代际区别。

本实验没有把用户地址作为 cookie，也没有物理分配后复用相同 C 地址；它检验的是不透明标识复用和 Node 代际，不是用户堆分配器的地址重用证明。

## 7. 内核引用不能替后台业务保活

```sh
./build/nodes trace call deliver local_begin free_request cancel reap remove destroy publish local_end destroy publish
```

Server 在分发前 local_begin，再 FREE；Client 取消并 reap 后，Node 只有发布持有。remove 可以成功，`refs=0 live_nodes=0`，但 `local_active=1`。紧接着 destroy 和 publish 都必须被模型的本地生命周期规则以 EBUSY 拒绝；local_end 后才能 destroy，再发布新 Node 102。这项延后重发策略由示例用户态选择，内核自己既看不到本地活动，也不会执行该项检查。

因此，成功 REMOVE 仅表明内核没有这三类依赖，不表明用户后台任务已经完成。真实应用需要自己的活动持有/同步规则；驱动不能从用户 C 对象或工作线程自动推断这件事。本模型用 alive/active 账目展示责任，没有实现通用引用框架或真正 free 本地对象。

## 8. 故障与退出账目

```sh
./build/nodes trace publish_fault remove destroy publish_fault publish
./build/nodes trace call_fault call receive_fault deliver cancel free_request reap remove destroy
./build/nodes trace call deliver owner_exit reap
./build/nodes trace call deliver reply owner_exit reap
```

第一条的首次故障不能移除原 Node；后一次新发布故障回收临时 Node，最终成功分配的 ID 为 103。第二条故障不能留下额外槽或提前交付，请求正常领取后仍能取消和清理。owner_exit 后发布和槽持有均为零，活跃记录单独保留 `tx_refs=1 live_nodes=1` 的孤立元数据；reap 才回到零。回复先接受的退出顺序必须保持 REPLIED，不得再次终结。

这里的复制失败是显式事件记账，不执行真实 copy_from_user/copy_to_user，也不模拟部分写入或缺页。call_fault 在模型中先取临时 slot_ref 再归还；真实驱动在输入复制成功后才给槽绑定 Node。两者都要求故障结束时无 Node/槽持有遗留，不表示中间步骤逐行相同。不同故障保留何种资源由契约规定，不以符号 EFAULT 证明真实内存保护已验证。

## 9. 自动验收、失败判据与清理

十五项 Python 测试使用固定业务预期与独立资源守恒断言，验证方法反例、注册表结果、重复发布、错误 owner、三类持有、两种取消、ID 代际、陈旧分发、本地活动、发布与输入/领取故障、退出与重复归还。另从 QUEUED 前缀遍历七种核心事件的全部 49 个有序事件对，核对终态单调和持有守恒；它不穷举全部 17 种事件与资源子状态。

资源负数/不守恒、结果来自错误 Node、重复发布增加分配、EBUSY 取消发布、旧 ID 复活、活动尚存却可 destroy、重复终结/归还或故障后泄漏，均为失败。未知模式、缺事件、未知事件及 method/registry 多余参数应退出 2，stderr 有 usage、stdout 为空；trace 在执行前验证全部事件名，不能先输出合法前缀再发现拼错。

测试用临时目录严格编译，编译截止三十秒，每次运行截止五秒。异常信号或超时不能当作预期错误；模型内部不变量失败退出 1。临时目录自动删除，手工产物可用 `rm -f build/nodes` 清理，没有后台进程、VMA 或设备资源。

模型采用静态数组和观察账目，引用归零仅把条目标成不活跃，便于打印历史状态；活跃查找排除这些条目。没有真实 kfree、更没有释放后继续解引用的许可。它不证明内核锁、owner 指针、线程调度、引用回收、内存泄漏、用户拷贝或任意 interleaving 正确。实际 Node 上限/64 位耗尽、真实 ENOMEM、Ref、管理器、对象传递、强弱引用及死亡通知不在本模型覆盖范围；VM 和固定 Android 对照由章节与报告另行验收。
