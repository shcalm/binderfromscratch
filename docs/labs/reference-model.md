# 数字、私有引用与对象转交

对应 C08-01、C08-02，以及 C08-03 的抽象资源规则。完整协议见 [Ref 设计](../design/references.md)，模型见 [references.c](../../examples/models/references.c)，独立验收见 [test_ref_model.py](../../tests/test_ref_model.py)。本实验是单进程普通 C 程序；A/B/C 是不同的模拟打开上下文，不是真实三个进程。本实验不访问设备、不执行 ioctl，也不代替三进程 VM 实验。

## 1. 运行与成功标准

前置条件为 C11 编译器和 Python 3，无需 root、网络、内核模块或 VM。在仓库根目录执行：

```sh
mkdir -p build
cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic examples/models/references.c -o build/references
./build/references raw
./build/references transfer
./build/references trace send_x import_fault import_x acquire_x free_x use_x release_x
python3 -m unittest discover -s tests -p test_ref_model.py -v
```

二十六项测试应全部通过。raw 的错误对象结果是故意运行的反例，程序仍退出 0；trace 中预期 EBADF/EBUSY 等也不改变程序退出码。未知模式、缺少事件、任意未知事件及 raw/transfer 多余参数退出 2，stderr 有 usage，stdout 为空；模型内部不变量失败退出 1。trace 在初始化和输出之前校验整个参数序列，不先执行合法前缀再报拼写错误。

## 2. 同一个整数进入另一张表

模型包含唯一 owner A，发布 X=101/cookie=11、Y=102/22、Z=103/33 三个 counter，初值依次 0、100、1000。Z 专用于后面的编号复用实验。初始化实际调用 LOCAL 转换、IMPORT、ACQUIRE、FREE，建立以下用户持有，不是直接给 Ref 数组填入三行：

| 上下文 | Handle | Node | user_refs | buffer_refs |
| --- | --- | --- | --- | --- |
| B | 1 | X/101 | 1 | 0 |
| B | 2 | Y/102 | 1 | 0 |
| C | 1 | Y/102 | 1 | 0 |

raw 把 B 的数字直接用于 C 的查找，实际运行 C 的私有 Ref 表及 counter 分发：

```text
event=result context=C handle=1 status=OK node=102 value=101
event=result context=C handle=2 status=EBADF node=0 value=none
```

第一行调用 Y，不是 X；第二行没有该 Ref。全局 Node ID、事务 ID、槽 token、cookie 和本地 Handle 分属不同用途。可持有引用也不自动获得服务所有方法的业务权限。

transfer 则让 B 以自己的 HANDLE 1 定向投递给 C；转换创建 C 的 HANDLE 2 指向 X。IMPORT 交付描述符，ACQUIRE 保留用户持有，再 FREE 槽。业务结果必须依次为 C2/X 增 1 得 1、B1/X 增 2 得 3、C1/Y 增 1 得 101。因此 B1/C2 指同一 X，B2/C1 指同一 Y，而数字 1 在 B/C 指不同对象。它们来自实际不同的表项和共享 Node counter，没有按预期文字直接打印结果。

raw/transfer 初始化会输出三条 event=bootstrap；transfer 还打印四步转换快照。两种模式完成后主动退出所有模拟上下文，最后都必须为：

```text
event=summary nodes=0 refs=0 slots=0 pending=0
```

## 3. trace 的初始条件与观察接口

每次 trace 重新初始化上表，三个 Node 已发布，三个上下文都已就绪、已映射，没有槽或事务；三个初始化 grant 已使用并归还。接下来的 token 从 4 开始且不复用。xhandle 最初记录整数 2，直到成功 send_x 系列观察到转换结果；xgrant/zgrant/owner_grant 分别记录最近一次对应投递的 token。

每一步先输出一条 event=step，再输出全部 Node、活 Ref、槽与对象账本；有活事务记录时再输出 event=tx。每项是空格分隔的 key=value：

| 行/字段 | 含义 |
| --- | --- |
| step operation/status/state/node/value | 事件结果、事务状态、此次实际观察的 Node 和业务值；无业务值为 none，Node 未解析为 0。 |
| step nodes/refs/users/buffers/locals/slots/pending | 存活 Node、Ref entry、用户单位、HANDLE 槽单位、LOCAL 槽单位、槽及未领取 grant 总数。 |
| step allocs/frees/converted/rolled_back | Node 申请/回收累计数，本次成功转换项数及失败逆序撤销项数。converted 即使后来失败也保留已完成前缀长度。 |
| node id/cookie/owner/allocated/published/transactions/target_slots/refs/local | Node 身份、owner、存活标记和五类保活责任；refs 是 Ref entry 数，不是引用单位之和。 |
| ref context/handle/node/user/buffer | 接收者本地 Ref 表项；同 Node 在同 context 只有一条，user 和 buffer 独立。 |
| slot context/token/kind/delivered/objects/target | 收件槽；GRANT/REQUEST/REPLY，共用每 context 16 槽，target 只用于请求。 |
| hold context/token/index/type/node/handle/cookie | 槽每一项的实际清理责任。HANDLE 带本地 handle、cookie=0；LOCAL 带 owner cookie、handle=0。 |
| tx node/state | 尚未 reap 的唯一事务记录，独立固定调用目标。 |

Node 记录在静态数组中保留历史，以 allocated 区分真实模型存活；引用归零后不会参与活查找。这不是实际 malloc/free 或内核 kfree。trace 可以停在合法在途状态，非零快照不自动意味着泄漏。

## 4. grant 与用户持有

```sh
./build/references trace send_x wrong_import free_x import_fault import_x import_x
./build/references trace send_x use_x acquire_x import_x free_x use_x release_x
./build/references trace send_twice release_x acquire_x import_x free_x release_x
```

send_x 从 B 私有 Handle 1 查 X，在 C 创建/复用 Ref，buffer 增一，pending grant 持有它。wrong_import 让 B 猜 C 的 grant，ENOENT；pending FREE 为 EBUSY；import_fault 保留相同 token/账本供重试；成功后再次 IMPORT 为 ENOENT。

引用在成功发布时已经授予指定 context，IMPORT 仅交付描述符；因此自己的 pending Handle 可以 use_x/ACQUIRE，不承诺同 context 内线程间保密。ACQUIRE 增 user，不消耗 buffer。FREE 只归还槽单位；user 尚存即可继续调用，RELEASE 至零才删除 Ref。buffer-only 的 RELEASE 为 EINVAL，不能借用户释放接口偷减槽持有。

send_twice 在一槽中传 X 两次，两项共用 C2，但 buffer=2；send_max 同理传 16 项。反复 send_x 产生不同 pending 槽，最多 16 个，第 17 次 EAGAIN；它们不占事务名额。CLI 只保存最近一次 grant token，旧槽仍可从输出观察，最终 close 负责清理全部，不隐式撤销旧投递。

## 5. 真正走到一半后回滚

```sh
./build/references trace send_bad send_bad_twice send_fault
./build/references trace send_x acquire_x import_x free_x send_bad_twice
./build/references trace send_twice send_bad_twice
./build/references trace forge_local wrong_cookie send_unknown forged_handle
```

send_bad 传 [B1,99]，第二项无效，EBADF 且 converted=1/rolled_back=1；send_bad_twice 传 [B1,B1,99]，两项已增加引用才失败，两个值均为 2。send_fault 完成一项后模拟 EXPORT 控制输出 EFAULT，两个值均为 1。新 Ref 回滚后删除；旧 Ref 只撤掉此次增加的单位，原用户持有和其他槽的 buffer 保留。

forge_local 试图由 B 使用 A 的 LOCAL，EPERM；wrong_cookie 由 A 发送错误 cookie，EINVAL。send_unknown 的第二项是未知类型，全表逻辑结构验证先拒绝 EOPNOTSUPP，converted=0；这与第二项权限失败时必须撤销前缀不同。forged_handle 查不存在的 C99，EBADF。

这些是逻辑描述符和显式故障事件；不执行实际字节编码、offsets 拷贝、只读页或部分 copy_to_user。内核对完整 24 字节、8 对齐和 LE 的验证须由 VM 测试另证。

## 6. 回到 owner 与编号复用

```sh
./build/references trace return_owner import_owner free_owner
./build/references trace send_x import_x free_x send_z import_z stale_x
```

return_owner 将 B1 交回 A，输出 LOCAL(101,11)，不创建 A 的自引用 Ref，只增加 Node.local 一份，FREE 时归还。第二条先释放 C2/X 的唯一 buffer，然后由 A 把 Z 定向授予 C。最小空闲数字 2 被复用为 Z；stale_x 仍拿旧整数 2 查表，实际递增 Z，得到 Node103/value1001。Handle 没有 generation，不能把“曾经拿到的数字”当永远不变的身份。

## 7. 调用目标与嵌入对象分开

```sh
./build/references trace send_x import_x call free_x send_z deliver free_request reply reap
./build/references trace send_x import_x call cancel reap
./build/references trace send_x import_x call deliver cancel reap free_request
./build/references trace send_x import_x call receive_fault deliver reply_fault reply_object reap call_output_fault free_request
```

call 按当时 C 的 xhandle 解析并固定目标，创建 A 请求槽，另将同一 Handle 作为一个嵌入对象转回 owner。因此 X 同时有 transaction、target-slot 和 LOCAL-object 三份不同责任。FREE 原借用槽、C2 被 Z 复用，都不能改变已接受事务的 X；reply 的业务分发仍递增 X 得 1。

QUEUED 取消自动清理未交付请求及 LOCAL 项；DELIVERED 取消保留 A 的请求槽，reap 只归还事务目标，FREE 才归还 target-slot 和 LOCAL。receive_fault 保留 QUEUED；reply_fault 在建立 Y 回复对象后撤销增量，仍 DELIVERED 可重试。reply_object 成功后 C1/Y 的 buffer 增一；return_reply 交付该回复，free_reply 归还对象。call_output_fault 则释放未交付回复，不影响 A 尚未 FREE 的请求。

本模型最多有一条活事务记录；call 在记录未 reap 时 EBUSY 是模型限制，不代表真实驱动不支持并发事务。reap 与 return_reply/call_output_fault 拆成事件以观察责任；真实 CALL 在控制输出前已经摘表，实验应按该顺序演示。即便已经 reap，只要旧 reply 槽尚未交付，新的 call 仍 EBUSY，防止覆盖待完成的输出责任；return_reply 成功或 call_output_fault 清理后才可再次 call。已交付 reply 槽继续由接收者持有，不再阻挡新 call。普通 reply 无 payload、不分配 reply 槽；return_reply 事件仅针对 reply_object 的对象槽。

## 8. 退出与映射边界

```sh
./build/references trace send_x close_b import_x use_x close_a release_x free_x close_c
./build/references trace send_x acquire_x close_a use_x send_x import_x release_x free_x close_b close_c
./build/references trace send_x unmap_c import_x remap_c free_x close_c close_b close_a
```

B 成功 EXPORT 后退出不撤回 C 的 pending 槽。A 退出后 publication 消失，但 B/C 的 Ref 仍保留死亡 Node；CALL/EXPORT/ACQUIRE 遇死亡对象 EPIPE，IMPORT、RELEASE 和 FREE 仍能清理。上面第一条的 release_x 因未 ACQUIRE 为 EINVAL，free_x 才清掉 buffer，不能把它当隐藏的用户持有。

unmap_c 后 IMPORT ENXIO，remap_c EBUSY，保留 C04 同 context 一次映射规则；pending FREE 仍 EBUSY，最终 close_c 清理。close_a/b/c 是模型终结事件，非真实 close 系统调用。尤其 close_c 表示调用进程结束，先取消/摘除自己的活 CALL，再最终释放上下文；不能据此声称真实 active ioctl 持 file 引用期间 final release 已经发生。模型已退出 endpoint 的后续事件只有抽象错误结果，不增添真实 ioctl 契约。

## 9. 独立 oracle 与限制

二十六项 Python unittest 严格编译模型，编译限时 30 秒、单次运行限时 5 秒。测试不 include 驱动或共用转换实现，而是断言固定业务结果和 Ref 映射，并从每步公开的 ref/slot/hold/tx 行重建对象图：每项 HANDLE 必须对应精确 Ref，重复项各计一次；LOCAL 计对应 owner；Ref entry 每条固定 Node 一份；target-slot 和 transaction 各自独立。再与模型计数及 allocs-frees 核对，同时拒绝孤立 hold、重复 index、空 Ref、重复 Node Ref。这样不能只靠模型打印的总数彼此相加就声称守恒。

覆盖 raw 反例、正式转交、pending 授权、grant 收件绑定、两种持有、16 对象与16 pending 槽、部分失败新建/复用/重复 Ref、owner 回转、编号复用、在途目标、取消、对象回复、待输出回复阻挡新调用、符号输出故障、一次映射和重复终结。退出测试对三种前缀各运行 A/B/C 的全部 6 种关闭顺序，共 18 个独立序列；不宣称穷举任意事件交错。

模型预设三个已就绪上下文和三个已发布 Node；没有 open_id 分配、ENABLE/REGISTER、TGID/fd 权限检查、真实 ServiceManager、线程、进程、设备、物理内存释放、用户对象强弱通知、引用死亡通知或完整业务授权。虽然表容量为 32、用户单位是 uint32_t 且有溢出检查，三个固定 Node 无法覆盖 32 Ref 满表或 U32_MAX/64 位耗尽；不将这些未运行路径算通过。逻辑对象类型检查也不证明真实 payload 的端序、对齐、长度、用户指针或 mmap 正确。实际 ENOMEM、内核锁、UAF、VMA/file 引用和任意调度仍由内核专项及真实证据负责。

手工二进制可用 `rm -f build/references` 清理；unittest 编译产物位于自动回收的临时目录，没有后台进程或设备资源。本轮只收尾 C08，不进入 C09。
