# M3：Node 对象身份开发验收

日期：2026-11-06。上一版为 `dev-c06-20261106`（`a24a2cd`），本版检查点为 `dev-c07-20261106`。本报告覆盖单 Server 多对象，**不表示 M3 对象网络或第 07 章正式发布**。设计、正文和实验分别见 [Node 决策](../design/nodes.md)、[第 07 章](../chapters/07-nodes.md)、[普通模型](../labs/node-model.md)。

## 1. 需求、选择和成本

两个 counter 执行同一个 increment 方法，却需要独立状态；事务 ID 只回答哪次调用，方法号不能替代对象身份。选择 owner 内部非零 cookie 去重、驱动单调 node_id 寻址，由 Server 按 node_id/cookie 查本地表分发。保留唯一 Server，新增独立 NODE 模式；ID 通过 pipe 提供，不引入名字注册表或私有 Ref。

Node 存活由发布、事务记录、请求槽三类持有相加决定。REMOVE 只有发布持有时成功，否则 EBUSY 且继续可调用。Client 取消后已交付请求槽仍能保护目标身份；请求 FREE 后尚在等待的事务也独立保护元数据。回复槽不持有目标 Node。用户本地业务对象的生命另由用户态活动持有保护，不能把内核计数当作后台任务完成证明。

成本包括每对象元数据、全局线性查找、16 对象上限、三类归还路径及全局锁跨用户复制。ID 可猜、知道 ID 的设备访问者即可尝试调用；当前不是私有 Handle 或完整能力授权。

## 2. 环境、命令和最终结果

沿用 Linux v6.6、BusyBox 1.36.1、x86_64 和[环境清单](../../tools/kernel/manifest.md)。本版 initramfs SHA-256 为 `c9b08974c5c84769afd5cd87b8bb7a24509d748ccf4ef3a87189fce8d13ad698`。

```sh
make test
make -C build/kernel/out M="$PWD/driver" W=1 modules
bash tools/qemu/build-rootfs.sh
LOG_TAG=node-accepted-tcg bash tools/qemu/run.sh
ACCEL=kvm LOG_TAG=node-accepted-kvm bash tools/qemu/run.sh
```

用户态 **75 项全部通过**，其中 Node 模型 15 项，包含 49 个核心有序事件对；驱动 `W=1` 编译通过。最终 TCG、KVM 均退出 0，同次执行此前全部设备、RPC、映射、Parcel、事务和 add 演示回归。卸载时 `live_contexts=0 requests=0 regions=0 slots=0`、`nodes_empty=1`；运行器未发现其匹配规则覆盖的内核告警。

```text
NODE_STAGE publish=idempotent capacity=16 output_fault=new_rollback,existing_preserved
NODE_STAGE counters=independent same_code=1 values=4,30 add=42 unknown=business_ENOSYS
NODE_STAGE remove_busy=queued,delivered,transaction_only,slot_only cancellation=queued,delivered id_reuse=prevented
NODE_STAGE retained_reply=does_not_pin_node read_after_remove=ok
NODE_STAGE target_holds=isolated idle_node_remove=ok
NODE_STAGE modes=isolated faults=call,receive,reply,output retry=ok rollback=clean
NODE_STAGE client_SIGKILL=queued,delivered transaction_refs=clean delivered_slot=explicit_free
NODE_STAGE server_exit=queued,delivered old_target=ENOENT orphan_nodes=clean
NODE_STAGE cross_process=4 ids=pipe same_code=1 counters=4,30 reverse_replies=ok
NODE_TEST_PASS counters=2 capacity=16 refs=validated resources=clean
```

## 3. 各实验支撑什么结论

| 问题 | 实际证据 |
| --- | --- |
| 方法号能否区分实例 | 普通模型实际共享 counter，得到错误 1/2/3；注册表按 Node 分发得到 A=1/3、B=101，add=42。固定参数，不声称通用业务库。 |
| 跨进程多对象是否可用 | fork Server 自行 open/mmap/register，发布两个 counter，通过 pipe 返回 ID；父进程两个线程共用 Client fd，先确认 queued=2 再让 Server 领取。两轮共 4 笔相同 code=1，Server 反序回复，双方独立解码并核对最终 4/30。另有同进程不同上下文的详细分发测试，不混作跨进程证据。 |
| 发布是否幂等 | 同 cookie 返回原 ID，owned_nodes 不增加；满 16 仍可幂等查询，第 17 个新对象 EAGAIN。移除一枚再发布恢复容量，ID 继续增大。 |
| 发布输出失败如何回滚 | 新 cookie 的只读输出触发 EFAULT，Node 数恢复 0；已有 cookie 的同类故障保留原 Node/ID。坏控制指针、零 cookie、坏统计/撤销指针分别拒绝。 |
| 两种在途持有是否必要 | queued、delivered 时 REMOVE EBUSY；请求 FREE 后 transaction_refs=1 仍忙；CALL 返回而请求未 FREE 时 slot_refs=1 仍忙。分别归还后可撤销。 |
| 持有是否属于正确目标 | 同时发布 A/B，仅调用 A：A 撤销 EBUSY，闲置 B 可立即移除，剩余汇总 1 Node/1 事务/1 槽；随后领取核对仍为 A。 |
| 回复是否错误保活目标 | 请求已 FREE、CALL 已返回，Client 保留回复 token。REMOVE 使 live_nodes=0，回复槽仍为 1；此后仍可解码旧回复并 FREE。 |
| cookie 复用会否复活旧身份 | 撤销后重发相同 cookie，获得更大的 node_id；旧 CALL/REMOVE ENOENT，新目标继续调用。模型另保存旧描述符并拒绝其分发，不冒充实际用户堆地址复用实验。 |
| 取消后谁持有请求 | queued SIGUSR1 后槽自动回收；delivered 取消后事务持有消失、槽仍阻止 REMOVE，迟到回复 ENOENT，Server FREE 后可撤销。 |
| 复制故障是否恢复 | CALL payload 坏地址/跨 PROT_NONE 部分复制 EFAULT，Node 仅保留发布；RECEIVE 输出 EFAULT 保持 queued/持有；REPLY 坏/部分输入失败保留事务并回滚临时回复槽，随后正常回复。CALL 只读输出 EFAULT 自动回收未交付回复槽。 |
| 模式和身份检查 | 错误上下文操作 EPERM；NODE 与旧 TRANSACTION 的 CALL/RECEIVE/REPLY 双向 EPROTOTYPE；合法目标自调用 EDEADLK；零目标、内外 reserved、size=0/4097 拒绝。未知业务 code 由 Server 返回 -ENOSYS，ioctl 成功。 |
| 两端进程退出 | Client 子进程自行 open/mmap，在 queued/delivered 时 SIGKILL，waitpid 确认，region/context 清理；已交付槽由存活 Server FREE。独立 Server 在两状态最终 munmap/close，Client EPIPE，等待 CALL 后 Node 归零、旧目标 ENOENT。 |

C06 同次回归另验证回复区满、FREE 恢复、空回复、Client munmap 等共享传输路径；这些没有在 NODE 模式逐项复制一套专项，不把共享代码覆盖称为所有模式排列的穷举。

## 4. 失败基线与审查

先在 C06 驱动中使用计划命令 16 运行注册探针，得到 `NODE_RED REGISTER errno=25`、客户机 FAIL、宿主退出 1。该失败只证明接口缺失，不证明完整 Node 行为已有逐项 RED。普通模型先因缺少实现而在 setUpClass 失败，零项业务测试执行；随后 15 项契约通过，不把初始化失败写成全部业务断言均观察到错误。

规范与驱动/UAPI 独立审查核对三持有、owner=NULL 孤立元数据、PUBLISH 输出原子回滚和所有 FREE 锁契约。模式错误优先级经核查后明确为现存非 NODE Server 优先 EPROTOTYPE。测试审查发现回复槽保留、多个 Node 的目标持有隔离缺乏专项，均补齐、复核关闭，并在最终两 VM 重新执行。源码对照及正文模型命令另外核查。

| 本地原始记录 | SHA-256 |
| --- | --- |
| `artifacts/node-driver-red.txt` | `45e63d0f0422c446b27faf3d8d23c1f6889a106f6e82cb902d5c9ec7272a4cfd` |
| `artifacts/node-model-red.txt` | `2bd4062fa90c29f462100e1bb8c709b6c247e574a1aa5325f3dea143a58e0ed3` |
| `artifacts/qemu-node-accepted-tcg.log` | `d681f1ced4b23bd7dac732aa81c676e9b66cd1db2d1397f66bbaa625d176415e` |
| `artifacts/qemu-node-accepted-kvm.log` | `ce2e424aaeb69ae0cc4f95f34dba6b131a503f2127ea5e127ad66c9891dec864` |

日志保存在忽略的 artifacts 目录，未作为外部下载包发布。标签固定当时代码和文档，按自己的构建入口重放，不混用其他检查点的模块。

## 5. Android 对照与边界

正文固定 Linux v6.6 的 owner 内按 ptr 查 Node、cookie 保存/冲突检查、对象与 Handle 转换、事务目标持有、buffer.target_node 及释放、向 owner 返回 ptr/cookie。固定 Android 14 `frameworks/native` 提交 `bfcf75076e562945bae131a4929f29a90d0d2481` 中，本地对象 binder 字段为 getWeakRefs()，cookie 为 BBinder 指针；用户态取得本地强持有后 transact。驱动不替用户调用业务。真实远程 target.handle 查私有 Ref，debug_id 不是公共目标编号。

mini 显式 PUBLISH/REMOVE、EBUSY 撤销、非指针 cookie、全局 ID 和三种计数均为教学选择。当前没有私有 Ref/Handle、对象描述符转换、ServiceManager、多服务进程、强弱引用通知或死亡通知，不能称为完成 M3。

没有真正 Node kmalloc ENOMEM、64 位 ID 耗尽、全部调度交错、KASAN/lockdep。owner 退出测试观察最终孤立 Node 回收，没有强制暂停 CALL 来动态固定观察短暂 orphan 阶段；此阶段由模型与静态持有分析解释。没有完整后台业务线程销毁框架，模型只展示本地活动责任；用户态同步销毁仍需自行实现。计数归零不是通用内存泄漏证明。

每项实验有 poll/计数截止、内层 alarm 与宿主截止。正常路径归还 token、join/wait、REMOVE 空闲 Node、munmap/close、卸载并关机；失败客户机丢弃，宿主不加载教学模块。普通模型临时目录自动清理，手动产物用 `rm -f build/nodes` 删除。正式章节发布、独立初学者复现及 M3 完整三进程引用传递门槛仍开放。
