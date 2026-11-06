# 可运行内核与实验环境清单

## 固定输入

- 内核：上游 Linux 6.6，归档 `https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.6.tar.xz`。
- 默认通过清华 kernel 镜像获取同一归档，仍以官方 SHA-256 验证。可设 `KERNEL_MIRROR=https://cdn.kernel.org/pub/linux/kernel` 使用官方地址。
- SHA-256：`d926a06c63dd8ac7df3f86ee1ffc2ce2a3b81a2d168484e76b5b389aba8e56d0`，来自 kernel.org v6.x 校验清单。
- 配置：x86_64_defconfig 加 `tools/kernel/config`；完整解析后配置保存到 `artifacts/kernel.config`。
- 模块：本仓库 `examples/modules/hello/`，通过同一内核输出目录编译。
- rootfs：静态 BusyBox。首轮使用宿主 `/usr/bin/busybox` 并记录版本与哈希；`BUSYBOX` 可覆盖。此时不承诺不同宿主获得字节一致的 rootfs。
- 运行器：QEMU x86_64，默认 TCG；可显式 `ACCEL=kvm`，两种模式分别实测后记录。

## 构建与运行

宿主需要 C 编译工具、Make、flex、bison、bc、libelf 开发文件、OpenSSL 开发文件、curl、xz、cpio、gzip、静态 BusyBox、QEMU。Ubuntu/Debian 对应包通常包含 `build-essential flex bison bc libelf-dev libssl-dev curl xz-utils cpio busybox-static qemu-system-x86`。

```sh
bash tools/kernel/build.sh
bash tools/qemu/build-rootfs.sh
bash tools/qemu/run.sh
```

首次完整编译可能耗时数分钟，并占用若干 GB；默认八个编译任务，可用 `JOBS=4` 调整。构建归档、源码和输出都在忽略的 `build/`，日志在 `artifacts/`。重新开始配置时删除 `build/kernel/out` 后重新运行；不要只编辑 fragment 却误以为已有输出自动重新配置。

运行器限制为 120 秒（`VM_TIMEOUT` 可调整），QEMU 不挂载宿主目录、不配置外部网络。超时非通过。init 断言、内核告警、缺少成功标记与 QEMU 非零退出都使运行失败。

## 证据

构建完成后查看 `artifacts/kernel-build.txt`、`artifacts/rootfs-build.txt` 与 `artifacts/qemu-tcg.log`。这些是实际本机产物，不把本文列出的输入当作已通过实验的证明。阶段报告另行记录实测输出和限制。

当前使用 GPL 内核与 BusyBox；若发布二进制镜像，需要同时提供相应许可证、准确源码与构建材料，不能只有镜像下载地址。
