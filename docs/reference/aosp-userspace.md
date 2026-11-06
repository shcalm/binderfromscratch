# AOSP 用户态 Binder 阅读基线

本文固定第 05 章及后续用户态对照的来源，补足 [Linux v6.6 驱动基线](android-binder.md)。这是源码阅读基线，不是已构建运行的 Android 系统；当前真实 Binder 实验只有驱动协议探测，不能称为 libbinder/ServiceManager 端到端验收。

## 1. 版本与获取

项目：AOSP `platform/frameworks/native`；标签：`android-14.0.0_r1`；注解标签对象 `8b173ce18fa9ac16daa9adcac8aa417eed1179e6`；所指提交 **`bfcf75076e562945bae131a4929f29a90d0d2481`**。标签元数据说明 Android 14.0.0 Release 1，记录日期为 2026-11-06。

元数据由 [GitHub AOSP 镜像标签接口](https://api.github.com/repos/aosp-mirror-neo/platform_frameworks_native/git/tags/8b173ce18fa9ac16daa9adcac8aa417eed1179e6)核对，本次未执行标签的 PGP 签名验证。官方源码入口为 [Gitiles 的相同提交](https://android.googlesource.com/platform/frameworks/native/+/bfcf75076e562945bae131a4929f29a90d0d2481/)。本机官方站连接超时，实际通过镜像内容接口按提交取源，并记录内容哈希；下载来源与源码版本是两项信息。

```sh
bash tools/reference/fetch-aosp.sh
sha256sum build/reference/aosp-frameworks-native/*
```

工具只取以下四个阅读文件，保存到忽略的 build 目录，并校验预期哈希；已存在文件也重新校验，校验失败不会默默改用主线。首次获取需要网络，可用 `AOSP_REF_DIR=/absolute/directory` 选择另一缓存目录。下载失败、API 限流或内容哈希错误均为失败，不能据此声称已阅读相同版本。

| 路径 | 本次 SHA-256 |
| --- | --- |
| `libs/binder/Parcel.cpp` | `6bc86cdf9347df82f46c06874f4f441d501de5e9d57263025deee02ebe263baf` |
| `libs/binder/include/binder/Parcel.h` | `1370bd744f4f392511e4d16c34f333f826497a0ce5101c8189d924fd0615dbc4` |
| `libs/binder/IPCThreadState.cpp` | `aaf58ce5087c882a75e64698c4d9d752c55e0e31d8011edb9a45e1f0231dae44` |
| `libs/binder/ProcessState.cpp` | `4146f161a7aac9e6ef1496798986325496d913a2231e63fe5d8e00be9b5e8ee1` |

四个文件头保留各自 Apache-2.0 授权说明；它们属于第三方代码，不因下载到本仓库 build 目录而变成本项目 MIT 原创代码。这里只阅读必要片段，不提供完整 Android 构建环境。

## 2. 从普通字节到对象偏移表

以下行号实际核对上述 Parcel.cpp 与 IPCThreadState.cpp，链接固定到提交，讨论 **kernel Binder** 分支；此版本还包含 RPC Binder 等其他路径，不能混合解释。

| 设计问题 | 源码入口 | 对应结论与教学边界 |
| --- | --- | --- |
| 整数由谁编码？ | [Parcel::writeInt32，1254 行](https://github.com/aosp-mirror-neo/platform_frameworks_native/blob/bfcf75076e562945bae131a4929f29a90d0d2481/libs/binder/Parcel.cpp#L1254)、[writeAligned，1886 行](https://github.com/aosp-mirror-neo/platform_frameworks_native/blob/bfcf75076e562945bae131a4929f29a90d0d2481/libs/binder/Parcel.cpp#L1886) | 用户态库用 memcpy 写入数据区并推进游标，驱动不逐个解释普通整数。教学显式 little-endian 格式不是由这些源码推出的通用 Binder 网络字节序规范。 |
| 字符串是否只传指针？ | [writeString8，1363 行](https://github.com/aosp-mirror-neo/platform_frameworks_native/blob/bfcf75076e562945bae131a4929f29a90d0d2481/libs/binder/Parcel.cpp#L1363)、[writeString16，1386 行](https://github.com/aosp-mirror-neo/platform_frameworks_native/blob/bfcf75076e562945bae131a4929f29a90d0d2481/libs/binder/Parcel.cpp#L1386) | 写长度、实际字符及结束符；空指针写 -1，String16 的长度按字符单元解释。教学 byte-string 若不含结束符、采用不同头部或版本，必须明确不兼容；不能把 C 字符指针数字当内容。 |
| 数组长度如何约束？ | [writeInt32Array，1264 行](https://github.com/aosp-mirror-neo/platform_frameworks_native/blob/bfcf75076e562945bae131a4929f29a90d0d2481/libs/binder/Parcel.cpp#L1264) | 检查长度，写计数和元素。普通字段的类型/顺序由服务接口约定，不是驱动猜测 C struct 布局。 |
| 为什么还需要对象索引？ | [writeObject，1641 行](https://github.com/aosp-mirror-neo/platform_frameworks_native/blob/bfcf75076e562945bae131a4929f29a90d0d2481/libs/binder/Parcel.cpp#L1641) | flat_binder_object 写入数据区；需要元数据时记录对象所在的 mDataPos，维护 mObjects，并参与用户态对象引用协作。不能把对象偏移表解释为全部普通字段的类型表。 |
| 数据和对象表如何交给驱动？ | [IPCThreadState::writeTransactionData，1231 行](https://github.com/aosp-mirror-neo/platform_frameworks_native/blob/bfcf75076e562945bae131a4929f29a90d0d2481/libs/binder/IPCThreadState.cpp#L1231) | 分别填 data_size/buffer 与 offsets_size/offsets，offsets_size 是字节数。mOut 再写 BC 命令和事务结构；不是每个字段各发一次独立 ioctl。 |

驱动侧的 [binder_object_header/flat_binder_object](https://github.com/torvalds/linux/blob/v6.6/include/uapi/linux/android/binder.h#L63)定义特殊对象类型及描述符。`flat_binder_object` 的 binder/cookie 或 handle 等字段不是普通用户态地址都可在内核解引用的许可证；真正对象与引用转换在后续 Node/Ref 章节展开。此处只识别“数据区内特殊描述符 + 单独偏移表”的职责边界。

## 3. 本次证据与未实现内容

已核对源码版本、四个文件哈希、上述编解码/对象表/事务封装入口。没有在此构建 AOSP libbinder、运行它的 Parcel 测试或用该版本 ServiceManager 完成服务调用。Linux v6.6 与 Android 14 用户态是分开固定的两个阅读输入，不表示任意二者构建组合已经由本教程验证。

后续 mini Parcel 必须独立定义格式、校验与拒绝未知对象类型；“借鉴设计”不等于 ABI 兼容。普通数据模型通过后，还要在映射接收路径验证编解码和失败释放，才算该章实现闭环。
