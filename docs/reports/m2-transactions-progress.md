# M2：事务与双向缓冲开发验收

日期：2026-11-06。上一检查点为 `dev-c05-20261106`（`31abf0e`），本版检查点为 `dev-c06-20261106`。这是开发验收，不是整个 M2 或第 06 章正式发布。设计见[事务决策](../design/transactions.md)，正文见[第 06 章](../chapters/06-transactions.md)，普通 C 实验见[事务模型](../labs/transaction-model.md)。

## 1. 为什么需要这次演进

上一版已经有请求 ID 和每 CALL 等待记录，但每个 Client 上下文只允许一笔调用，回复只有固定整数。新需求是同一打开上下文并发两次调用、业务 sequence 可以重复、Server 反序完成并返回结构化数据。单结果槽会覆盖，单靠业务序号不能验证回复资格；新增独立事务模式、Client 接收映射和显式状态。

QUEUED、DELIVERED、REPLIED、FAILED 只描述调用进展。请求槽领取后归 Server，回复槽 CALL 输出成功后归 Client；两端分别 FREE。CALL 在锁内保存结果快照、摘表，解锁释放记录，之后只使用栈上结果和文件生命周期保护的 caller。输出 EFAULT 撤销未交付回复槽；active_calls 覆盖到全部输出完成。代价是双向槽、失败回滚、终结竞争及大锁跨用户复制。

## 2. 环境、命令与实际结果

Linux v6.6、BusyBox 1.36.1、x86_64 及编译配置沿用[环境清单](../../tools/kernel/manifest.md)、[M0/M1](m0-m1-progress.md)和[映射报告](m2-mapping-progress.md)。本版 initramfs SHA-256：`4340c6d1bf0d3b04c58ff0eb35371665e08c0eeaaef30ed06477895f6b9d5c7b`。

```sh
make test
make -C build/kernel/out M="$PWD/driver" W=1 modules
bash tools/qemu/build-rootfs.sh
LOG_TAG=transaction-reviewed-tcg bash tools/qemu/run.sh
ACCEL=kvm LOG_TAG=transaction-reviewed-kvm bash tools/qemu/run.sh
```

用户态 **60 项通过**，其中新增事务模型 11 项；独立契约表验证 52 个状态/事件和 64 个核心有序事件对。驱动 `W=1` 编译通过。完整 TCG、KVM 均退出 0，包含已有模块回滚、接收区 ENOMEM 注入、设备、RPC、映射、55 条 Parcel 消息和 add 演示。运行器未发现其规则覆盖的内核告警；卸载均为 `live_contexts=0 requests=0 regions=0 slots=0`。

```text
TRANSACTION_STAGE same_fd_threads=2 sequence=77 reverse_replies=associated buffers=independent
TRANSACTION_STAGE receive_EFAULT=retryable token_owner=validated duplicate_free=ENOENT
TRANSACTION_STAGE reply_errors=retryable partial_copy=EFAULT queued_reply=ENOENT
TRANSACTION_STAGE reply_slots=16 full=EAGAIN free_recovery=ok empty_full_reply=ok
TRANSACTION_STAGE unmapped_reply=ENXIO empty_unmapped=ok output_EFAULT=rollback
TRANSACTION_STAGE reply_max=4096 codec=validated reply_oversize=EINVAL
TRANSACTION_STAGE cancellation=queued,delivered orders=cancel_first,reply_first terminal_once=ok
TRANSACTION_STAGE server_exit=queued,delivered errors=EPIPE resources=clean
TRANSACTION_STAGE client_SIGKILL=queued,delivered queued_auto_free=ok delivered_explicit_free=ok
TRANSACTION_TEST_PASS reverse=2 reply_slots=16 cancellation=validated resources=clean
```

## 3. 证据与设计的对应

| 设计问题 | 实际实验与边界 |
| --- | --- |
| 单槽为什么不够 | C 模型执行两次覆盖写入和两次读取，第二笔丢失 42；独立 ID 模型反序关联成功。模型 sequence=7，VM 为 77，均刻意重复。 |
| 同 fd 是否真正并发 | 两个 pthread 共享同一 Client fd，先确认 queued=2，再领取两条并反序回复；逐笔解码为 220/110，业务 status 保持。 |
| 谁能回复 | 错上下文 EPERM；伪造、重复、未交付和取消后的 ID 为 ENOENT；旧新 CALL/RECEIVE/REPLY 模式 EPROTOTYPE。未交付 ID 用隔离白盒预测，并由随后 RECEIVE 确认，非通用公开查询接口。 |
| 领取输出失败 | 坏指针及只读页分别触发新 RECEIVE EFAULT，queued=2/delivered=0、槽数不变，随后按原 payload/FIFO 领取。 |
| 请求和回复是否独立 | 一笔先 FREE 请求再回复，另一笔先回复再 FREE 请求；逐步检查双方 used_slots。双方 token 都验证错 owner、重复 FREE 为 ENOENT。 |
| 回复复制失败 | 坏控制指针、坏 payload、跨 PROT_NONE 页的部分输入复制 EFAULT；事务仍 DELIVERED、临时回复槽归零，修正后回复成功。 |
| 回复区耗尽 | 保留 16 枚回复 token，下一次非空回复 EAGAIN；FREE 一个后重试成功且 token 不复用。再满时空回复仍成功终结，无新增槽。 |
| Client 无映射 | CALL 发布后完整 munmap，非空回复 ENXIO；空 payload 的业务 -ENXIO 正常返回。 |
| CALL 输出失败 | 控制页只读，Server 已提交回复，Client EFAULT；未交付回复槽回滚。随后同上下文正常调用成功。此用例不证明所有部分输出失败均零写入。 |
| 回复边界 | 4072 字节 label 加字段恰为 4096，逐字节及 codec 检查；status=7 保留。4097、size=0/data 非零均 EINVAL。 |
| 取消两阶段 | queued 信号取消自动回收请求槽；delivered 取消保留 Server 槽并显式 FREE；CALL 为 EINTR，迟到回复 ENOENT。 |
| 两种确定顺序 | 取消先行时等待 CALL 返回后再回复；回复先行时 REPLY 成功后发信号。后者可能在 CALL 返回后才送达，不声称强制命中内核输出窗口。 |
| Server 最终退出 | 子进程分别在 queued/delivered 时 munmap 并 close，Client EPIPE；回收全部 region/slot。 |
| Client 进程死亡 | 子进程自行 open/mmap，在 queued/delivered 时 SIGKILL，waitpid 确认信号；核对 child region/context 已消失。queued 槽自动回收，delivered 槽由存活 Server FREE。 |

每阶段等待相关 CALL 返回并 join/wait 后检查账目，不把 live_requests=0 等同所有输出结束。使用计数、管道和领取点协调，不使用随机 sleep。内层 alarm=45 秒，宿主默认截止=120 秒；预期断言失败或超时均不计通过。

## 4. 先失败与独立审查

先用计划命令号 12 在 C05 驱动执行注册探针，得到 ENOTTY（errno 25），客户机 FAIL、宿主退出 1；新实现后注册和旧回归通过。该记录只证明新接口从缺失变为存在；完整行为另由最终矩阵验证，不能把注册探针当作全部用例的失败基线。

设计与驱动/UAPI 生命周期审查未发现阻塞问题。模型审查独立运行 11 项通过。测试审查发现缺少新模式 Client 进程退出和 RECEIVE 输出故障，补齐后复核关闭，并重新执行最终 TCG/KVM。正文源码定位也按固定基线复核。

| 本地原始记录 | SHA-256 |
| --- | --- |
| `artifacts/transaction-red.txt` | `48f698d5ed550324d333e7ee58683f0728f9aa4729641c8885203311d9e6728b` |
| `artifacts/qemu-transaction-reviewed-tcg.log` | `506079fb1499763e2d856a4a1605d2ea0035f993237be4c3773f644182c3af4f` |
| `artifacts/qemu-transaction-reviewed-kvm.log` | `b8972e7dc17e8d928b5287a05dcc221f14ea8781a852700c29e8951cfa8a11ea` |

日志保存在忽略的 artifacts 目录，尚无外部下载发布；代码检查点与构建命令是重放入口。普通测试清理临时目录，VM 正常结束会 FREE、join/wait、munmap/close、卸载并关机；失败 VM 丢弃，宿主不加载教学模块。

## 5. Android 对照与未关闭的门槛

正文对照 Linux v6.6 的 binder_transaction、同步 transaction_stack、BC/BR 请求回复及 BC_FREE_BUFFER；用户态固定 Android 14 `frameworks/native` 提交 `bfcf75076e562945bae131a4929f29a90d0d2481` 的 waitForResponse/BR_REPLY/freeBuffer。COMPLETE 不携带业务结果，即便 v6.6 同步路径延迟发出也不能替代 BR_REPLY。两个来源分别固定，只作源码对照，没有构建为经过验证的 Android 系统。

mini 全局 ID 加每 CALL 等待者不等于真实线程同步栈；当前依旧唯一 Server，无 Node/Ref/FD、线程池、嵌套回调或死亡通知。新事务路径仅普通字节，无对象偏移转换。模型保留静态观察账目，不证明真实 kfree 或调度。

未执行真正事务 kmalloc ENOMEM、token/ID 64 位耗尽、全部竞争交错、回复已接受而 CALL 尚未输出时的 Client SIGKILL、KASAN/lockdep。EAGAIN 槽耗尽不是 ENOMEM 实验，零账目不是通用泄漏检测。全局锁仍跨用户复制，恶意缺页可阻挡其他调用。本章正式发布、独立初学者复现和整个 M2 里程碑门槛仍未关闭。
