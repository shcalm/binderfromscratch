// SPDX-License-Identifier: GPL-2.0-only
#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/vmalloc.h>
#include "mini_binder.h"

struct mb_slot {
    u64 token;
    bool delivered;
};

struct mb_region {
    struct mutex lock;
    void *data;
    atomic_t vmas;
    struct mb_slot slots[MB_MAP_SLOTS];
    u64 payload_copies;
    u64 payload_bytes;
};

struct mb_context {
    struct pid *owner;
    u64 id;
    bool busy;
    bool buffer_mode;
    struct mb_region region;
};

struct mb_transaction {
    struct list_head link;
    wait_queue_head_t wait;
    struct mb_request request;
    struct mb_call call;
    bool delivered;
    bool done;
    int error;
    bool buffered;
    struct mb_context *receiver;
    struct mb_buffer_call buffer_call;
    struct mb_buffer_request buffer_request;
};

static DEFINE_MUTEX(mb_lock);
static LIST_HEAD(transactions);
static DECLARE_WAIT_QUEUE_HEAD(server_wait);
static struct mb_context *server;
static u64 next_request;
static u64 next_token;
static unsigned int queued, delivered, live_requests;

static atomic64_t next_id = ATOMIC64_INIT(0);
static atomic64_t live_contexts = ATOMIC64_INIT(0);
static atomic64_t live_regions = ATOMIC64_INIT(0);
static atomic64_t live_slots = ATOMIC64_INIT(0);
static bool fail_after_register;
module_param(fail_after_register, bool, 0444);
MODULE_PARM_DESC(fail_after_register, "Exercise cleanup after device registration");
static bool fail_region_alloc;
module_param(fail_region_alloc, bool, 0444);
MODULE_PARM_DESC(fail_region_alloc, "Inject a receive-region allocation failure");

static void mb_vma_open(struct vm_area_struct *vma)
{
    struct mb_region *region = vma->vm_private_data;

    atomic_inc(&region->vmas);
}

static void mb_vma_close(struct vm_area_struct *vma)
{
    struct mb_region *region = vma->vm_private_data;

    atomic_dec(&region->vmas);
}

static const struct vm_operations_struct mb_vm_ops = {
    .open = mb_vma_open,
    .close = mb_vma_close,
};

static int mb_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct mb_context *context = file->private_data;
    struct mb_region *region = &context->region;
    int error = 0;

    if (task_tgid(current) != context->owner)
        return -EPERM;
    if (PAGE_SIZE != MB_MAX_PAYLOAD || vma->vm_pgoff ||
        vma->vm_end - vma->vm_start != MB_MAP_BYTES ||
        !(vma->vm_flags & VM_SHARED) || !(vma->vm_flags & VM_READ) ||
        (vma->vm_flags & (VM_WRITE | VM_EXEC)))
        return -EINVAL;
    /* mmap holds an mm lock: never take mb_lock here or in VMA callbacks. */
    mutex_lock(&region->lock);
    if (region->data) {
        error = -EBUSY;
        goto unlock;
    }
    region->data = fail_region_alloc ? NULL : vmalloc_user(MB_MAP_BYTES);
    if (!region->data) {
        error = -ENOMEM;
        goto unlock;
    }
    vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY);
    vm_flags_clear(vma, VM_MAYWRITE | VM_MAYEXEC);
    error = remap_vmalloc_range(vma, region->data, 0);
    if (error) {
        vfree(region->data);
        region->data = NULL;
        goto unlock;
    }
    vma->vm_private_data = region;
    vma->vm_ops = &mb_vm_ops;
    mb_vma_open(vma);
    atomic64_inc(&live_regions);
unlock:
    mutex_unlock(&region->lock);
    return error;
}

/* Caller holds mb_lock, keeping the receiving context alive through copying. */
static int mb_region_reserve(struct mb_context *context,
                             struct mb_buffer_request *request, void **target)
{
    struct mb_region *region = &context->region;
    unsigned int slot;
    int error = 0;

    mutex_lock(&region->lock);
    if (!region->data || !atomic_read(&region->vmas)) {
        error = -ENXIO;
        goto unlock;
    }
    for (slot = 0; slot < MB_MAP_SLOTS; ++slot)
        if (!region->slots[slot].token)
            break;
    if (slot == MB_MAP_SLOTS) {
        error = -EAGAIN;
        goto unlock;
    }
    if (next_token == U64_MAX) {
        error = -EOVERFLOW;
        goto unlock;
    }
    request->token = ++next_token;
    request->offset = slot * MB_MAX_PAYLOAD;
    region->slots[slot].token = request->token;
    region->slots[slot].delivered = false;
    *target = (char *)region->data + request->offset;
    memset(*target, 0, MB_MAX_PAYLOAD);
    atomic64_inc(&live_slots);
unlock:
    mutex_unlock(&region->lock);
    return error;
}

static int mb_region_free(struct mb_region *region, u64 token, bool rollback)
{
    unsigned int slot;
    int error = -ENOENT;

    mutex_lock(&region->lock);
    for (slot = 0; slot < MB_MAP_SLOTS; ++slot) {
        if (!token || region->slots[slot].token != token)
            continue;
        if (!rollback && !region->slots[slot].delivered) {
            error = -EBUSY;
            break;
        }
        region->slots[slot].token = 0;
        region->slots[slot].delivered = false;
        atomic64_dec(&live_slots);
        error = 0;
        break;
    }
    mutex_unlock(&region->lock);
    return error;
}

static void mb_region_stats(struct mb_region *region,
                            struct mb_buffer_stats *stats)
{
    unsigned int slot;

    mutex_lock(&region->lock);
    for (slot = 0; slot < MB_MAP_SLOTS; ++slot)
        stats->used_slots += region->slots[slot].token != 0;
    stats->mapped_vmas = atomic_read(&region->vmas);
    stats->payload_copies = region->payload_copies;
    stats->payload_bytes = region->payload_bytes;
    mutex_unlock(&region->lock);
    stats->live_regions = atomic64_read(&live_regions);
    stats->live_slots = atomic64_read(&live_slots);
}

static int mb_open(struct inode *inode, struct file *file)
{
    struct mb_context *context;

    context = kzalloc(sizeof(*context), GFP_KERNEL);
    if (!context)
        return -ENOMEM;
    context->owner = get_pid(task_tgid(current));
    mutex_init(&context->region.lock);
    atomic_set(&context->region.vmas, 0);
    context->id = atomic64_inc_return(&next_id);
    file->private_data = context;
    atomic64_inc(&live_contexts);
    return 0;
}

static void mb_finish(struct mb_transaction *transaction, int error)
{
    if (transaction->buffered && !transaction->delivered)
        mb_region_free(&transaction->receiver->region,
                       transaction->buffer_request.token, true);
    if (transaction->delivered)
        delivered--;
    else
        queued--;
    transaction->error = error;
    transaction->receiver = NULL;
    WRITE_ONCE(transaction->done, true);
    wake_up_all(&transaction->wait);
}

enum mb_call_mode {
    MB_CALL_FIXED,
    MB_CALL_BUFFER,
    MB_CALL_PARCEL,
};

static int mb_validate_offsets(u32 size, const u32 *offsets, u32 count)
{
    unsigned int i;

    for (i = 0; i < count; ++i) {
        u32 offset = offsets[i];

        if (offset % 8 || offset > size ||
            size - offset < sizeof(struct mb_object_header))
            return -EINVAL;
        if (i && (offset <= offsets[i - 1] ||
                  offset - offsets[i - 1] < sizeof(struct mb_object_header)))
            return -EINVAL;
    }
    /* No header contents are interpreted while every type is unsupported.
     * Future types must inspect the receive backing and check full sizes.
     */
    return count ? -EOPNOTSUPP : 0;
}

static long mb_call(struct mb_context *context, void __user *arg,
                    enum mb_call_mode mode)
{
    struct mb_transaction *transaction;
    struct mb_parcel_call parcel_call = { 0 };
    u32 offsets[MB_MAX_OBJECTS];
    bool buffered = mode != MB_CALL_FIXED;
    void *target;
    int error;

    transaction = kzalloc(sizeof(*transaction), GFP_KERNEL);
    if (!transaction)
        return -ENOMEM;
    transaction->buffered = buffered;
    if (mode == MB_CALL_PARCEL) {
        if (copy_from_user(&parcel_call, arg, sizeof(parcel_call))) {
            error = -EFAULT;
            goto free;
        }
        if (!parcel_call.size || parcel_call.size > MB_MAX_PAYLOAD ||
            parcel_call.offsets_count > MB_MAX_OBJECTS ||
            (!parcel_call.offsets_count && parcel_call.offsets)) {
            error = -EINVAL;
            goto free;
        }
        if (parcel_call.offsets_count &&
            copy_from_user(offsets, u64_to_user_ptr(parcel_call.offsets),
                           parcel_call.offsets_count * sizeof(offsets[0]))) {
            error = -EFAULT;
            goto free;
        }
        transaction->buffer_call.data = parcel_call.data;
        transaction->buffer_call.size = parcel_call.size;
    } else if (buffered ? copy_from_user(&transaction->buffer_call, arg,
                                  sizeof(transaction->buffer_call)) :
                   copy_from_user(&transaction->call, arg,
                                  sizeof(transaction->call))) {
        error = -EFAULT;
        goto free;
    }
    if (buffered && (!transaction->buffer_call.size ||
        transaction->buffer_call.size > MB_MAX_PAYLOAD ||
        transaction->buffer_call.reserved)) {
        error = -EINVAL;
        goto free;
    }
    transaction->call.result = 0;
    transaction->call.status = 0;
    transaction->buffer_call.result = 0;
    transaction->buffer_call.status = 0;
    init_waitqueue_head(&transaction->wait);
    mutex_lock(&mb_lock);
    if (!server)
        error = -ENOTCONN;
    else if (server == context)
        error = -EDEADLK;
    else if (server->buffer_mode != buffered)
        error = -EPROTOTYPE;
    else if (context->busy)
        error = -EBUSY;
    else if (live_requests >= MB_MAX_REQUESTS)
        error = -EAGAIN;
    else if (next_request == U64_MAX)
        error = -EOVERFLOW;
    else
        error = 0;
    if (error) {
        mutex_unlock(&mb_lock);
        goto free;
    }
    if (buffered) {
        error = mb_region_reserve(server, &transaction->buffer_request, &target);
        if (error) {
            mutex_unlock(&mb_lock);
            goto free;
        }
        if (copy_from_user(target,
             u64_to_user_ptr(transaction->buffer_call.data),
             transaction->buffer_call.size))
            error = -EFAULT;
        else if (mode == MB_CALL_PARCEL)
            error = mb_validate_offsets(transaction->buffer_call.size, offsets,
                                        parcel_call.offsets_count);
        if (error) {
            mb_region_free(&server->region,
                           transaction->buffer_request.token, true);
            mutex_unlock(&mb_lock);
            goto free;
        }
        mutex_lock(&server->region.lock);
        /* Count payloads accepted for publication, not all attempted copies. */
        server->region.payload_copies++;
        server->region.payload_bytes += transaction->buffer_call.size;
        mutex_unlock(&server->region.lock);
        transaction->receiver = server;
        transaction->buffer_request.size = transaction->buffer_call.size;
    }
    context->busy = true;
    transaction->request.id = ++next_request;
    transaction->buffer_request.id = transaction->request.id;
    transaction->request.a = transaction->call.a;
    transaction->request.b = transaction->call.b;
    list_add_tail(&transaction->link, &transactions);
    queued++;
    live_requests++;
    mutex_unlock(&mb_lock);
    wake_up_all(&server_wait);

    error = wait_event_interruptible(transaction->wait,
                                    READ_ONCE(transaction->done));
    mutex_lock(&mb_lock);
    if (transaction->done)
        error = transaction->error;
    else {
        /* Restarting the ioctl would submit a new business request. */
        if (error == -ERESTARTSYS)
            error = -EINTR;
        mb_finish(transaction, error);
    }
    list_del(&transaction->link);
    live_requests--;
    context->busy = false;
    mutex_unlock(&mb_lock);
    if (!error) {
        if (mode == MB_CALL_PARCEL) {
            parcel_call.result = transaction->buffer_call.result;
            parcel_call.status = transaction->buffer_call.status;
            if (copy_to_user(arg, &parcel_call, sizeof(parcel_call)))
                error = -EFAULT;
        } else if (buffered ? copy_to_user(arg, &transaction->buffer_call,
                                    sizeof(transaction->buffer_call)) :
                       copy_to_user(arg, &transaction->call,
                                    sizeof(transaction->call)))
            error = -EFAULT;
    }
free:
    kfree(transaction);
    return error;
}

static long mb_receive(struct mb_context *context, void __user *arg, bool buffered)
{
    struct mb_transaction *transaction;
    int error;

    for (;;) {
        mutex_lock(&mb_lock);
        if (server != context) {
            mutex_unlock(&mb_lock);
            return -EPERM;
        }
        if (context->buffer_mode != buffered) {
            mutex_unlock(&mb_lock);
            return -EPROTOTYPE;
        }
        list_for_each_entry(transaction, &transactions, link) {
            if (transaction->done || transaction->delivered)
                continue;
            if (buffered ? copy_to_user(arg, &transaction->buffer_request,
                                        sizeof(transaction->buffer_request)) :
                           copy_to_user(arg, &transaction->request,
                                        sizeof(transaction->request))) {
                mutex_unlock(&mb_lock);
                return -EFAULT;
            }
            if (buffered) {
                unsigned int slot = transaction->buffer_request.offset /
                                    MB_MAX_PAYLOAD;

                mutex_lock(&context->region.lock);
                context->region.slots[slot].delivered = true;
                mutex_unlock(&context->region.lock);
                transaction->receiver = NULL;
            }
            transaction->delivered = true;
            queued--;
            delivered++;
            mutex_unlock(&mb_lock);
            return 0;
        }
        mutex_unlock(&mb_lock);
        error = wait_event_interruptible(server_wait,
                  READ_ONCE(queued) || READ_ONCE(server) != context);
        if (error)
            return error;
    }
}

static long mb_reply(struct mb_context *context, void __user *arg)
{
    struct mb_reply reply;
    struct mb_transaction *transaction;
    int error = -ENOENT;

    if (copy_from_user(&reply, arg, sizeof(reply)))
        return -EFAULT;
    mutex_lock(&mb_lock);
    if (context != server) {
        error = -EPERM;
        goto unlock;
    }
    list_for_each_entry(transaction, &transactions, link) {
        if (transaction->request.id != reply.id || transaction->done ||
            !transaction->delivered)
            continue;
        transaction->call.result = reply.result;
        transaction->call.status = reply.status;
        transaction->buffer_call.result = reply.result;
        transaction->buffer_call.status = reply.status;
        mb_finish(transaction, 0);
        error = 0;
        break;
    }
unlock:
    mutex_unlock(&mb_lock);
    return error;
}

static long mb_ioctl(struct file *file, unsigned int command, unsigned long arg)
{
    struct mb_context *context = file->private_data;
    struct mb_query query = { 0 };
    struct mb_stats stats = { 0 };
    struct mb_buffer_stats buffer_stats = { 0 };
    u64 token;
    void __user *user = (void __user *)arg;
    int error;

    if (task_tgid(current) != context->owner)
        return -EPERM;
    switch (command) {
    case MB_IOC_QUERY:
        query.version = MB_ABI_VERSION;
        query.features = MB_FEATURE_RPC | MB_FEATURE_MMAP | MB_FEATURE_PARCEL;
        query.open_id = context->id;
        query.live_contexts = atomic64_read(&live_contexts);
        return copy_to_user(user, &query, sizeof(query)) ? -EFAULT : 0;
    case MB_IOC_REGISTER:
    case MB_IOC_REGISTER_BUFFER:
        mutex_lock(&mb_lock);
        error = server || context->busy ? -EBUSY : 0;
        if (!error && command == MB_IOC_REGISTER_BUFFER &&
            !atomic_read(&context->region.vmas))
            error = -ENXIO;
        if (!error) {
            context->buffer_mode = command == MB_IOC_REGISTER_BUFFER;
            server = context;
        }
        mutex_unlock(&mb_lock);
        return error;
    case MB_IOC_CALL:
        return mb_call(context, user, MB_CALL_FIXED);
    case MB_IOC_CALL_BUFFER:
        return mb_call(context, user, MB_CALL_BUFFER);
    case MB_IOC_CALL_PARCEL:
        return mb_call(context, user, MB_CALL_PARCEL);
    case MB_IOC_RECEIVE:
        return mb_receive(context, user, false);
    case MB_IOC_RECEIVE_BUFFER:
        return mb_receive(context, user, true);
    case MB_IOC_REPLY:
        return mb_reply(context, user);
    case MB_IOC_STATS:
        mutex_lock(&mb_lock);
        stats.queued = queued;
        stats.delivered = delivered;
        stats.live_requests = live_requests;
        stats.server_registered = server != NULL;
        mutex_unlock(&mb_lock);
        return copy_to_user(user, &stats, sizeof(stats)) ? -EFAULT : 0;
    case MB_IOC_FREE_BUFFER:
        if (copy_from_user(&token, user, sizeof(token)))
            return -EFAULT;
        return mb_region_free(&context->region, token, false);
    case MB_IOC_BUFFER_STATS:
        mb_region_stats(&context->region, &buffer_stats);
        return copy_to_user(user, &buffer_stats, sizeof(buffer_stats)) ? -EFAULT : 0;
    default:
        return -ENOTTY;
    }
}

static int mb_release(struct inode *inode, struct file *file)
{
    struct mb_context *context = file->private_data;
    struct mb_transaction *transaction;
    unsigned int slot;

    mutex_lock(&mb_lock);
    if (server == context) {
        server = NULL;
        list_for_each_entry(transaction, &transactions, link) {
            if (!transaction->done)
                mb_finish(transaction, -EPIPE);
        }
        wake_up_all(&server_wait);
    }
    mutex_unlock(&mb_lock);
    for (slot = 0; slot < MB_MAP_SLOTS; ++slot)
        if (context->region.slots[slot].token)
            atomic64_dec(&live_slots);
    if (context->region.data) {
        vfree(context->region.data);
        atomic64_dec(&live_regions);
    }
    put_pid(context->owner);
    kfree(context);
    atomic64_dec(&live_contexts);
    return 0;
}

static const struct file_operations mb_fops = {
    .owner = THIS_MODULE,
    .open = mb_open,
    .release = mb_release,
    .unlocked_ioctl = mb_ioctl,
    .mmap = mb_mmap,
    .llseek = no_llseek,
};

static struct miscdevice mb_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "mini_binder",
    .fops = &mb_fops,
    .mode = 0600,
};

static int __init mb_init(void)
{
    int error = misc_register(&mb_device);

    if (!error && fail_after_register) {
        misc_deregister(&mb_device);
        pr_info("mini_binder: initialization rollback after register\n");
        return -EIO;
    }
    if (!error)
        pr_info("mini_binder: device registered ABI=%u\n", MB_ABI_VERSION);
    return error;
}

static void __exit mb_exit(void)
{
    misc_deregister(&mb_device);
    pr_info("mini_binder: device removed live_contexts=%lld requests=%u regions=%lld slots=%lld\n",
            atomic64_read(&live_contexts), live_requests,
            atomic64_read(&live_regions), atomic64_read(&live_slots));
}

module_init(mb_init);
module_exit(mb_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Incrementally designed teaching IPC device");
