# M2：Parcel 请求开发验收

日期：2026-11-06。前版为 `dev-c04-20261106`（`bbd19a9`），本版检查点为 `dev-c05-20261106`。本报告覆盖普通数据编解码、对象偏移包络校验和映射接收，不表示整个 M2 或第 05 章正式发布。设计见[Parcel 决策](../design/parcel-format.md)，正文见[第 05 章](../chapters/05-parcel.md)。

## 1. 要验证的设计

传输字节不能代替双方约定字段。用户态 codec 负责 MBP1 格式的定宽整数、nullable bytes、数组、零填充和边界；驱动只处理传输包络与特殊对象索引。CALL_PARCEL 复用上一章的接收映射、领取、FREE 和固定整数 REPLY。合法非空对象表一律 EOPNOTSUPP，没有伪造 Node/Ref/FD 能力。

偏移表先限制数量再复制，全表结构校验在 payload 复制后、事务发布前完成。结构不合法优先 EINVAL；错误用户地址 EFAULT；失败归还预留槽。普通字段错误则由 Server 解码并返回业务 -EBADMSG，仍须 FREE。成功发布计数不是所有实际复制操作的计数，未发布字节也可能被已经映射整区的 Server 观察。

## 2. 命令、环境与结果

运行内核、BusyBox 和编译环境沿用[映射报告](m2-mapping-progress.md)及[M0/M1 环境清单](m0-m1-progress.md)。本版 initramfs SHA-256 为 `9c8fedbeb100f6a13b2e99139c22660371e8e74c35763337d3372099f8f96203`。

```sh
make test
bash tools/qemu/build-rootfs.sh
LOG_TAG=parcel-reviewed-tcg bash tools/qemu/run.sh
ACCEL=kvm LOG_TAG=parcel-reviewed-kvm bash tools/qemu/run.sh
```

用户态共 **49 项通过**，其中模型 9 项、公开库 8 项；其余是之前阶段回归。驱动 `W=1` 模块编译通过。最终 TCG 与 KVM 均退出 0，包含旧模块回滚、接收区 ENOMEM 注入、设备、同步 RPC、映射权限/取消/退出、Parcel 及 add 演示。卸载时 `live_contexts=0 requests=0 regions=0 slots=0`，运行器未检出其规则覆盖的内核告警。

```text
PARCEL_OFFSETS_PASS structural_errors=EINVAL objects=EOPNOTSUPP rollback=clean
PARCEL_CODEC_PASS mapped_messages=55 every_prefix=43 business_rejects=49 output_fault=1 slots=0
PARCEL_TEST_PASS
```

55 笔由 6 笔合法消息和 49 笔业务格式错误组成：正常消息、同消息只读控制输出、null、空串、4072 字节 label 和 null 加 1018 元素数组为 6 笔；43 个非空截断前缀及 magic/version/padding/恶意 label 长度/恶意数组数/额外尾字节为 49 笔。长度 0 属传输错误，另在包络测试中验证，不算成功交付。

## 3. 证据矩阵

| 问题 | 实际证据与结果 |
| --- | --- |
| 为什么不能浅复制 struct | `raw` 在同进程合法地址上验证 label/数组指针别名；wire 保留编码前内容。未解引用外进程地址。 |
| 编解码是否有独立依据 | Python struct.pack oracle 比较完整 wire，包含 27 组确定字段、数值端点、null/空/NUL/非 UTF-8；不是只做自身往返。 |
| 游标和输出能否可靠使用 | 库测试验证失败不推进原语游标、不修改 scalar/array 输出、数组容量不足不部分写入、无分配和借用语义；模型组合调用只在整条成功后发布 message。 |
| 4096 上限 | 模型独立编码、解码及 VM 两种精确 4096 请求均通过；再增字节/元素在用户态拒绝，传输 size=4097 在驱动拒绝。 |
| 普通字段由谁检查 | 49 条坏 MBP1 正常交付，Server 返回 status=-EBADMSG，Client ioctl 返回 0；FREE 后每笔槽/请求归零。 |
| 偏移结构检查是否完整 | 错位、边界不足、UINT32_MAX、重复、乱序、首项合法后项越界均 EINVAL；16 项表最后一项非法同样 EINVAL。 |
| 未实现的对象是否拒绝 | 单项、恰好尾部 8 字节、多项和最多 16 项合法表均 EOPNOTSUPP；任意头字节均不产生对象。 |
| 控制和复制失败 | 错控制/payload/offsets 指针，offsets/payload 跨 PROT_NONE 页的部分复制失败为 EFAULT；包络/表拒绝后调用结构保持原值。 |
| 回滚是否真的恢复 | 每次拒绝后核对 queued/delivered/live_requests、局部与全局槽、成功发布次数和字节数；接着正常调用仍成功，busy 未被遗留。 |
| 已执行但回写失败 | Client 把控制结构改为只读后提交；Server 确认解码成功、FREE/REPLY，Client 最终 EFAULT；事务/槽仍归零，没有自动重试。 |
| 关闭与回归 | Server 完整 munmap/close 后全局 region/slot 为 0，只剩 observer 上下文；之后卸载进一步核对全局 0。旧 RPC/映射全部在同次 VM 执行。 |

## 4. 先失败，再实现

在 C04 驱动中先编译使用计划命令号的 Parcel 测试。该驱动返回 ENOTTY（errno 25），客户机明确 MINI_LAB_FAIL，宿主退出 1。新增 UAPI 与调用模式后，相同包络断言通过；随后补入真实库编解码、全前缀、16 项末项错误和资源统计，最终两种加速模式通过。

库与模型也先执行缺失实现/反例的失败测试，再实现共享 codec。模型与 VM 使用同一个库；Python oracle 独立于库，避免为了 CLI 和驱动演示各保留一份可能漂移的编码器。

| 本地原始记录 | SHA-256 |
| --- | --- |
| `artifacts/parcel-envelope-red.txt` | `2d5847047256db65c2b02499bb794fe7aaea33ae6e2917bf3aa604b233747cda` |
| `artifacts/qemu-parcel-reviewed-tcg.log` | `1bdad1ebfe0fbc888e63bbd0bc9f5e538a2738adeda4b61f5cf6f4550c54f752` |
| `artifacts/qemu-parcel-reviewed-kvm.log` | `37dc9bbeb3577b9115d8e2d292faf716447adc42aafc9a43884dd1926ad3886c` |

日志在忽略的 artifacts 目录保存，尚未发布为外部下载包。截图或摘要不能替代原始日志；代码标签、源码环境与准确命令是重放依据。

## 5. Android 对照与限制

已按[AOSP 基线](../reference/aosp-userspace.md)核对 Android 14 固定提交的 Parcel/IPCThreadState，并对照 Linux v6.6 的 object header、binder_get_object、事务对象遍历。普通字段由库处理、特殊对象另给 offsets 是职责对应；魔数、LE 字节串、无 null 数组、u32 偏移、16 项上限、八字节教学头与固定回复都不是兼容 ABI。真实 Binder 的对象大小判断和引用转换尚未实现。

独立设计/实现审查未发现阻塞问题；代码质量审查指出“输入地址与自身比较”的无效测试断言，已改为比较原始 wire 地址并重新运行两种 VM 验收。审查不代替动态验证。

没有变长回复、线程事务栈、Node/Ref/FD、死亡通知或真实 libbinder 端到端调用。本次没有 KASAN/lockdep、任意用户输入并发修改和全部调度交错实验；大锁跨用户复制仍可能因缺页阻塞。资源计数只核对本模块账目，不是通用泄漏检测器。正式章节标签与独立初学者复现门槛继续未关闭。

正常测试逐条 FREE、等待 Client、munmap/close；内层 alarm 与宿主截止共同限制挂起。失败时丢弃 VM，不在宿主加载模块。手工模型产物可用 `make clean` 移除；内核/rootfs 缓存保留以供复跑。
