# 第 08 章设计决策：引用属于接收者的打开上下文

状态：开发实现规范；本文规定 C08 的行为，已运行证据及未覆盖项见[验收报告](../reports/m3-references-progress.md)。前置设计见 [Node](nodes.md) 和[任务清单](../plans/task-backlog.md)。不是正式发布声明。

## 1. 范围与授权边界

C08 保留唯一 Server A，A 可以发布多个 Node。B、C 是分别 open 的真实进程；引用私有性的实际边界是打开上下文，不是裸 PID。每个 ioctl/mmap 仍先检查当前 TGID 与 open owner，继承其他进程的 fd 不能冒用该上下文。

本章增加独立 REF 服务模式、私有 Ref 表、强引用教学子集和对象描述符转换。不增加 ServiceManager、多个同时服务的 owner、弱引用、死亡通知、FD 或通用用户态对象引用协议。C09 再处理发现与多个 owner。EXPORT/IMPORT 是教学旁路的定向对象投递，不是 Android 的 BC_TRANSACTION 传输，也不是服务业务调用。

| 方案 | 权限来自哪里 | 本章决定 |
| --- | --- | --- |
| 按全局 Node ID 任意导入 | 知道可猜的名字 | 拒绝；会绕过私有引用的授权边界。 |
| 一次性随机 bearer token | 持有不可预测 token | 可行但需要额外秘密、撤销和转发语义，本章不采用。 |
| 绑定收件 context 的 pending-slot grant | 发送者拥有本地 Node 或活 Ref，内核限定收件上下文 | 采用；复用现有只读 mmap 槽和 FREE 路径。 |
| 能力 FD 经 SCM_RIGHTS 传递 | 文件对象及 Unix FD 传输 | 留待 FD 章节。 |

QUERY.open_id 是公开收件地址，grant 是领取收据，二者都不是 bearer credential。实验通过既有可信 pipe 交换收件地址和 grant。知道 Node ID、cookie、他人的 Handle 或 grant，不能给自己创建 Ref。发送者只能把自己当前持有的引用授予指定接收者；接收者必须先 ENABLE_REF 并 mmap。持有 Ref 不等于获准执行所有业务方法，方法权限仍由服务检查。

## 2. 上下文、模式与固定限制

- 新增 MB_FEATURE_REF=32，ABI_VERSION 保持 1；所有旧结构、命令号与旧模式正常行为保留。
- 使用 QUERY.open_id 寻址，不增加第二套 context ID。open_id 从 1 单调递增，永不回绕、永不复用；分配器须在 U64_MAX 后使新 open 返回 EOVERFLOW，不能继续使用当前 atomic64 无条件递增方式。
- 存活 contexts 链表、ID 分配与寻址由 mb_lock 保护。release 先在锁内摘除可寻址身份；EXPORT 寻址、复制和提交均在该锁下，故不能与目标最终 release 形成悬空指针。
- ENABLE_REF 是不可撤销 opt-in，直到最终 release。它不注册 Server，不授予任何对象。首次需要已 mmap、没有 active_calls、没有旧槽；当前上下文若是旧模式 Server 则 EPROTOTYPE。已经 ENABLE 时重复调用幂等，不重新拒绝已有 Ref 槽或 active_calls，但仍需当前有 VMA。
- REGISTER_REF 要求已 ENABLE、已 mmap、没有 active_calls，且没有任何模式 Server。它注册唯一 REF Server；不隐式 ENABLE。PUBLISH_NODE/REMOVE_NODE 同时接受 NODE 和 REF Server，其他语义沿用 C07。
- 已 ENABLE 的上下文不能发起旧 CALL 或注册旧 Server；返回 EPROTOTYPE。已有非 REF Server 时，新对象投递/CALL_REF 返回 EPROTOTYPE。旧 CALL/RECEIVE/REPLY 也不能操作 REF 事务。REF Node 没有 CALL_NODE 全局 ID 旁路。
- 每 context 最多 32 条 Ref；每 `(context, Node)` 最多一条。Handle 是最小空闲正 u32，从 1 开始；0 永远无效。删除后可复用，不加 generation，不承诺陈旧整数在复用后仍会失败。
- 沿用每 context 16 个 4096 字节槽、65536 字节只读共享映射、最多 16 个对象 offsets、全局最多 64 条事务、每 Server 最多 16 个已发布 Node。pending grant、请求和回复共用槽配额；grant 不占事务配额。
- user_refs/buffer_refs 及 Node 内部计数为有检查的 u32；任何递增溢出返回 EOVERFLOW，禁止回绕。Ref 容量、槽容量和事务容量不足返回 EAGAIN；实际内存分配失败为 ENOMEM。

## 3. 定宽 UAPI

控制结构按现有 Linux 本机端序 ABI，所有指针以 u64 表示；offsets 指向本机端序 u32 数组。对象描述符是 payload 中的固定小端格式，不能直接把本机结构强转当作跨端序序列化。所有未用输出字段、尾部 offsets 和 reserved 输出清零。除明确列出的 reserved 外，纯输出字段的输入值忽略。

以下布局中的 u32/u64/s32 对应 __u32/__u64/__s32，不含 C 指针；每个含 u64 的结构按 8 字节对齐。实现和用户态测试应使用 sizeof/offsetof 静态断言。

```c
struct mb_ref_parcel {                 /* 24 */
    __u64 data;                       /* +0: input payload pointer */
    __u64 offsets;                    /* +8: input u32[] pointer */
    __u32 size;                       /* +16 */
    __u32 offsets_count;              /* +20 */
};
struct mb_ref_buffer {                 /* 104 */
    struct mb_buffer_request buffer;  /* +0: existing 32-byte structure */
    __u32 offsets_count;              /* +32 */
    __u32 reserved;                   /* +36: output zero */
    __u32 offsets[MB_MAX_OBJECTS];     /* +40: 64 bytes */
};
struct mb_ref_export {                 /* 40 */
    __u64 recipient;                  /* +0: QUERY.open_id */
    struct mb_ref_parcel parcel;      /* +8 */
    __u64 grant;                      /* +32: output recipient slot token */
};
struct mb_ref_import {                 /* 112 */
    __u64 grant;                      /* +0: input */
    struct mb_ref_buffer buffer;      /* +8: output */
};
struct mb_ref_call {                   /* 144 */
    __u32 target;                     /* +0: caller-private Handle */
    __u32 code;                       /* +4: uninterpreted method */
    struct mb_ref_parcel request;     /* +8 */
    struct mb_ref_buffer reply;       /* +32: output */
    __s32 status;                     /* +136: output application status */
    __u32 reserved;                   /* +140: input/output zero */
};
struct mb_ref_request {                /* 128 */
    struct mb_ref_buffer buffer;      /* +0: output */
    __u64 node;                       /* +104: target owner's local ID */
    __u64 cookie;                     /* +112: target owner's cookie */
    __u32 code;                       /* +120 */
    __u32 reserved;                   /* +124: output zero */
};
struct mb_ref_reply {                  /* 40 */
    __u64 id;                         /* +0: original request ID */
    struct mb_ref_parcel parcel;      /* +8 */
    __s32 status;                     /* +32: arbitrary application status */
    __u32 reserved;                   /* +36: input zero */
};
struct mb_ref_stats {                  /* 80; all output */
    __u64 global_ref_entries;         /* +0 */
    __u64 global_user_refs;           /* +8 */
    __u64 global_buffer_refs;         /* +16: HANDLE slot units */
    __u64 global_local_objects;       /* +24: LOCAL slot units */
    __u64 global_pending_slots;       /* +32: pending grant slots */
    __u64 current_ref_entries;        /* +40 */
    __u64 current_user_refs;          /* +48 */
    __u64 current_buffer_refs;        /* +56 */
    __u64 current_local_objects;      /* +64 */
    __u64 current_pending_slots;      /* +72 */
};
```

| 号 | 命令及 ioctl 方向 | 参数、成功效果 |
| --- | --- | --- |
| 23 | REGISTER_REF, _IO | 无参数，注册唯一 REF Server。 |
| 24 | ENABLE_REF, _IO | 无参数，对本 context opt-in。 |
| 25 | EXPORT_REF, _IOWR | mb_ref_export，转换到指定收件槽，返回 grant。 |
| 26 | IMPORT_REF, _IOWR | mb_ref_import，领取 pending grant，返回本地 buffer/offsets。 |
| 27 | ACQUIRE_REF, _IOW | u32 handle，增加一个 user_refs。 |
| 28 | RELEASE_REF, _IOW | u32 handle，释放一个 user_refs。 |
| 29 | CALL_REF, _IOWR | mb_ref_call，等待终态，返回 reply/业务 status。 |
| 30 | RECEIVE_REF, _IOR | mb_ref_request，唯一 Server 接收请求。 |
| 31 | REPLY_REF, _IOW | mb_ref_reply，向原调用者提交回复。 |
| 32 | REF_STATS, _IOR | mb_ref_stats，observer 可读，无需 ENABLE。 |

宏均使用现有 magic 'm'，名称为 MB_IOC_ 加上表内命令名。ACQUIRE/RELEASE 的参数是用户地址指向的 u32，不是 ioctl arg 中直接编码的数字。

EXPORT 和 CALL 请求 size 必须为 1..4096；EXPORT 允许 offsets_count=0，作为无对象的普通 parcel。REPLY 允许 size=0，但此时 data/offsets/count 均须 0，不分配槽、不要求调用者仍有 VMA。任何 envelope 的 count 必须 <=16；count=0 时 offsets 必须 0，count>0 时复制整个 u32 数组，坏地址 EFAULT。size>0 的 data 由实际 copy 检查，NULL 为 EFAULT。

mb_buffer_request 中 request.id 为事务 ID，EXPORT 没有事务，因此 IMPORT 返回的 buffer.buffer.id 为 0；reply 的 id 也为 0。token/offset/size 定位本 context 映射内的槽；无 payload 的 reply 整个 mb_ref_buffer 清零。每个 grant/request/reply 槽都保存 size、offsets_count 和 offsets 的内核快照，RECEIVE/IMPORT/CALL 的输出从该槽快照取得，不读取发送者旧指针。offsets 是 payload 起点以内的相对位置，不是虚拟地址。返回的 offset、size、对象表共同构成读取结果，不要求接收者猜测对象位置。

## 4. Payload 对象格式与转换

```c
struct mb_ref_object {    /* exactly 24 bytes, payload little endian */
    __le32 type;         /* +0: MB_OBJECT_LOCAL=1, MB_OBJECT_HANDLE=2 */
    __le32 flags;        /* +4: must be zero */
    __le64 value;        /* +8: owner node_id or private u32 Handle */
    __le64 cookie;       /* +16: LOCAL owner cookie; HANDLE must be zero */
};
```

每个 offset 必须 8 字节对齐、严格递增，描述符完整落在 size 内，不得与前一个描述符重叠。仅 offsets 标识的对象参与转换；其他字节一律是普通 codec 数据，即便外观恰好像描述符也不解释。

处理顺序固定为：复制控制头及整个 offsets 数组；在锁下取得目标槽；把普通 payload 一次复制到接收者只读映射的内核 backing；从该 backing 做全表结构验证；按表顺序进行语义查找和转换。禁止从发送者的可变 payload 再读描述符来判定转换。全表结构验证逐项按以下顺序检查：8 对齐、严格递增/不重叠、至少完整 8 字节 header，否则 EINVAL；type 未知为 EOPNOTSUPP；已知类型检查完整 24 字节，否则 EINVAL；最后检查 flags 与值域。flags 非零、HANDLE 高 32 位非零或 cookie 非零、LOCAL value/cookie 为零均为 EINVAL。已支持类型的真实尺寸是 24，不能沿用 C05 仅检查 8 字节 header 的规则；界限使用减法检查，避免 offset+size 溢出。全表结构失败不发生任何 Ref 增量。

| 发送表示 | 源权限检查 | 远程接收者 | 接收者恰为 Node owner |
| --- | --- | --- | --- |
| LOCAL(id,cookie) | 只能引用本发送 context 已发布且 cookie 一致的 Node | 找到/创建接收者 Ref，输出 HANDLE(dest_handle,0) | 输出规范化 LOCAL，增加 local_object_refs。 |
| HANDLE(handle,0) | 仅查发送 context Ref，user_refs 或 buffer_refs 非零，Node 仍活 | 找到/创建接收者 Ref，输出 HANDLE(dest_handle,0) | 输出 LOCAL(node.id,node.cookie)，增加 local_object_refs，不创建 Ref。 |

LOCAL 全局 ID 不存在为 ENOENT；存在但不属于发送 context 为 EPERM；属于发送者而 cookie 不一致为 EINVAL。不存在的 HANDLE，包括 0，为 EBADF。死亡 Node 的 HANDLE 在 CALL/EXPORT/对象转换时为 EPIPE；无权用另一个 context 的相同数字作后备查找。转换输出 flags 固定 0，不把 owner cookie 传给远程接收者。

相同 Node 已有 Ref 时复用 Handle，每个描述符独立增加 buffer_refs；重复对象不去重引用单位。只有第一次创建该 Ref entry 才增加 Node.ref_entries。最小空闲 Handle 分配只影响接收表，绝不把发送者整数直接复制过去。

## 5. 定向 grant

EXPORT 要求发送者 ENABLE；recipient 非零且不是自己，自投递 EDEADLK。按 open_id 找活 context，不存在 ENOENT；未 ENABLE 的收件人 EPERM；无当前 VMA ENXIO。收件人可以是 A、B 或 C，不要求是 Server。发送者没有业务 Server 身份也能转交其已持有的 Handle。

EXPORT 在接收者取得普通空槽，执行第 4 节转换，保存对象清理账本和 offsets，将槽标为 pending grant，然后输出 grant=token。token 沿用现有全局单调不复用的槽 token；耗尽 EOVERFLOW，不回绕。成功后引用属于收件槽，发送者 RELEASE/FREE/退出不撤回；不额外持有发送者 context/file，也不创建独立 grant Node 计数。EXPORT 的输出 EFAULT 则撤销新槽和该次所有引用增量，不留下待领投递。

IMPORT 只查调用者自己的槽，要求 token 对应 pending grant。不存在、0、属于其他 context、已成功领取、或是非 grant 槽均为 ENOENT，不扫描其他 context 以泄露 grant 存在性。成功输出后，原子标记 delivered、清 pending，global/current_pending_slots 减一。输出 EFAULT 保持 pending、buffer_refs 和 offsets 不变；再次 IMPORT 同一 grant 可以重试，不能先增加一轮引用。

授权提交点是 EXPORT 成功、CALL 发布 QUEUED 请求、REPLY 被接受形成有效 reply 槽；并非 IMPORT/RECEIVE/CALL 输出描述符的时刻。Ref 在 pending/queued 槽中已经存在，同一收件 context 的其他线程若猜到自己的 Handle，可凭 buffer_refs 调用或 ACQUIRE；接收者也已能在自己的映射看到 pending 字节。IMPORT/RECEIVE 只负责交付描述符和转变槽状态，不构成同一 context 内线程间的保密或权限边界。跨 context 仍不能查询或使用这些 Ref。所有转换、提交、输出回滚均由 mb_lock 线性化，其他 ioctl 看不到一个 EXPORT 的半转换状态。

pending grant 在 IMPORT 前执行普通 FREE_BUFFER 为 EBUSY；IMPORT 后 FREE 正常释放。没有超时、发送者取消或自动撤回接口，未领取槽持续占配额直至接收者最终 release。EXPORT 要求当前 VMA；IMPORT 也要求当前 VMA。保留 C04 每 context 只允许一次 mmap 的约束：munmap 后 IMPORT 为 ENXIO，不允许在该 context 重新 mmap；未领取槽只能由最终 release 清理。FREE、RELEASE、REF_STATS 不要求当前 VMA，已经交付的槽在 munmap 后仍可 FREE。

## 6. 引用、保活与事务

Ref 结构至少包含 context、Node、handle、user_refs、buffer_refs。有效 Ref 必须满足 user_refs 或 buffer_refs 非零，判断使用逻辑或，不能以可能溢出的 u32 加法判断；user_refs 初始 0。buffer_refs 在转换时增加，FREE 时减少。ACQUIRE 可从 user 或 buffer 任一存量取得用户持有，增加 user_refs；死亡 Node 返回 EPIPE。RELEASE 只减 user_refs；条目存在但 user_refs=0 时 EINVAL，不能偷减槽持有。死亡 Node 允许 RELEASE。两种计数均为 0 时立即摘除 Ref，归还一份 Node.ref_entries，再尝试回收 Node。

槽保存最多 16 项内核清理账本，每项记录实际 Node/Ref 与持有类别。不能在释放时再信任用户描述符、重新解析已释放 Handle 或扫描所有 Ref 猜测归属。重复项对应多份单位，必须各减一次。每次语义转换完成一个引用增量后立即记账；后续失败逆序撤销成功前缀。复用旧 Ref 的回滚只减本次 buffer_refs，保留原 user/buffer 数量和 Handle。新建 Ref 若回滚后归零才删除。

| Node 持有 | 获得/归还 |
| --- | --- |
| publication | PUBLISH 一份；REMOVE 或 owner 最终 release 归还。 |
| transaction_refs | 已接受 CALL 固定目标；CALL 摘表取得终态时归还。 |
| slot_refs | 已接受请求槽固定调用目标；对应 FREE/内部回滚归还。 |
| ref_entries | 每条活 Ref 一份，无论其 user/buffer 单位数；Ref 删除归还。 |
| local_object_refs | owner 收件槽中每个 LOCAL 对象描述符一份；槽 FREE/回滚归还。 |

Node 只有 owner=NULL 且上述四个计数字段均为 0 才能实际摘表释放。Ref/buffer 的持有必须真实保活 Node，不能仅增加统计。REMOVE 仅在其余四项均为 0 时成功；EBUSY 不撤销发布。NODE_STATS 旧布局不变，只报旧 publication/transaction/target-slot 视角，不能将它们的和误读成 C08 总持有；新增计数从 REF_STATS 观察。REF_STATS 在 mb_lock 下取一致快照，全部字段用 u64 汇总；current_user_refs 不可用 u32 累加多个接近 U32_MAX 的 Ref。

请求槽的 target Node 与嵌入对象持有分开，即便指向同一 Node 也不能合并或漏减。旧 C07 普通 reply 不固定 target；C08 reply 仍不固定已完成的调用目标，但可通过嵌入 HANDLE/LOCAL 对象持有 Node。pending grant、请求、reply 使用同一账本释放机制。

CALL_REF 查调用者私有 Handle，固定其指向的 Node 后发布事务；同时具有 user_refs 或 buffer_refs 即可调用，不强制先 ACQUIRE。用户释放 Ref、FREE 借用槽乃至 Handle 被复用，都不改变已接受事务存储的 Node 指针。RECEIVE_REF 返回目标的本地 Node ID/cookie 与嵌入对象表，服务仍按自己的表分发业务。REPLY_REF 目标只能来自该 DELIVERED 事务，不能另指定收件人。

事务状态、信号 EINTR 不重启、active_calls 覆盖输出、反序回复和同 context 并发 mapped CALL 继承 C06/C07。RECEIVE 输出 EFAULT 保留 QUEUED 及其槽；REPLY 的复制/转换失败释放新 reply 槽，事务仍 DELIVERED 可重试。请求 FREE 不隐式回复，REPLY 不隐式 FREE 请求。

QUEUED 取消内部释放未交付请求槽和嵌入对象；DELIVERED 取消不能收回服务已收到的槽，保留其 target/objects 直至 FREE。回复被接受而 CALL 控制输出 EFAULT 时，释放未交付 reply 槽及嵌入对象。取消与回复在 mb_lock 下线性化：先达到终态者生效，迟到 REPLY 返回 ENOENT。

内核持有只保护 Node 元数据。owner 用户态仍要用同一互斥协议协调查找、活动持有、REMOVE 与本地对象销毁；LOCAL 接收不实现 Android BC_ACQUIRE/BC_INCREFS 通知。不能因为 Node 有 Ref 就宣称任意用户态裸指针自动安全。

## 7. 最终 release 与锁序

继续使用 mb_lock -> region.lock；VMA open/close 不取 mb_lock。跨 context 操作持 mb_lock 保护 context、Node、Ref 和事务，region.lock 一次只取一个，不建立两个 region 锁的地址排序。复制可能在大锁内缺页阻塞，这是已知教学限制。

C08 的 CALL_REF 在事务摘表、归还目标事务持有之后，仍保留调用线程独占的记录并持 mb_lock 完成控制输出、reply 槽交付/回滚和 active_calls 归还，随后释放记录。它没有沿用 C06 的锁外控制输出；选择此处扩大临界区，是为把带对象 reply 的输出与引用回滚保持在同一线性化区间。代价是坏页输出可能阻塞其他上下文，本章不据此声称生产性能。摘表后不再有其他路径访问记录，也不能再通过已归还持有的 target Node 指针读取字段。

最终 release 的逻辑顺序如下，任何内部 FREE 都能处理 pending/未交付槽：

1. 在 mb_lock 下使 context 不再能被 open_id 寻址。若为 Server，清 server、以 EPIPE 终结未完成事务并唤醒等待者；先清理仍由队列持有的请求槽。
2. 对其全部 owner Node 置 owner=NULL，归还 publication，保留仍被 Ref/事务/槽持有的死亡元数据。后续处理不能再解引用该 context；不能在别人的 RELEASE 时通过旧 owner 指针找表。
3. 释放该 context 全部槽，包括 pending grants、未 FREE 的请求/reply 和每项对象持有，再删除其全部 Ref/user 持有。每次归还后按总持有检查回收。
4. 完成其余 context 资源释放、摘除并释放元数据。VMA/file 引用延迟 final release 的现有行为保留；测试要 munmap+close 或终结进程。

不同上下文最终 release 的先后必须都安全。A 先退出时 B/C 的死亡 Ref 保持 Node 到 RELEASE/FREE；B/C 先退出时引用归还但 A 的 publication 保持 Node。未完成 CALL 自己的文件引用和 transaction_refs 继续保护其上下文/目标，不因其他 context 的退出提前回收。

owner 退出后已形成的 pending grant/reply/已交付请求仍可 IMPORT/读取/FREE，其中 Handle 只代表死亡对象；不对清理操作额外要求 Server 存活。新 CALL、EXPORT 以及回复中的对象语义转换遇死亡 Node 为 EPIPE。

## 8. 错误优先级与可重试性

这是新命令的确定顺序；不是对旧命令历史优先级的全局重写。最先满足的错误返回，后续阶段不执行。系统资源真实分配 ENOMEM 可发生在进入相应阶段时，不宣称它能覆盖任意故意构造的多重错误。未知 ioctl 仍 ENOTTY。

1. 当前 TGID 不是 open owner：EPERM，早于任何用户内存访问。
2. 复制命令输入头 EFAULT；校验显式 reserved、size/count/pointer 组合和 recipient=0 得 EINVAL；复制 offsets 数组 EFAULT。纯输出 RECEIVE/STATS 不执行输入复制。HANDLE 数值是否有效留在查表阶段。
3. 模式/身份：除 STATS 外要求 ENABLE 的命令未 ENABLE 为 EPERM。REGISTER 先检查其他 Server/active_calls EBUSY，再检查 ENABLE EPERM、VMA ENXIO。首次 ENABLE 先拒绝本 context 旧 Server EPROTOTYPE，再查 active_calls/已有旧槽 EBUSY、VMA ENXIO；重复 ENABLE 只查 VMA ENXIO。CALL/EXPORT 若当前有非 REF Server，EPROTOTYPE。RECEIVE/REPLY 先查自己是否为 Server EPERM，再查 REF 模式 EPROTOTYPE。
4. CALL 查 target Handle：无条目 EBADF，死亡 EPIPE，owner 是自己 EDEADLK；再查存活 REF Server，不存在 ENOTCONN。检查本 context 的 exclusive_call EBUSY、调用者 VMA ENXIO、64 条事务 EAGAIN、事务 ID 耗尽 EOVERFLOW。EXPORT 按第 5 节依次查自收 EDEADLK、recipient 存在 ENOENT、收件 ENABLE EPERM。IMPORT 先按第 5 节 token 分类，再查 VMA ENXIO。ACQUIRE/RELEASE 先查 Handle EBADF；ACQUIRE 再查死亡 EPIPE、计数溢出 EOVERFLOW；RELEASE 再查 user_refs=0 EINVAL，不拒绝死亡。
5. RECEIVE 若无 QUEUED 则按原规则等待或被信号中断；REPLY 查符合本 Server 的 DELIVERED ID，否则 ENOENT。获取目标槽时依次检查映射 ENXIO、槽满 EAGAIN、token 耗尽 EOVERFLOW。REPLY 空 payload 跳过槽/映射检查。
6. payload 复制 EFAULT，然后按 offset 顺序做全表结构检查，第 4 节的 EINVAL/EOPNOTSUPP；全部通过后，按 offset 顺序做权限、死亡、Ref 容量与计数检查及转换。当前项先解析源并检查死亡，再为目的创建/复用 Ref；容量只在确需创建条目时检查 EAGAIN。后项失败逆序清理前缀和槽。
7. EXPORT、IMPORT、RECEIVE、CALL、STATS 的控制输出 EFAULT 最后发生。EXPORT 撤销新投递；IMPORT 保持 pending；RECEIVE 保持 QUEUED；CALL 撤销未交付 reply 槽；STATS 不改变状态。EXPORT 成功、IMPORT/RECEIVE 成功才计相应交付状态；用户 copyout 部分可见不保证用户内存原子性。

ACQUIRE、RELEASE 不要求 Server 仍存活，不要求当前 mmap。REPLY 的业务 status 即便负数也只是输出数据，不能与 ioctl 失败 errno 混淆。FREE 对本 context 的不存在/其他 context token 为 ENOENT，本 context 未交付槽为 EBUSY；IMPORT 猜其他 context 的 pending grant 同样 ENOENT。

payload_copies/payload_bytes 计数与现有 BUFFER_STATS 兼容：仅在数据复制和全部对象转换成功、即将提交有效槽时增加一次及 size；后续 EXPORT/CALL 输出 EFAULT 不倒扣历史复制计数。结构/语义失败不计接受复制。它们是累计工作计数，不是存活资源；泄漏验收看槽、Ref、Node 和事务计数。

## 9. 验收矩阵

以下是规范要求的验收目标，不等于全部分支都已实际运行；具体覆盖与未覆盖见[验收报告](../reports/m3-references-progress.md)。每项记录进程/context、grant/slot token、sender/recipient Handle、Node、user/buffer/local 单位及最终账目。

| 场景 | 必须观察到的结果 |
| --- | --- |
| 三进程主线 | A 发布 X/Y；B 表为 1->X、2->Y，C 表先为 1->Y；raw 1 到 C 调用 Y、raw 2 EBADF；B 正式传 X 后 C 得 2->X，实际调用 A，返回 X 的独立状态。 |
| 初始授权 | A 以 LOCAL 定向授予 B；仅 global node_id/cookie 不能给 C 导入；伪造 LOCAL owner EPERM，错误 cookie EINVAL。 |
| 收件 opt-in/地址 | 未 ENABLE EPERM、未 mmap ENXIO、不存在地址 ENOENT；继承他人 fd EPERM；并发收件退出没有 UAF、半提交槽或残余 refs。 |
| grant | 他人 IMPORT ENOENT，owner 输出 EFAULT 可重试且不增引用；pending FREE EBUSY；成功重领 ENOENT；sender 成功投递后 RELEASE/退出仍可领取；发布后、IMPORT 前自己的 Handle 已获授权，不承诺不可调用。 |
| 私有 Handle | 相同数字可以指不同 Node，不同数字可指同一 Node；不存在/0 EBADF；不能通过全局 Node ID 或旧 CALL_NODE 绕过 REF 模式。 |
| 重复传递 | 同 Node 两个对象复用同 Handle，buffer_refs 增两份；ACQUIRE 后 FREE 仅去槽持有；RELEASE 至零摘表，额外 RELEASE 失败。 |
| owner 接收 | B 的 HANDLE 转回 A 时输出 LOCAL(id,cookie)，Ref 表不新增，local_object_refs 增加并在 FREE 归还。 |
| 结构校验 | 0/16/17 offsets、8 对齐、尾部截断、重复/重叠/倒序、未知 type、flags、高位/非零 HANDLE cookie；普通未列入对象表的相同字节不转换。 |
| 部分失败 | [有效, 无效 Handle] 第二项 EBADF；分别覆盖第一项新建和复用 Ref，恢复原 Handle/count/slots；再覆盖多个重复项后的失败。 |
| 配额/复用 | 通过旧 owner 退出、新 owner 顺次注册，让 C 保留旧死亡 Ref，再向 C 转入当前活对象；32 条满后新条目 EAGAIN，已存在的活 Node 仍可增单位；释放一条后最小空闲 Handle 可指向新 Node，旧整数无 generation 保证，不放宽 REMOVE EBUSY。 |
| 在途与取消 | CALL 接受后 RELEASE/FREE/Handle 后续复用不改原目标；queued 取消清槽，delivered 取消保留槽；已 FREE 请求仍等待回复，已接受回复不隐式 FREE 请求。 |
| 回复对象 | 回复可建立调用者 Ref；REPLY copy/转换失败可重试；CALL 输出 EFAULT 清理嵌入引用；空回复零槽，旧普通 reply 不因 target 存在新增嵌入持有。 |
| owner 退出 | pending、已领取、已 ACQUIRE、在途 CALL 各状态下 A 退出；Node 变死亡，CALL/EXPORT EPIPE，IMPORT/RELEASE/FREE 仍能清理；A/B/C 多种退出顺序最终 live_nodes/global_ref_entries/live_slots/global_pending_slots/live_requests=0。 |
| 输出故障/旧回归 | EXPORT 输出 EFAULT 无新投递；新/幂等 PUBLISH 输出故障沿用 C07；全套旧模式测试、W=1、TCG/KVM、文档链接检查保留。 |

Ref 上限 32 大于唯一 owner 的 16 个同时已发布 Node，并不等于它永远不可达：owner 退出后旧死亡 Ref 可留在 observer，新的唯一 REF Server 再发布另一批；重复这个顺序可以积累 32 个死亡 Ref。它们不能用来 EXPORT，但可以验证 ACQUIRE 的 EPIPE、RELEASE 回收和容量计数。要验证“活源对象转入已满的接收表”可令 C 保留旧死亡 Ref，新的 A 持有活 Node，向 C EXPORT；不得为测容量放宽 owner/REMOVE 规则。

## 10. 固定 Linux v6.6 对照

- [binder_get_ref_olocked / binder_get_ref_for_node_olocked](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L1015)：Ref 按 proc 的 desc 查找，同 Node 复用 Ref，分配最小空闲编号；mini 不实现特殊 context-manager handle 0。
- [binder_translate_binder](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L2129)：本地对象查 Node/cookie、检查传递权限，转成接收 proc 的 Handle，远程 cookie 清零。
- [binder_translate_handle](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L2182)：先在发送 proc 查私有引用，接收者是 owner 时还原 BINDER ptr/cookie，否则转成接收 proc 的 Ref。
- [事务 target.handle 查找](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L3005)：调用目标也必须来自本 proc 引用，而不是 debug Node ID。
- [buffer release](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L1935) 与[转换失败清理](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L3676)：目标 Node 与嵌入对象分别归还，失败仅释放已经处理的对象前缀。
- [BC_INCREFS/ACQUIRE/RELEASE/DECREFS](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L3830)：真实协议含 strong/weak 与 owner 通知；mini 的 user_refs/buffer_refs 是明确受限的教学单位，不是完整 binder_ref 协议。

mini 的 LOCAL 使用受 owner 校验的 node_id/cookie，不使用真实 flat_binder_object 的用户 ptr；定向 grant、固定对象表、显式 PUBLISH/REMOVE、EBUSY 撤销和唯一 Server 都是教学决策。文中可以对照机制，但不能把这些差异隐藏成真实 Android UAPI。
