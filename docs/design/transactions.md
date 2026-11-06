# 第 06 章设计决策：事务终结与双向缓冲

状态：开发实现与完整 TCG/KVM 矩阵通过，证据和未覆盖路径见[验收报告](../reports/m2-transactions-progress.md)；尚未正式发布。前版为第 05 章开发检查点。先前已经有请求 ID、全局队列、每个 Client 独立等待和整数回复；本章从这些已存在的机制演进，不宣称直到本章才第一次出现事务。

## 1. 哪个新增场景使上一版不够

Server 需要把结构化结果而非一个整数交给 Client；同一个 Client 打开的上下文有两笔并发请求，Server 可以反序完成它们。业务 sequence 可以重复，不能当成内核回复凭据。同时，Client 取消、Server 回复、映射缓冲耗尽和进程退出可能争用同一笔记录。

| 方案 | 能解决什么 | 本次决定及代价 |
| --- | --- | --- |
| 每条连接只保留一个请求/固定回复 | 之前的 add 足够，清理简单 | 保留旧命令语义；新场景若仍用一个结果槽会覆盖/串线，不能靠业务 sequence 弥补。 |
| 用户态发送回复地址/共享可写邮箱 | 容量固定且双方可信时可实现 | 需要校验写权限、同步及共享所有权；不能把另一进程指针直接当本进程地址使用。 |
| 内核事务 ID + 每笔等待记录 + Client 接收映射 | 内核确定回复资格，反序完成仍可关联；回复有自己缓冲 | 选择；新增双向槽归属、输出失败回滚、终结竞争和内存上限。 |

事务身份由驱动单调 ID 提供，业务字段仍由应用解释。调用方只拿到自己 ioctl 对应的结果，Server 只可回复本上下文已经领取且未终结的 ID。内核不根据 Parcel 的 sequence 猜测调用者。

## 2. 新增模式和 ABI

旧 REGISTER/CALL、REGISTER_BUFFER/CALL_BUFFER/CALL_PARCEL 保持原语义，不把旧 Server 误当作会处理变长回复的新 Server。新增独立 TRANSACTION 服务模式，QUERY 提供新的能力位。所有保留字段必须 0，所有结构用固定宽度 UAPI 字段。

| 新命令 | 参数与行为 |
| --- | --- |
| REGISTER_TRANSACTION | 当前上下文已有接收映射、没有在途调用，注册为唯一 Server；依旧不是多服务或 Node。 |
| CALL_TRANSACTION | 40 字节：u64 data，u32 size，u32 reserved，输出 u64 token/u64 offset/u32 reply_size/s32 status。输入请求 1–4096 字节，调用方必须先建接收映射；无对象表。 |
| RECEIVE_TRANSACTION | 返回与既有 mb_buffer_request 相同的 id/token/offset/size/reserved，仅新 Server 模式可用。请求槽归 Server。 |
| REPLY_TRANSACTION | 24 字节：u64 id/u64 data/u32 size/s32 status。0–4096 字节回复；size=0 时 data 必须 0。有数据则复制到对应 Client 的接收槽。 |

命令号使用尚未占用的 12–15，按上表顺序；能力位 MB_FEATURE_TRANSACTION=8。回复的 status 是应用状态，负值不自动变成 ioctl errno。无 payload 回复返回 token=offset=reply_size=0；有 payload 才需 FREE。普通数据仍由用户态库编解码；此新传输也没有 Node/Ref/FD 转换，不把普通字节当对象。

新 CALL 允许同一打开上下文多个线程并发阻塞；每笔有独立等待记录与结果。旧 CALL/CALL_BUFFER/CALL_PARCEL 仍保持一次一个的 EBUSY 契约，不能与同上下文新调用混用。上下文维护 active_calls 与是否有旧式独占调用；注册 Server 时 active_calls 必须 0。active_calls 从发布持续到 CALL 的全部输出复制和回复槽交付/回滚完成，再于 mb_lock 下减少并清独占标记；不能在摘除事务记录后立即视为上下文空闲。线程池和嵌套回调仍留到第 10 章，内核不创建用户线程。

## 3. 显式状态和一次终结

事务状态为 QUEUED、DELIVERED、REPLIED、FAILED；终态不返回非终态。所有状态转移和队列/进行中计数受 mb_lock 保护。

| 当前状态 | 事件 | 新状态与资源 |
| --- | --- | --- |
| 尚未发布 | 分配/输入复制失败 | 直接撤销请求槽和记录，不占 queued/live，不唤醒 Server。 |
| QUEUED | RECEIVE 成功复制描述符 | DELIVERED；请求槽转 Server，事务继续等待。 |
| QUEUED | RECEIVE 输出 EFAULT | 保持 QUEUED，Server 可重试领取。 |
| QUEUED | Client 取消或 Server 最终退出 | FAILED；驱动归还尚未交付的请求槽并唤醒等待者。 |
| DELIVERED | 合格整数或新模式回复成功 | REPLIED；请求槽仍由 Server，变长回复槽开始准备交给 Client。 |
| DELIVERED | REPLY 输入复制、目标槽分配失败 | 保持 DELIVERED，返回错误给 Server，可重试或用空 payload 业务错误终结。 |
| DELIVERED | Client 取消或 Server 最终退出 | FAILED；请求槽仍由 Server/其最终 release 回收，迟到回复 ENOENT。 |
| REPLIED/FAILED | 重复回复/取消/退出 | 不得再次改变计数或结束同一事务；重复/不存在 ID 为 ENOENT。 |

回复资格检查仍先确定当前注册 Server；其他上下文即便知道 ID 也返回 EPERM。模式不符 EPROTOTYPE，包括新 Server 调用旧整数 REPLY、旧 Server 调用新 REPLY，以及旧/新 RECEIVE；已排队但未领取的 ID 不能回复。单调 ID 不复用，耗尽返回 EOVERFLOW。

Client 信号与回复争用 mb_lock：先成功终结者决定结果。已终结后收到信号不能把成功回复改为失败；先取消后回复不能重新产生结果。ERESTARTSYS 仍转换为 EINTR，避免系统自动重新提交业务。

## 4. 请求、回复和调用记录分别归谁

Server RECEIVE 成功后拥有请求 token，完成解码即可 FREE，与 REPLY 无隐式绑定。回复复制也使用 Client 的 region reserve，但此时 Client 尚未收到输出描述符，因此该槽先保持未交付。

Server REPLY 成功表示驱动已接受回复并终结事务，不保证 Client 的控制结构最终可写。Client 唤醒后在锁内取得独立结果快照、把记录从全局表摘除并结清 live_requests，解锁后释放记录；此后只使用 CALL 栈上的结果、token 和由 ioctl 文件生命周期保护的 caller context，不再访问事务或 Server。随后向自己的用户地址复制 token/offset/size/status；只有复制成功才将回复槽标为已交付，CALL 返回后 Client 可以解码/FREE。输出 EFAULT 时驱动撤销这枚尚未成功交付的回复 token，避免 Client 不知道 token 而漏槽。所有输出处理完成后才在 mb_lock 下减少 active_calls、清独占标记。业务已执行，不能自动重试。

用户必须在 CALL 返回后才使用输出；其他线程偷读尚未返回的控制结构不构成交付，猜到 token 提前 FREE 仍可能 EBUSY。单次 copy_to_user 可以部分修改输出，因此失败时用户不得使用其部分结果。测试用整页只读控制页，不能推论所有输出失败都完全不改写用户内存。

事务持有 caller 上下文地址，只在对应 CALL 尚未返回期间使用。执行中的 ioctl 持有文件引用，caller region backing 在最终 release 前存活；mb_lock 防止 Server 回复时该记录被取消线程删除。Server 引用只在未交付请求时保留，交付后沿用之前的清除规则。不能为省说明而无界额外持有 Server fd，导致死亡无法触发。

映射与 backing 生命周期继续依照[接收区设计](receive-region.md)：Client 并发 munmap 不能保证返回偏移仍可访问。REPLY reserve 时已无 Client VMA 返回 ENXIO；若分配成功后再 munmap，内核 backing 仍安全，但应用必须自行避免无效地址。close(fd) 与 VMA 持有仍可能延迟最终 release。

## 5. 有界失败和锁

每个打开上下文仍是 16 槽，每消息至多 4096 字节，已发布且尚未从事务表摘除的记录合计至多 64 条。live_requests 计这些记录，active_calls 按上下文额外覆盖结果输出阶段，二者口径不同。回复区满返回 EAGAIN，未映射 ENXIO，输入失败 EFAULT；这些失败不终结原事务，也不留临时回复槽。分配 token 的 EOVERFLOW 同样不终结，虽未注入到 64 位计数边界，仍保留零 payload 的终结出口。Server 可以修正输入重试；如果无法恢复，可发送零 payload 的业务失败，完全跳过 reserve 及 VMA 检查，在回复区满或已经 munmap 时仍能终结。这不是内建 RPC 超时。

请求和回复均在 mb_lock 下 reserve→copy→publish/rollback，region mutex 不跨用户复制；mmap/VMA 回调不取 mb_lock。成功发布计数分别计入各自接收上下文；被拒绝或失败复制不计，已接受但 Client 输出失败的回复仍算曾成功接受的回复 payload，其槽必须撤销。统计口径需在报告区分，不能与物理复制次数等同。

未来减少大锁必须首先提供对上下文与事务的稳定持有，不能只把 copy_from_user 移到锁外。恶意缺页拖住全局锁是当前明确保留的成本，本章不以状态机声称解决所有并发/死锁。

## 6. 实验要求

普通 C 模型先演示单结果槽覆盖，再用独立事务 ID 和状态机反序回复，拒绝重复/伪造/未交付回复；遍历每个状态的合法和非法事件，验证终结一次。模型不模拟真实线程调度和内核内存生命周期。

VM 必须覆盖：两个线程共用 Client fd、相同业务 sequence、反序结构化回复；重复/错误 Server/未交付 ID；请求槽和回复槽各自 FREE；回复部分 copy_from_user 失败后重试；Client 回复区 16 槽耗尽、EAGAIN 后空回复或释放恢复；Client munmap 后 ENXIO/空回复；Client 输出 EFAULT 自动收回回复槽；queued/delivered 取消、Server 退出、回复与取消的两种确定性顺序。用 pipe、领取点或计数协调，不靠 sleep；最终账目核对请求、槽、region 和上下文。

所有旧用户态/VM 测试回归。新调试测试无 KASAN/lockdep 时必须公开，不把零计数当通用泄漏证明。针对真正 kmalloc ENOMEM 的注入另在基础设施/稳定性任务完成；槽耗尽 EAGAIN 不伪称内存分配失败。

## 7. Android 对照和刻意差异

固定 v6.6 的 binder_transaction、transaction_stack、BC_TRANSACTION/BC_REPLY、BR_TRANSACTION/BR_REPLY 与 BC_FREE_BUFFER。真实 Binder 的同步栈、线程绑定和 BR_TRANSACTION_COMPLETE 不等于这里的共享 fd 多线程调用；本章只论证“事务身份、一次终结、请求/回复缓冲归属”。确认发送完成的驱动事件不是业务执行完成或业务成功。

具体源码入口、版本链接和运行范围由正文核对，不能将 mini 新 ioctl 直接称为 BC/BR 兼容实现。AOSP 普通 Parcel 编解码仍沿用上一章固定基线；本章不会假称已构建运行该版本 Android 系统。
