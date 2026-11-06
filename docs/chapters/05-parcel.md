# 第 05 章｜让双方一致地解释字节

第 04 章已经把请求放进 Server 的只读接收区：Client 提交字节，Server 领取 `id/token/offset/size`，读取后显式 FREE，回复仍是固定整数。开发检查点是 `dev-c04-20261106`，见[版本索引](../reference/code-versions.md)。它回答了“字节放在哪里、谁负责归还”，没有回答“这些字节各自表示什么”。

本章增加一个具体请求：u32 序号、i32 状态、可以为 null 的字节串和变长 i32 数组。设计记录见 [parcel-format.md](../design/parcel-format.md)，普通 C 实验见[Parcel 模型](../labs/parcel-model.md)。用户态库及驱动包络已在本机完成 TCG/KVM 开发验收；完整版本、最终结果及局限由[阶段报告](../reports/m2-parcel-progress.md)记录。正文仍是开发稿，独立初学者复现和正式发布门槛尚未完成。

## 1. 复制 struct 为什么没有复制消息

先写出自然的 C 表达：

```c
struct request {
    uint32_t sequence;
    int32_t status;
    const unsigned char *label;
    size_t label_size;
    const int32_t *values;
    size_t count;
};
```

对它做 `memcpy()`，会复制指针数值，不会沿着指针复制 label 和 values。接收进程中的同一个数值可能没有映射，也可能恰好指向无关对象；即使 fork 后一度有相同地址，也不能把这种巧合当作协议。`size_t` 宽度、结构体 padding、主机端序同样没有自动成为跨进程约定。

不必用随意解引用外进程地址来证明这一点。[parcel.c](../../examples/models/parcel.c) 的 `raw` 模式在合法的同进程地址上浅复制上述关系，再修改原数组和原字节串；副本仍看到修改。它在修改前还编码一份消息，编码后的值则保留原内容。关键输出为：

```text
same_label_pointer=1
same_array_pointer=1
raw_label_before=410042
raw_label_after=5a0042
raw_array_after=99,-2
wire_label=410042
wire_values=1,-2
```

该反例只证明浅复制继续别名原数据，以及该次编码复制了内容；没有模拟真实进程地址空间、内核用户复制或 Binder 对象转换。跨进程不能沿用裸指针，是由地址空间和所有权规则进一步推出的。

## 2. 决定格式之前，比较三个可行方案

| 方案 | 什么时候合适 | 对本次需求的代价 |
| --- | --- | --- |
| 固定大小、内嵌字节和数组的 struct | 字段少、容量稳定、两端共同维护协议 | 可以避免裸指针；需要规定宽度、padding、端序及有效长度。预留最大空间会进入消息大小，增长字段需版本规则。 |
| JSON 等成熟格式 | 需要可读性、多语言生态和成熟解析器 | 完全可用于服务；需规定二进制字节串表示、整数范围和解析资源上限。不会因为选择文本就失去 RPC 能力。 |
| 明确长度的二进制字段流 | 已知 schema，希望逐字节审查边界与所有权 | 本章选择；自行承担编码、版本、验证和维护成本。没有通用模式发现，也没有性能胜过前两者的实验结论。 |

我们需要区分 null 与空字节串，保留嵌入 NUL，限制总长，并让错误输入无法推动读游标越界。先用固定 schema 说明这些责任，比先实现一个任意类型的序列化框架更容易检验。四字节对齐是本协议的选择，不是“二进制协议天然如此”。

本章把三种规则分开：应用 schema 决定普通字段的顺序和含义；用户态 codec 实现字节读写；驱动传输包络规定数据区和特殊对象索引。下面的 `MBP1` 是教学格式名，不是 Android Parcel 的文件头，也不是 libbinder 兼容承诺。

## 3. MBP1：每个字节都必须有明确含义

整条普通消息最多 4096 字节，头、长度和填充都算在内。运输层提供总长度，消息中不再放另一份总长度。所有数值显式 little-endian，i32 在线上采用 32 位补码表示。

| 顺序 | 字段 | 精确规则 |
| --- | --- | --- |
| 1 | magic | 4 字节 ASCII `MBP1`，即 `4d 42 50 31`。 |
| 2 | version | u32，必须为 1；不认识就拒绝，不猜测新版布局。 |
| 3 | sequence | u32，范围 0–4294967295。 |
| 4 | status | i32，范围 -2147483648–2147483647；这是请求中的普通字段，不是 ioctl 返回码。 |
| 5 | label 长度 | u32；`ffffffff` 表示 null，没有后续 label 数据/填充；0 表示非 null 的空字节串。 |
| 6 | label 内容 | 恰好 length 个字节，再补 0–3 个零字节到四字节边界；length 不含填充。 |
| 7 | values 个数 | u32；0 表示空数组，没有 null 数组标记。 |
| 8 | values 内容 | count 个 i32，每个显式编码为 4 字节。 |
| 9 | 结束检查 | 游标必须恰好等于输入长度，不接受额外尾字节。 |

label 允许 NUL 和非 UTF-8，不补 C 字符串终止符，不以 strlen 作为字段长度。读取者必须带长度使用它。魔数和版本只识别约定，不能自动获知“下一项是数组”，也不提供任意添加字段的向前兼容；修改 schema 需要重新约定版本和兼容策略。

例如 sequence=`0x12345678`、status=-7、label 为 `A NUL B`、数组为 `[1,-2,INT32_MAX,INT32_MIN]`，完整消息为 44 字节：

| 字节偏移 | 字节 | 解释 |
| --- | --- | --- |
| 0 | `4d425031` | magic。 |
| 4 | `01000000` | version 1。 |
| 8 | `78563412` | sequence。 |
| 12 | `f9ffffff` | status -7。 |
| 16 | `03000000` | label 长度 3。 |
| 20 | `410042` | label 三个字节。 |
| 23 | `00` | 一字节零填充。 |
| 24 | `04000000` | 四个数组元素。 |
| 28 | `01000000 feffffff ffffff7f 00000080` | 四个 i32。 |

不能只验证“编码器与解码器能互相往返”：二者可能犯相同的错。实验用 Python `struct.pack('<I', ...)`、`struct.pack('<i', ...)` 和独立填充规则比较整串字节，再把独立构造的消息交给 C 解码。

## 4. codec 的责任不只是移动指针

用户态接口定义在 [parcel.h](../../userspace/parcel.h)，实现由 `userspace/parcel.c` 提供。writer 保存可写缓冲、容量和当前位置；reader 保存只读输入、长度和当前位置。初始化写入/核对八字节头，之后从偏移 8 开始。接口不分配内存，存储由调用者提供。

| 接口组 | 调用者得到什么 | 必须遵守什么 |
| --- | --- | --- |
| `mbp_write/read_u32/i32` | 定宽整数读写 | 按字节组合，不能把可能未对齐输入强转为整数指针。 |
| `mbp_write/read_bytes` | null、空串或带长度内容 | reader 返回输入中的只读借用视图，不复制内容，不添加 NUL；输出 null 标记必须独立检查。 |
| `mbp_write/read_array` | 长度前缀和 i32 元素 | reader 将元素复制到调用者数组，capacity 以元素数计；容量不足必须失败，不得写一半再报告成功。 |
| `mbp_writer_finish` | 最终编码长度 | 必须先检查前面每项成功，不能把部分消息发送出去。 |
| `mbp_reader_finish` | 检查已消费完整输入 | 多出的尾字节也是错误，不能只因前几个字段正常就接纳。 |

所有函数成功返回 1、失败返回 0；可选 error 指针得到静态原因字符串，不是 errno。每次字段失败不推进调用者游标，也不发布该次失败的输出；writer 不承诺把已经写过的每个字节恢复原状。整条消息若中途失败，调用方必须丢弃此前读到的部分业务结果。

减法检查是游标安全的起点：先确认 `position <= size`，再检查 `needed <= size - position`，不要先算可能回绕的 `position + needed`。数组先证明 count 可容纳，再计算 count×4；字节串长度和补齐也要检查溢出与 4096 上限。读取填充时要求全零，不能跳过任意字节。以临时游标完成检查，成功后再提交到原 reader/writer，才能实现“失败不推进”。

下面是 Server 已验证映射范围后的一段读取流程；变量存储和完整清理由调用者管理：

```c
int ok = mbp_reader_init(&reader, base + request.offset, request.size, &error)
      && mbp_read_u32(&reader, &sequence, &error)
      && mbp_read_i32(&reader, &request_status, &error)
      && mbp_read_bytes(&reader, &label, &label_size, &is_null, &error)
      && mbp_read_array(&reader, values, values_capacity, &count, &error)
      && mbp_reader_finish(&reader, &error);
```

这个固定调用顺序就是 schema。通用 reader 不会自动知道业务要求有四项，更不会自动替用户态选择处理函数。`request_status`、最终回复的 status、系统调用 errno 是三种不同层次的状态，不要因名称相近而混为一谈。

## 5. 为什么内核不检查 MBP1 的 label 和数组

接收消息最终要按服务协议解释。如果驱动内置 magic、字符串和数组规则，每增加一个业务 schema 都得修改内核；驱动还会和用户态重复维护版本、文本编码及字段顺序。没有涉及内核资源的普通字段，应由用户态解释，错误由服务业务状态报告。

因此本章驱动可以接受“运输包络合法，但普通格式错误”的请求。Server 解码失败后回复业务 status=`-EBADMSG`，同时按所有权规则 FREE。Client 的 ioctl 可以成功返回 0，同时收到业务失败；这不是驱动漏查字段。相反，指针不可复制、长度超过传输上限、特殊对象表结构错误，属于传输/内核边界，要在排队前拒绝。

一个普通 u32 即使碰巧等于某个 fd、Handle 或地址，也只是普通数值。内核不能扫描整段 payload，猜哪几个数该安装成资源。将来真正的对象转交需要明确位置、类型、权限和接收方身份：这才引出独立对象偏移表，而不是为了给每个普通字段都增加类型标签。

## 6. 数据区之外，再定义特殊对象包络

新增 `MB_IOC_CALL_PARCEL` 的控制结构为 32 字节：

| 字段 | 宽度 | 本章语义 |
| --- | --- | --- |
| data | u64 | Client 数据区用户地址。 |
| offsets | u64 | Client 偏移表用户地址，不是 Server 地址。 |
| size | u32 | 数据区字节数，1–4096。 |
| offsets_count | u32 | 偏移项数量，0–16；不是表的字节数。 |
| result、status | u32、s32 | 固定结果，仍由现有 REPLY 返回。 |

QUERY 的 `MB_FEATURE_PARCEL` 表示该包络与校验能力，不表示已经实现 Node、Ref、FD。无对象普通消息必须 `offsets=0` 且 `offsets_count=0`，仍经 REGISTER_BUFFER/RECEIVE_BUFFER 交付。RECEIVE 没有第二套 MBP1 消息协议；它依旧返回映射描述符，Server 自己解析普通字段。

教学偏移表每项为 u32，是数据区中的**字节偏移**。每个声明位置至少容纳八字节 `mb_object_header`：u32 type、u32 flags。该特殊头部采用当前 x86_64 UAPI 的定宽字段，不要与普通数据显式 LE 的编码混读；更不能把八字节最小头当成未来完整 Binder/FD 描述符。

校验必须覆盖**整张表**，不能遇到第一项“不支持”就提前结束，漏掉后面的结构错误：

1. 检查 size、count 及无对象时的规范空指针；先证明 count≤16，再算至多 64 字节表大小。
2. 复制控制结构和整张表；坏用户地址或跨不可读页造成复制失败，返回 EFAULT。
3. 沿用 Server 模式、容量和调用资格检查；预留目标槽，把 payload 直接复制到 backing。
4. 对快照中的每项检查八字节对齐；先确认 offset≤size，再检查 `size-offset>=8`；要求严格递增且最小头不重叠。
5. 任一结构不合法返回 EINVAL。整张表结构合法且 count>0，返回 EOPNOTSUPP：本章**拒绝所有对象类型**，不根据一个看似正确的 type 接受对象。
6. 拒绝后回滚预留槽，不入队、不设置 busy、不增加请求计数；普通无对象数据才沿既有路径发布。

例如 size=64 时，`[0,16]` 与 `[56]` 在本章最小头规则下结构合法，因此是不支持对象的 EOPNOTSUPP；`[0,0]`、`[8,0]`、`[0,64]`、`[4]` 都是 EINVAL。尤其 `[0,64]` 不能因为第一项已足够触发“不支持”就忽略第二项越界。这是为了确定地验证包络，不是声称真实 Binder 的整个对象转换过程也按这个顺序实现。

对象校验只能使用复制后的内核数据和表，不能先检查发送者地址、稍后再重新读取它来决定安全行为；两次读取之间发送者可能改写。即使只复制一次，也不保证并发修改输入时获得原子业务快照，Client 必须保持输入稳定。

校验失败发生在入队前，不能调用只适用于已入队事务的 `mb_finish()` 去减 queued；应该撤销 reserve。成功复制统计沿用“成功发布的 payload”口径：已经复制但被对象校验拒绝的字节不计成功发布，不代表它没有物理复制。控制结构和偏移表也仍需复制，不能叫零复制。

还有一个容易遗漏的边界：Server 已映射整个 backing，可能直接观察未发布槽里的写入。拒绝时“不交付”，指不返回描述符、不授予协议使用权，并不是验证前字节绝对不可见。要提供后一种保证，需要额外 staging copy 或更复杂的映射发布设计，本章没有实现。

## 7. 解码借用不改变 FREE 的责任

reader 的 label 指针借用接收槽，FREE 后就不能继续使用，即使 CPU 仍能读到旧内容；下一笔 reserve 会清零并覆盖。若要放进后台队列，必须先复制到应用拥有的存储，或把 token 的归还责任一起转交并延长持有。数组读进调用者存储后可以独立存在，label 与数组的所有权并不相同。

普通数据解码失败时，槽也已经成功交付，仍须 FREE；错误不是泄漏理由。收到描述符后的代码应统一走归还路径，再按是否仍有有效事务提交业务回复。Client 已取消时，迟到 REPLY 可得 ENOENT，但已交付槽仍由 Server 负责。REPLY 和 FREE 的先后顺序可以选择，只有不再读取借用内容后才能 FREE。

事务完成、槽归还、VMA 关闭与最终文件 release 继续是四个不同事件。munmap 后借用地址失效，已有 fd 仍可归还 token；close fd 但保留 VMA 又可能延迟服务断开。新 codec 没有改变这些规则，也没有新增锁。驱动有界检查最多 16 项，锁顺序仍是 `mb_lock → region.lock`，大锁跨用户复制的缺页阻塞成本继续存在。

## 8. 先运行不需要驱动的实验

前置条件是 C11 编译器、Python 3、make 和普通用户态环境；无需 root、设备或 VM。在仓库根目录执行：

```sh
make build/parcel
./build/parcel raw
./build/parcel demo
./build/parcel selftest
python3 -m unittest discover -s tests -p test_parcel_model.py -v
```

前三个有效模式都应退出 0。demo 应输出本章 44 字节布局对应的完整 hex，sequence=305419896、status=-7、label=410042、values=1,-2,2147483647,-2147483648。raw 若没有展示别名与 wire 内容的差异，demo 字节不符，selftest 失败或任一命令超时，都算失败。模型九项及公开库八项自动测试已通过，全套用户态回归共 49 项；映射传输另由下一节 VM 验证。

再运行可确定失败的输入；每条预期退出 1、stderr 给原因、stdout 为空，不要放在 `set -e` 后未经处理的命令链里：

```sh
./build/parcel decode 4d425031
./build/parcel decode 4d42503102000000
./build/parcel decode 4d4250310100000078563412f9ffffff03000000410042010400000001000000feffffffffffff7f00000080
./build/parcel decode 4d4250310100000001000000ffffffffffffffffffffffff
```

四条分别检验 truncated、version、padding、limit。未知命令退出 2 且打印 usage；不能把输入错误、用法错误和异常信号混在一起。超时或错误输入成功退出 0 均失败。

自动测试使用独立 Python oracle，覆盖 27 组确定性组合、null/空串/NUL/非 UTF-8、44 字节消息的每个不完整前缀、错误魔数和版本、非零填充、恶意长度/计数、尾字节。4072 字节 label 加空数组，或 null label 加 1018 个 i32，均恰好达到 4096；再加一个 label 字节或数组元素必须失败。`fffffffe` 字节长度和 `40000000` 数组个数不能先算成回绕的小尺寸再接受。

模型 selftest 还检查失败游标保持和 size_t 溢出，补足 CLI 难以构造 SIZE_MAX 对象的情况。自动测试用临时目录编译并清理，编译限时 30 秒，每次执行限时 5 秒；手工产物可 `rm -f build/parcel`。它没有内核、映射槽或进程退出交错，不能代替下一节。

## 9. 在映射页上检验普通字段与对象边界

准备 [P04 环境](p04-environment.md)后，VM 的完整入口为：

```sh
make vm-test
make vm-test-kvm
```

这些目标负责构建和运行客户机；KVM 需要可用 `/dev/kvm`，不具备时应记录未运行。不要在宿主调用 `/parcel_test` 或加载驱动。`tests/kernel/parcel_test.c` 与用户态库一起编译，客户机 init 执行；全局 MINI_LAB_PASS、运行器退出 0、最终资源归零及没有内核告警必须同时满足。宿主运行器默认 120 秒截止，内层测试有独立截止；同名日志可能覆盖，最终归档位置以阶段报告为准。

下表各项已在本机 TCG/KVM 实际运行，原始日志哈希见阶段报告；55 条映射消息含 49 条 Server 业务拒绝和 6 条合法请求，其中一条验证输出复制 EFAULT。源码中的测试断言或 PASS 字符串本身仍不是执行证据：

| 问题 | 要运行的步骤与预期 | 失败判据 |
| --- | --- | --- |
| 普通消息能否完成？ | 无对象表提交正常 schema，Server 在映射页逐项解码，FREE 并回复固定结果；覆盖 null、空串、嵌入 NUL 和精确 4096 | 字节/字段错误、结果不符、FREE 后槽残留或调用挂起。 |
| 普通格式错误由谁拒绝？ | 传输合法而 magic/version/长度/填充/数组/尾字节恶意，Server 返回业务 -EBADMSG 并 FREE | 驱动越层解释普通字段、Server 接受坏格式、把业务错误误作系统 errno 或遗留槽。 |
| 控制结构与用户地址如何失败？ | 坏控制指针、payload 指针、offsets 指针及跨 PROT_NONE 页的部分复制为 EFAULT | 错误码不符、入队或成功发布计数增长。 |
| 表大小与空表约定是否明确？ | size=0/4097、count=17、count=0 却 offsets 非零为 EINVAL | 接受非规范包络或按未受限 count 计算表大小。 |
| 全表都检查了吗？ | `[4]`、边界/UINT32_MAX、重复/乱序、首项合法后项越界为 EINVAL | 首项遇“不支持”就提前 EOPNOTSUPP，或任何非法表入队。 |
| 合法对象是否仍拒绝？ | 结构合法的单项/多项/16 项、边界恰容纳头部及测试头字节均 EOPNOTSUPP；没有任何受支持类型 | 假称支持某对象，或产生请求/槽泄漏。 |
| 回滚能否恢复？ | 每次拒绝后确认 queued/delivered/live_requests/槽为 0，再执行正常调用 | 仅错误码正确但资源未撤销、busy 未清或后续调用失败。 |
| 最终结果复制失败意味着什么？ | 控制结构先可写准备，再设只读；输入/业务成功后结果写回 EFAULT，仍核对归零 | 由 EFAULT 推断业务未发生、自动重发或残留事务/槽。 |
| 旧路径仍工作吗？ | 同次客户机保留原查询、RPC、映射权限/取消/退出与分配失败回归 | 只看 parcel 子测试成功，忽略旧路径或最终卸载失败。 |

同步应使用管道命令/报告、内核计数或确定的领取点，不能靠随意 sleep 猜测“应该已经收到”。输入拒绝的返回与队列账目分开核对；完成业务后输出 EFAULT 的情况，必须等相关线程/进程结束再看最终状态。

清理要回收 Client/Server、释放已交付 token、解除映射、关闭 fd/管道，最后卸载模块和关闭客户机。失败 VM 销毁后重建，不沿用半失败状态。模型无持久资源不代表真实调用无需清理，计数归零也不等同于 KASAN 或通用泄漏检测通过。

## 10. 固定版本的 Android 源码对应

用户态基线为 AOSP `android-14.0.0_r1`、`frameworks/native` 提交 **`bfcf75076e562945bae131a4929f29a90d0d2481`**；下载与哈希见 [AOSP 基线](../reference/aosp-userspace.md)。本章核对缓存的 Parcel.cpp、IPCThreadState.cpp 哈希与基线一致，并直接阅读下列位置。只讨论 kernel Binder 分支，不混用同文件内的 RPC Binder 路径。

| 教学问题与机制 | 固定源码入口 | 相同点与不同点 |
| --- | --- | --- |
| u32/i32 由用户态 codec 编码 | [Parcel::writeInt32，1254 行](https://github.com/aosp-mirror-neo/platform_frameworks_native/blob/bfcf75076e562945bae131a4929f29a90d0d2481/libs/binder/Parcel.cpp#L1254)、[writeAligned，1886 行](https://github.com/aosp-mirror-neo/platform_frameworks_native/blob/bfcf75076e562945bae131a4929f29a90d0d2481/libs/binder/Parcel.cpp#L1886) | 普通编码在用户态；真实 writeAligned 用 memcpy 并推进数据位置。mini 的 MBP1 头、固定 schema 和显式 LE 并非真实 Parcel 的共同线格式。 |
| 字符串需要内容、长度与 null 规则 | [writeString8，1363 行](https://github.com/aosp-mirror-neo/platform_frameworks_native/blob/bfcf75076e562945bae131a4929f29a90d0d2481/libs/binder/Parcel.cpp#L1363)、[writeString16，1386 行](https://github.com/aosp-mirror-neo/platform_frameworks_native/blob/bfcf75076e562945bae131a4929f29a90d0d2481/libs/binder/Parcel.cpp#L1386) | 真实方法写长度、内容和结束符，null 写 -1；String16 长度按字符单元。mini 是无终止符任意字节串，不能互换。 |
| 数组的计数是协议内容 | [writeInt32Array，1264 行](https://github.com/aosp-mirror-neo/platform_frameworks_native/blob/bfcf75076e562945bae131a4929f29a90d0d2481/libs/binder/Parcel.cpp#L1264) | 都约束长度并写计数/元素；真实方法支持空指针写 -1，mini schema 的数组不支持 null。 |
| 特殊对象为什么需要独立索引 | [writeObject，1641 行](https://github.com/aosp-mirror-neo/platform_frameworks_native/blob/bfcf75076e562945bae131a4929f29a90d0d2481/libs/binder/Parcel.cpp#L1641) | 真实 flat_binder_object 在数据区，需要元数据时把 mDataPos 记入对象表，并参与引用协作；mini 只建立并校验包络，拒绝所有对象，不做这些协作。 |
| 数据区与对象表如何交给驱动 | [IPCThreadState::writeTransactionData，1231 行](https://github.com/aosp-mirror-neo/platform_frameworks_native/blob/bfcf75076e562945bae131a4929f29a90d0d2481/libs/binder/IPCThreadState.cpp#L1231) | 真实分别填 data_size/buffer 与 offsets_size/offsets，再写 BC 命令和事务结构。offsets_size 是字节数，元素为 binder_size_t；mini 是最多 16 个 u32 偏移及元素个数，不兼容 ABI。 |
| 特殊头与完整描述符如何区分 | Linux v6.6 UAPI [binder_object_header，66 行](https://github.com/torvalds/linux/blob/v6.6/include/uapi/linux/android/binder.h#L66)、[flat_binder_object，77 行](https://github.com/torvalds/linux/blob/v6.6/include/uapi/linux/android/binder.h#L77) | 真实公共头只有 u32 type，flat 对象另含 flags、binder/handle 与 cookie；mini 的 type/flags 八字节头是自定义最小结构，不应称为真实公共头的同尺寸复刻。 |
| 驱动必须验证内核特殊描述符 | Linux v6.6 [binder_get_object，1690 行](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L1690)、[binder_transaction 对象遍历，3308 行](https://github.com/torvalds/linux/blob/v6.6/drivers/android/binder.c#L3308) | 真实按类型确定完整结构大小并验证/转换对象；mini 八字节头仅是本章最小包络规则，不能替代完整对象校验，也不能套用同一错误优先级。 |

Linux v6.6 驱动与 Android 14 用户态是分别固定的阅读输入，没有在本教程组成已构建运行的 Android 系统。真实 Parcel 没有本章 MBP1 消息头，真实 Binder 还传输回复数据、对象和更多控制语义；本章保持固定整数回复，没有 Node/Ref/FD 转换，也没有 libbinder 端到端调用证据。

## 11. 代价、练习与下一步

这套设计让普通字段留在用户态、内核资源入口显式化，但增加了格式版本、游标、借用视图和两层错误。每字段有检查，数组需要复制到应用存储，接收区仍按整页占槽，解码失败仍要走 FREE。它服务于可解释、可检验的教学目标，没有吞吐或缓存优势结论。

1. 编码后 label 指针还能跨进程传递吗？传递的是长度与字节，不是旧指针；接收端得到自己输入缓冲中的新视图。若它随接收槽借用，FREE 后仍失效。
2. null label 与空串的 payload 都没有内容，为什么还要不同标记？业务可以赋予它们不同含义；只有长度 0 无法表达这种区别。本 schema 的数组没有提出 null 需求，不能擅自把 UINT32_MAX 当数组 null。
3. 完成所有字段读取为什么还要 finish？额外尾字节可能表示双方 schema 不一致或恶意输入，不能静默忽略。finish 不能替代每项长度和填充检查。
4. 对象表第一项合法但不支持，第二项重复，应该返回什么？EINVAL。先完成全表结构检查，才决定合法对象集合在本章不受支持；不能提前 EOPNOTSUPP 掩盖坏表。
5. 把一个 fd 数值当普通 u32 编码，内核会替接收者安装 fd 吗？不会。普通数值没有资源转交语义；本章也没有任何可接受的特殊对象类型。
6. Server 解码失败是否可以不 FREE？不能，成功交付已经转移槽责任；业务失败和协议缓冲回收是两件事。迟到回复 ENOENT 同样不代替 FREE。

现在我们能一致地解释一条请求，但“解释成功”没有说明哪一次调用完成。两个 Client 使用相同 sequence、Server 反序回复、Client 取消与回复竞争时，普通字段既不授予回复资格，也不能代替驱动事务身份。第 06 章从这些交错重新审视 Transaction：如何关联请求与回复，谁能终结一次、谁最终释放记录，以及通信完成事件为什么不是业务结果。接收缓冲的归还继续独立计账。
