/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef MINI_BINDER_UAPI_H
#define MINI_BINDER_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define MB_ABI_VERSION 1
#define MB_FEATURE_RPC 1
#define MB_MAX_REQUESTS 64

struct mb_query {
    __u32 version;
    __u32 features;
    __u64 open_id;
    __u64 live_contexts;
};

#define MB_IOC_QUERY _IOR('m', 0, struct mb_query)

struct mb_call {
    __u32 a;
    __u32 b;
    __u32 result;
    __s32 status;
};

struct mb_request {
    __u64 id;
    __u32 a;
    __u32 b;
};

struct mb_reply {
    __u64 id;
    __u32 result;
    __s32 status;
};

struct mb_stats {
    __u64 queued;
    __u64 delivered;
    __u64 live_requests;
    __u32 server_registered;
    __u32 reserved;
};

#define MB_IOC_REGISTER _IO('m', 1)
#define MB_IOC_CALL _IOWR('m', 2, struct mb_call)
#define MB_IOC_RECEIVE _IOR('m', 3, struct mb_request)
#define MB_IOC_REPLY _IOW('m', 4, struct mb_reply)
#define MB_IOC_STATS _IOR('m', 5, struct mb_stats)

#endif
