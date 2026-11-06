# 第 08 章 Ref 开发验收报告

日期：2026-11-06。上一版为 `dev-c07-20261106`（`3cc2f61`），本版为 `dev-c08-20261106` 开发检查点。规范见[引用设计](../design/references.md)，教学正文见[第 08 章](../chapters/08-references.md)，普通 C 实验见[模型手册](../labs/reference-model.md)。本报告不是第 08 章正式发布或 M3 全部验收声明。

## 1. 设计需求与代价

新要求是 B 可向 C 授予自己已有的对象，而 C 不能靠全局 Node ID 自行导入。全局名字、秘密 bearer token、接收者私有 Ref 表是三种不同方案；本章采用私有表和内核描述符转换。B 的 1 和 C 的 1 可以指不同 Node，转交必须先解析来源关系，再为目的上下文建立关系。

仍保留唯一 REF Server。为了让非 Server 的 C 能接收 B，显式设计 recipient-bound pending-slot grant；地址来自 QUERY.open_id，收据来自槽 token，实验通过可信 pipe 传递。这是教学引导旁路，不是假装实现 BC_TRANSACTION 或 ServiceManager。请求/回复另支持同一对象格式。

两种 Ref 单位分开：buffer_refs 属于逐对象槽账本，user_refs 属于主动 ACQUIRE。每条 Ref entry 固定 Node 一份，发回 owner 的 LOCAL 描述符另固定 Node 一份；目标事务、目标槽与嵌入对象分别持有。代价包括逐项清理账本、前缀回滚、死亡元数据、32 条本地表和 16 槽限额、Handle 可复用但无 generation。所有计数递增检查溢出，活性使用逻辑或。

沿用 `mb_lock → region.lock`。CALL_REF 控制输出仍在大锁下，事务摘表后记录由 CALL 私有，直到输出/回滚后才释放；这与 C06 锁外输出不同，选择和代价已在规范第 7 节说明。没有用大锁实现声称生产吞吐性能。

## 2. 环境与实际命令

沿用 Linux v6.6、BusyBox 1.36.1、x86_64 与[固定环境清单](../../tools/kernel/manifest.md)。initramfs SHA-256：`f9d9dc2a237bcb2916f044f76c97ee57ca3816eea2282ce2126e577854c4e15d`。

```sh
make test
make -C build/kernel/out M="$PWD/driver" W=1 modules
bash tools/qemu/build-rootfs.sh
LOG_TAG=ref-accepted-tcg bash tools/qemu/run.sh
ACCEL=kvm LOG_TAG=ref-accepted-kvm bash tools/qemu/run.sh
bash tools/reference/fetch-aosp.sh
```

用户态 **101 项全部通过**，其中 Ref 模型 26 项；模型包含 18 个关闭顺序。`W=1` 模块编译通过。最终 TCG、KVM 均退出 0，同次运行全部既有设备、RPC、映射、Parcel、Transaction、Node 和 add 演示回归。AOSP 四个缓存文件哈希校验通过；只读源码，没有构建 AOSP 用户态。

最终 VM 标记：

```text
REF_LIFECYCLE_PASS
REF_TRANSACTION_PASS
REF_THREE_PROCESS_PASS A=X,Y B=1:X,2:Y C=1:Y,2:X X=2 Y=102
REF_VALIDATION_PASS
REF_MODE_PASS
REF_DEATH_PASS
REF_CAPACITY32_REUSE_PASS
REF_TEST_PASS
mini_binder: device removed live_contexts=0 requests=0 regions=0 slots=0
mini_binder: nodes_empty=1
```

每组专项结束，observer 还核对 Node、事务、Ref entry、user/buffer/local 单位、pending 槽、映射与活槽归零；此时唯一 observer context 尚存，最终关闭和模块卸载才得到 contexts=0。运行器未发现其匹配规则覆盖的告警，不能据此推出任意竞态或所有泄漏均被排除。

## 3. 三进程实验的真实观察

A 是父进程 owner，B、C 为不同 fork 子进程，各自自行 open/mmap/ENABLE；继承的 fd 不能用于冒用父上下文。pipe 承担同步、收件地址、grant 和观察结果，实际调用与回复经 mini 驱动。A 保存 X/Y 的真实本地对象表，收到相同 code=93 后按 node/cookie 查对象并 increment，返回 4 字节 LE counter；预期值只用于验收，不参与构造回复。

| 顺序 | 实际查表/结果 | 证明什么 |
| --- | --- | --- |
| A 授予 B:X、B:Y、C:Y | B=1→X、2→Y；C=1→Y | 本地编号由接收表决定。 |
| C 直接用数字 1 | Y 从 100 变 101 | 复制 B 的数字可能调用成功但对象错了。 |
| C 直接用数字 2 | EBADF | 数字本身不创建引用。 |
| B 伪造 LOCAL X | EPERM | 知道全局 ID/cookie 不等于 owner。 |
| B 正式转交 HANDLE 1 给 C | C 得到 2→X | 保留 Node 关系，转换编号。 |
| C 按 2、2、1 调用 | X 得 1、2，Y 得 102 | 真正状态独立，既有 C1→Y 不受转交影响。 |
| B 成功 EXPORT 后退出 | C 仍可 IMPORT，增加一份 buffer 单位 | 槽持有属于接收者，不依赖发送者继续存活。 |
| A 退出后 C 领取旧 pending | 可 IMPORT；随后 CALL/ACQUIRE EPIPE | 死亡 Node 与可清理的引用元数据分离。 |

日志有 10 行 REF_TRACE，包含实际 PID/open_id、grant、接收 Handle、接收上下文 user/buffer/pending 总数与业务结果。例如首次 C:Y 授予经历 `0/1/1 → 0/1/0 → 1/1/0 → 1/0/0`，分别对应接受、IMPORT、ACQUIRE、FREE。转交 C:X 前已有 Y 的 user 持有，所以从 `1/1/1` 开始而非 0；日志是上下文总数，不是单个 Handle 的计数。

## 4. 异常与资源实验

| 场景 | 实际验证与边界 |
| --- | --- |
| grant 身份与状态 | foreign IMPORT ENOENT；pending FREE EBUSY；IMPORT 只读输出 EFAULT 保留 token/count 可重试；重复 IMPORT ENOENT；pending 阶段能 ACQUIRE，符合发布即授予。 |
| 两类持有 | 重复对象共用 Handle、各加 buffer；ACQUIRE 后 FREE 保留 user；RELEASE 归零删除；回复对象和 owner LOCAL 由相应槽独立保活。 |
| 精确前缀回滚 | `[有效, 坏 Handle]` 首项新建、以及 `[已有, 已有, 坏 Handle]` 重复旧 Ref，分别恢复原 user/buffer/Handle；EXPORT 控制输出 EFAULT 取消新投递。 |
| 请求/回复对象 | HANDLE 请求回 owner 为 LOCAL；REPLY 对象转换 EBADF 和坏地址/跨 PROT_NONE 页的部分复制 EFAULT，完整 REF_STATS/BUFFER_STATS 与 DELIVERED 不变，随后重试成功；CALL 只读控制输出 EFAULT 归还 reply 对象单位。 |
| 在途固定目标 | CALL 接受后释放旧 Ref、复用 Handle 给 Y；已接受事务仍返回 X。请求 FREE 后事务仍阻止 REMOVE；空回复不释放请求。 |
| 取消 | QUEUED SIGUSR1 自动清请求与对象；DELIVERED 取消保留 Server 槽，迟到 REPLY ENOENT，FREE 后归还 target/LOCAL。 |
| 退出 | owner 在 QUEUED/DELIVERED 最终 munmap/close，等待线程 EPIPE；已回复对象保留死亡 Node，RELEASE 只减 user、死对象 EXPORT EPIPE、最后 FREE 回收。真实发送进程退出另由三进程用例覆盖。 |
| 结构与全表检查 | 0/16/17 offsets、7/8/23 字节截断、错位/重复/重叠/倒序/越界、未知类型、flags、Handle 高位/cookie、LOCAL 零值/错 cookie；首项语义错误加后项结构错误时先返回结构错误且无增量。未列入 offsets 的类似对象字节保持原样。 |
| 有界资源与复用 | 16 pending 槽满后 EAGAIN；通过旧 owner 退出积累 31 条死亡 Ref，加一条当前活 Ref 达 32，重复活对象转换仍可成功，新对象 EAGAIN；释放最小空闲编号后复用并实际 CALL 新 Node。 |
| 模式、fd 与映射 | 旧注册/CALL_NODE 不能绕过 REF 表；有旧 Server 时新路径 EPROTOTYPE。继承 fd 的十个新 ioctl 和 mmap 均 EPERM，早于坏指针。munmap 后 IMPORT ENXIO、同 context remap 失败，pending 最终 release 清理。 |

容量实验没有放宽 REMOVE 的 EBUSY：死亡 Ref 保留旧 Node，允许下一唯一 Server 注册新一批对象，才使 32 条上限真实可达。新命令的布局大小、关键 offsetof 和 8 字节 alignment 由静态断言检查。

## 5. RED、独立审查与证据

旧驱动启用 REF 的实际 RED：此前 Node 测试通过，新 ENABLE_REF 返回 ENOTTY=25，VM 运行器退出 1。它证明缺少接口，不等于所有后来行为逐项在旧驱动失败。模型先使用可编译 stub，独立业务 oracle 实际得到空观察而失败；全套当时为 98 test 方法、43 个失败（含参数化子测试），保留文件明确标为手工摘录，不冒充原始完整日志。

规范审查修正一次 mmap 后不能重新映射领取 grant 的约定。驱动/UAPI 审查核对 ledger 前缀、重复对象、owner=NULL、FREE 锁序、open_id 不复用和控制输出责任，无未解决 P1/P2。测试审查后补实际 counter、REPLY 坏/部分复制重试及明确覆盖边界。

模型独立审查另发现：reap 后新 CALL 会覆盖尚未交付 reply 的输出入口，引用图虽守恒，责任仍丢失。新增回归实际观察第二次 CALL=OK、预期 EBUSY，再修单调用模型的输出限制；回复交付/回滚后才允许新 CALL，独立复跑 26 项通过。它说明计数守恒不能替代操作可达性验证。

| 本地记录 | SHA-256 |
| --- | --- |
| `artifacts/ref-driver-red.txt` | `d55466f596e42d0762d19bdddb9671e1214cf6361b6b4082e10d1fdc3d6471cd` |
| `artifacts/ref-model-red-excerpt.txt`，手工保留摘录 | `cbdf41a92de1bc1f0c672b3dea8d39e87866d996ed51b2fa21f90c2dc9ae7d6d` |
| `artifacts/qemu-ref-accepted-tcg.log` | `918cc44d5dac20bc7fec6446093e0f0f7fd7fc5a3beba36641e6b59138209cba` |
| `artifacts/qemu-ref-accepted-kvm.log` | `46161725a70ba1a5200a56aec9d794a3a76c0491642411ad1bfd6bb28f5192fe` |

这些日志在忽略的 artifacts 中，未作为外部下载包发布；开发标签固定源码与文档，重放须使用该标签自己的构建脚本。早期 REF_STAGE1_PASS 不作为本次最终矩阵证据。

## 6. Android 对应与未覆盖项

正文核对 Linux v6.6 的私有 desc 查找、同 Node Ref 复用、LOCAL→HANDLE、HANDLE→另一 HANDLE/owner LOCAL、target.handle 解析、buffer release 和失败前缀清理。用户态核对固定 Android 14 `frameworks/native` 提交 `bfcf75076e562945bae131a4929f29a90d0d2481` 的 flattenBinder/unflattenBinder kernel 分支。本章没有构建运行该 Android 组合。

没有通用强弱引用通知、死亡通知、ServiceManager、多 owner、业务权限框架或对象用户堆销毁自动协议。没有真实 Ref/Node kmalloc ENOMEM、U32_MAX 计数或 64 位 open/token/Node/事务 ID 耗尽注入、KASAN/lockdep、任意交错或并发 EXPORT 与收件最终 release 压力循环。REF 同 fd 的并发反序回复、Client SIGKILL 没有本模式专项；C06/C07 同次回归只证明其公共路径，不升级成 REF 全排列覆盖。纯空回复已测，REF 专项没有重复穷举所有“无映射/满槽+空回复”组合。

普通模型只有单条在途事务及输出责任，逻辑故障事件不证明真实复制、物理内存或内核并发。26 项模型测试的图重建、18 个关闭序列与 VM 用例互补，均不是完整状态空间穷举。资源计数归零也不是通用无泄漏证明。

客户机有 poll/计数截止和 alarm，宿主有 VM 总截止；正常归还槽、join/wait、munmap/close、卸载并关机，失败客户机可丢弃，不在宿主装载模块。本轮按用户要求收尾 C08 开发检查点后暂停；不进入第 09 章，正式章节发布、独立初学者重放及网站/电子书门槛仍保留在总任务表。
