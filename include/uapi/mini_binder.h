/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef MINI_BINDER_UAPI_H
#define MINI_BINDER_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define MB_ABI_VERSION 1

struct mb_query {
    __u32 version;
    __u32 features;
    __u64 open_id;
    __u64 live_contexts;
};

#define MB_IOC_QUERY _IOR('m', 0, struct mb_query)

#endif
