/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef MINI_BINDER_UAPI_H
#define MINI_BINDER_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define MB_ABI_VERSION 1
#define MB_FEATURE_RPC 1
#define MB_FEATURE_MMAP 2
#define MB_MAX_REQUESTS 64
#define MB_MAP_BYTES 65536
#define MB_MAP_SLOTS 16
#define MB_MAX_PAYLOAD 4096

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

struct mb_buffer_call {
    __u64 data;
    __u32 size;
    __u32 reserved;
    __u32 result;
    __s32 status;
};

struct mb_buffer_request {
    __u64 id;
    __u64 token;
    __u64 offset;
    __u32 size;
    __u32 reserved;
};

struct mb_buffer_stats {
    __u32 used_slots;
    __u32 mapped_vmas;
    __u64 payload_copies;
    __u64 payload_bytes;
    __u64 live_regions;
    __u64 live_slots;
};

#define MB_IOC_REGISTER_BUFFER _IO('m', 6)
#define MB_IOC_CALL_BUFFER _IOWR('m', 7, struct mb_buffer_call)
#define MB_IOC_RECEIVE_BUFFER _IOR('m', 8, struct mb_buffer_request)
#define MB_IOC_FREE_BUFFER _IOW('m', 9, __u64)
#define MB_IOC_BUFFER_STATS _IOR('m', 10, struct mb_buffer_stats)

#endif
