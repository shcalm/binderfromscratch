# 在 QEMU 里手动验证 mini-binder

本实验要回答：客户端把 `1` 和 `2` 交给驱动后，谁做加法、结果怎样传回，以及服务端不存在或计算失败时会发生什么。先亲手运行 Server 和 Client，再逐项运行驱动测试，观察请求、回复与资源回收。

本页使用当前开发工作树构建的镜像，适用于已有同步 RPC、映射、Parcel、Transaction、Node 和 Ref 的版本。复现旧章节快照时，应按[代码版本索引](../reference/code-versions.md)使用该版本自己的构建入口。本实验验证教学驱动 `/dev/mini_binder`；真实 Android Binder 的完整服务调用需要另外的用户态环境。

## 1. 分清宿主命令和虚拟机命令

| 位置 | 如何辨认 | 在这里做什么 |
| --- | --- | --- |
| 宿主 Linux 终端 | 启动 QEMU 前的普通终端 | 编译驱动和程序、生成镜像、启动 QEMU |
| QEMU 内的 Linux | 启动完成后出现 `~ #` 或 `/ #` | 挂载文件系统、加载模块、运行 Server/Client 和测试程序 |

下面的代码块不包含提示符，可以直接复制。第 2 节在宿主执行；第 3–8 节在虚拟机执行；第 9 节再回到宿主。宿主构建不需要 root，虚拟机中的 shell 已以 root 身份运行。

## 2. 在宿主构建并进入交互 shell

宿主需要 [P04](../chapters/p04-environment.md) 列出的编译依赖和 `qemu-system-x86_64`。在仓库根目录执行：

```sh
make kernel
make busybox
make rootfs
```

首次会下载并校验 Linux 6.6 和 BusyBox 1.36.1，内核编译需要较长时间。已有相同内核和 BusyBox 产物时，修改驱动或测试程序后只需重新 `make rootfs`。它会把当前模块、Server/Client 和测试程序放进 `build/initramfs.cpio.gz`；旧镜像不会随着源码编辑自动更新。

若当前用户可以读写 `/dev/kvm`，在同一个宿主终端启动：

```sh
qemu-system-x86_64 \
  -machine q35,accel=kvm -cpu max -m 512 -smp 2 \
  -display none -serial stdio -monitor none -no-reboot -nic none \
  -kernel build/kernel/out/arch/x86/boot/bzImage \
  -initrd build/initramfs.cpio.gz \
  -append 'console=ttyS0 rdinit=/bin/sh panic=1 oops=panic'
```

没有可用 KVM 时，只把上面 `accel=kvm` 改为 `accel=tcg`，其余参数相同。TCG 使用软件模拟处理器，通常更慢。

等待出现：

```text
/bin/sh: can't access tty; job control turned off
~ #
```

此时键盘输入已交给虚拟机。这个简易 shell 的 job-control 提示不阻止本页的后台程序、`kill` 和 `wait` 操作；它不提供完整的 `fg` / `bg` 终端作业控制。

`rdinit=/bin/sh` 让内核直接启动 shell，因此本次不会运行镜像中的自动验收 `/init`，也不会自动加载驱动或自动关机。`-serial stdio` 保留终端输入，供你亲手输入命令；这里直接启动 QEMU，不套用自动验收运行器，也没有它的 120 秒截止。

## 3. 在虚拟机中加载驱动

从这里开始，命令都在虚拟机的 `~ #` 下执行：

```sh
export PATH=/bin
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys

insmod /mini_binder.ko
busybox ls -l /dev/mini_binder
/device_test
```

预期看到：

```text
mini_binder: device registered ABI=1
crw------- ... /dev/mini_binder
DEVICE_TEST_PASS open_id=...
```

`c` 表示字符设备，设备号和 `open_id` 以本次输出为准。`device_test` 验证设备查询和基础错误路径；设备存在还不能证明跨进程加法已经完成。

镜像只给少量 BusyBox 命令创建了短名称。`ls`、`ps`、`tail` 等可写为 `busybox ls`、`busybox ps`、`busybox tail`，无需另外安装软件。

## 4. 没有服务端时，调用得到什么？

先预测：驱动是否会自己算出 `3`？然后执行：

```sh
/add_client 1 2
echo $?
```

预期：

```text
transport error: call: Transport endpoint is not connected
3
```

客户端能打开设备，但驱动还没有注册的 Server 可以接收这笔请求，所以调用失败。加法业务在 Server 用户态程序里实现。`echo $?` 必须紧跟被观察的命令，否则读到的是另一个命令的退出码。

本页会主动触发非零退出，不要在虚拟机 shell 中启用 `set -e`，也不要把全部实验用 `&&` 串起来。

## 5. 启动服务端，观察成功回复和业务错误

在同一个虚拟机 shell 中执行：

```sh
/add_server 3 > /tmp/server.log 2>&1 &
server_pid=$!
cat /tmp/server.log
```

`&` 让 Server 在后台运行，shell 继续接受输入；`$!` 保存它的 PID。`3` 表示成功提交三笔回复后退出，其中可以包含业务失败的回复。它不是线程数，也不是运行秒数。

**必须看到单独一行 `READY`，再继续下面的客户端命令。** 如果日志暂时为空，再执行 `cat /tmp/server.log`；不要仅凭进程已经创建就假定注册完成。如果出现 `transport error: register`，先按第 10 节排查。Server 注册成功后阻塞等待请求，这时不退出是预期行为。

### 第一笔：1 + 2

```sh
/add_client 1 2
echo $?
```

预期是 `result=3`，退出码 `0`。继续查看：

```sh
cat /tmp/server.log
```

日志在 `READY` 后增加一条：

```text
event=replied request_id=... status=0
```

这次结果经过 Client 的 `MB_IOC_CALL`、Server 的 `MB_IOC_RECEIVE`、用户态加法和 `MB_IOC_REPLY` 才返回。Client 输出结果与 Server 打印日志的调度顺序不固定，必要时再次查看日志。

### 第二笔：19 + 23

```sh
/add_client 19 23
echo $?
```

预期是 `result=42`，退出码 `0`。每次 `/add_client` 都启动一个独立客户端进程；Server 仍是刚才保存 PID 的同一个进程。

### 第三笔：超出 uint32_t 的范围

```sh
/add_client 4294967295 1
echo $?
```

预期：

```text
business error: status=-75
2
```

参数各自合法，但和超出 `uint32_t` 范围。在本环境中 `-75` 是 `-EOVERFLOW`。Server 已领取请求并成功提交一笔错误回复，Client 也已收到它；失败的是业务计算。驱动不把业务失败伪装成 ioctl 传输失败。

## 6. 等服务端退出，再调用一次

完成上面三笔请求后执行：

```sh
wait "$server_pid"
echo $?
cat /tmp/server.log
```

`wait` 应返回 `0`，日志应有 `READY` 和三条 `event=replied`：两条 `status=0`，一条 `status=-75`。Server 正常完成三笔回复，所以它可以成功退出，即使其中一笔业务返回了溢出。

若只发了两笔请求就执行 `wait`，shell 会继续等待尚未完成的 Server。应先补齐第三笔；若要提前结束本轮，在执行 `wait` 前使用 `kill -TERM "$server_pid"`，再 `wait "$server_pid"` 回收，这时 Server 的预期退出码为 `143`。

现在再调用：

```sh
/add_client 1 2
echo $?
```

应再次得到 `transport error: call: Transport endpoint is not connected`，退出码 `3`。

| 观察 | 本次验证了什么 |
| --- | --- |
| 无 Server：传输错误，退出 `3` | 打开设备不等于已经有服务处理请求 |
| Server 已注册：`result=3`、`result=42`，退出 `0` | 参数和回复可以在不同进程间通过驱动传递 |
| Server 返回溢出：业务错误，退出 `2` | 收到回复与业务成功是两个条件 |
| Server 退出后：传输错误，退出 `3` | 服务注册随该打开文件的最终释放而清理 |

这里的 Server 没有额外子进程或继承设备引用，因此 `wait` 回收后可以观察到注册已清理。它不证明所有服务都在 PID 死亡的同一瞬间完成清理；文件引用边界见[第 03 章](../chapters/03-sync-rpc.md)。

## 7. 手动运行各机制的驱动测试

先完成第 6 节，确认自己的 `add_server` 已退出，然后逐项运行。每条命令返回后立即执行 `echo $?`，应为 `0`；完整输出中还应包含对应汇总标记。

```sh
/rpc_test
/mapping_test
/parcel_test
/transaction_test
/node_test
/ref_test
```

| 程序 | 主要观察 | 汇总标记 |
| --- | --- | --- |
| `/rpc_test` | 并发请求、容量、回复资格、退出和中断 | `RPC_TEST_PASS` |
| `/mapping_test` | 只读接收区、槽归还、取消和映射生命周期 | `MAPPING_TEST_PASS` |
| `/parcel_test` | 消息格式校验、错误拒绝与回滚 | `PARCEL_TEST_PASS` |
| `/transaction_test` | 回复关联、回复槽、取消与资源回收 | `TRANSACTION_TEST_PASS` |
| `/node_test` | 多对象、独立计数状态、对象持有与移除 | `NODE_TEST_PASS` |
| `/ref_test` | 三进程私有 Handle、引用转换及死亡清理 | `REF_TEST_PASS` |

`mapping_test` 故意让子进程写只读映射，因此可能输出用户进程 `segfault` 日志。测试必须确认该子进程按预期被信号终止，最终仍输出 `MAPPING_TEST_PASS` 并返回 `0`；内核 `BUG`、`WARNING`、`Oops` 或 panic 不能这样忽略。

某条测试失败或卡住时，停止继续执行后续测试，保留该项输出和 `dmesg`。本页手动 QEMU 会话没有宿主自动截止；销毁失败客户机并重启，避免沿用状态不明的驱动继续实验。完整自动验收还会核对退出状态、最终标记和内核诊断，见第 9 节。

## 8. 检查回收并关闭虚拟机

所有 Server/Client 和测试程序都退出后，在虚拟机中执行：

```sh
rmmod mini_binder
dmesg | busybox tail -n 5
```

当前版本应出现：

```text
mini_binder: device removed live_contexts=0 requests=0 regions=0 slots=0
mini_binder: nodes_empty=1
```

这些字段表明驱动自己的账目归零，不等于完成了整个内核的泄漏审计。若卸载提示模块仍在使用，先用 `busybox ps` 检查自己启动的后台程序是否尚未退出。

最后执行：

```sh
poweroff -f
```

QEMU 结束后回到宿主终端。不要用 `exit` 结束本页的虚拟机 shell：它是 PID 1，退出可能触发内核 panic。虚拟机里的 `/tmp/server.log` 和本次修改会随会话丢失；需要留存时在关机前复制终端输出。

## 9. 保存记录、修改代码与自动复验

需要记录整个交互过程时，可以在启动 QEMU **之前的宿主终端**运行：

```sh
mkdir -p artifacts
script -q artifacts/qemu-manual-session.log
```

这会进入记录中的宿主子 shell。在其中执行第 2 节的 QEMU 命令和后续实验；虚拟机关机、回到宿主后，再执行 `exit` 结束记录。该宿主 `exit` 与上一节的虚拟机 PID 1 不同。再次记录时换一个日志文件名，保留旧证据。

修改驱动或测试代码后，先关闭旧虚拟机，在宿主重新执行 `make rootfs`，再用第 2 节命令启动。正在运行的虚拟机使用启动时解压的镜像，宿主重编译不会替换客户机中已经加载的模块。

完成手动观察后，在宿主运行完整自动验收：

```sh
make test
make vm-test-kvm
```

没有可用 KVM 时运行 `make vm-test`。用户态测试与 VM 测试覆盖的层次不同：`make test` 不装载驱动；VM 命令重建镜像、运行 `/init`、检查整套验收结果并自动关机。VM 成功时最后输出 `PASS: VM assertions and log checks (kvm)` 或 `(tcg)`，工作日志分别为 `artifacts/qemu-kvm.log` 和 `artifacts/qemu-tcg.log`，同名运行会覆盖。

## 10. 现象不符合预期时先检查什么

| 现象 | 检查与处理 |
| --- | --- |
| 找不到 `bzImage`、静态 BusyBox 或 initramfs | 在宿主仓库根目录按顺序执行 `make kernel`、`make busybox`、`make rootfs` |
| KVM 权限或初始化失败 | 检查 `/dev/kvm` 的读写权限；也可改用 `accel=tcg` |
| 开机后自动跑测试并关机 | 启动时用了 `/init`；手动实验应为 `rdinit=/bin/sh` |
| 找不到 `/mini_binder.ko` 或 `/add_client` | 确认命令在虚拟机中执行，并使用本工作树 `make rootfs` 生成的镜像 |
| `ls: not found` | 使用 `busybox ls`；镜像只为部分 BusyBox 命令建立短名称 |
| 客户端 `transport error: open` | 先挂载 devtmpfs，再 `insmod /mini_binder.ko`；检查 `/dev/mini_binder` |
| 启动 Server 后仍是 ENOTCONN | 查看 `/tmp/server.log`，确认已经出现 `READY`，且未用完 COUNT 次回复 |
| Server 注册返回 `Device or resource busy` | 可能已有自己的 Server 或测试程序占用注册；用 `busybox ps` 检查，结束旧程序后再启动 |
| `wait` 一直不返回 | 确认已发足 COUNT 笔请求；检查 Server 日志和进程状态，必要时从宿主另一终端结束本次 QEMU 并重启 |
| 自动验收返回 `124` | 表示宿主截止到期；先看 `artifacts/qemu-kvm.log` 或 `qemu-tcg.log`，定位最后一条事件；空日志还需检查启动和终端输入问题 |

本页在 2026-11-17 用 Linux 6.6、BusyBox 1.36.1 的本机 KVM 交互会话逐项执行，观察到四类调用结果、全部七项设备/机制测试的成功标记，以及卸载时的零账目。该记录对应当前开发版，不代表后续章节拟议功能或完整 Android 服务调用已实现。
