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
#include <asm/unaligned.h>
#include "mini_binder.h"

struct mb_object_hold {
    struct mb_node *node;
    struct mb_ref *ref;
};

struct mb_slot {
    u64 token;
    bool delivered;
    bool pending;
    struct mb_node *node;
    u32 size;
    u32 offsets_count;
    u32 offsets[MB_MAX_OBJECTS];
    u32 holds_count;
    struct mb_object_hold holds[MB_MAX_OBJECTS];
};

struct mb_region {
    struct mutex lock;
    void *data;
    atomic_t vmas;
    struct mb_slot slots[MB_MAP_SLOTS];
    u64 payload_copies;
    u64 payload_bytes;
};

enum mb_service_mode {
    MB_SERVICE_FIXED,
    MB_SERVICE_BUFFER,
    MB_SERVICE_TRANSACTION,
    MB_SERVICE_NODE,
    MB_SERVICE_REF,
};

enum mb_transaction_state {
    MB_TX_QUEUED,
    MB_TX_DELIVERED,
    MB_TX_REPLIED,
    MB_TX_FAILED,
};

struct mb_context {
    struct list_head link;
    struct list_head refs;
    struct pid *owner;
    u64 id;
    unsigned int active_calls;
    bool exclusive_call;
    bool ref_enabled;
    enum mb_service_mode mode;
    struct mb_region region;
};

struct mb_node {
    struct list_head link;
    struct mb_context *owner;
    u64 id;
    u64 cookie;
    unsigned int transaction_refs;
    unsigned int slot_refs;
    u32 ref_entries;
    u32 local_object_refs;
};

struct mb_ref {
    struct list_head link;
    struct mb_node *node;
    u32 handle;
    u32 user_refs;
    u32 buffer_refs;
};

struct mb_transaction {
    struct list_head link;
    wait_queue_head_t wait;
    struct mb_request request;
    struct mb_call call;
    enum mb_transaction_state state;
    int error;
    bool buffered;
    struct mb_context *receiver;
    struct mb_context *caller;
    struct mb_node *node;
    u32 code;
    struct mb_transaction_call transaction_call;
    struct mb_buffer_call buffer_call;
    struct mb_buffer_request buffer_request;
    struct mb_ref_call ref_call;
    struct mb_ref_buffer ref_request;
};

static DEFINE_MUTEX(mb_lock);
static LIST_HEAD(transactions);
static LIST_HEAD(nodes);
static LIST_HEAD(contexts);
static DECLARE_WAIT_QUEUE_HEAD(server_wait);
static struct mb_context *server;
static u64 next_request;
static u64 next_token;
static u64 next_node;
static unsigned int queued, delivered, live_requests;

static u64 next_id;
static atomic64_t live_contexts = ATOMIC64_INIT(0);
static atomic64_t live_regions = ATOMIC64_INIT(0);
static atomic64_t live_slots = ATOMIC64_INIT(0);
static bool fail_after_register;
module_param(fail_after_register, bool, 0444);
MODULE_PARM_DESC(fail_after_register, "Exercise cleanup after device registration");
static bool fail_region_alloc;
module_param(fail_region_alloc, bool, 0444);
MODULE_PARM_DESC(fail_region_alloc, "Inject a receive-region allocation failure");

/* Node identity and all holds are protected by mb_lock. */
static struct mb_node *mb_find_node(u64 id)
{
    struct mb_node *node;

    list_for_each_entry(node, &nodes, link)
        if (node->id == id)
            return node;
    return NULL;
}

static void mb_node_reap(struct mb_node *node)
{
    if (!node || node->owner || node->transaction_refs || node->slot_refs ||
        node->ref_entries || node->local_object_refs)
        return;
    list_del(&node->link);
    kfree(node);
}

static struct mb_ref *mb_find_ref(struct mb_context *context, u32 handle)
{
    struct mb_ref *ref;

    list_for_each_entry(ref, &context->refs, link)
        if (ref->handle == handle)
            return ref;
    return NULL;
}

static void mb_ref_reap(struct mb_ref *ref)
{
    struct mb_node *node = ref->node;

    if (ref->user_refs || ref->buffer_refs)
        return;
    list_del(&ref->link);
    node->ref_entries--;
    kfree(ref);
    mb_node_reap(node);
}

/* Each successful conversion owns one ledger unit, including duplicates. */
static void mb_slot_drop_objects(struct mb_slot *slot)
{
    while (slot->holds_count) {
        struct mb_object_hold *hold = &slot->holds[--slot->holds_count];

        if (hold->ref) {
            hold->ref->buffer_refs--;
            mb_ref_reap(hold->ref);
        } else {
            hold->node->local_object_refs--;
            mb_node_reap(hold->node);
        }
    }
}

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

/* Caller holds mb_lock, including for slots without a target Node. */
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
        mb_slot_drop_objects(&region->slots[slot]);
        if (region->slots[slot].node) {
            struct mb_node *node = region->slots[slot].node;

            region->slots[slot].node = NULL;
            node->slot_refs--;
            mb_node_reap(node);
        }
        memset(&region->slots[slot], 0, sizeof(region->slots[slot]));
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
    INIT_LIST_HEAD(&context->refs);
    mutex_init(&context->region.lock);
    atomic_set(&context->region.vmas, 0);
    mutex_lock(&mb_lock);
    if (next_id == U64_MAX) {
        mutex_unlock(&mb_lock);
        put_pid(context->owner);
        kfree(context);
        return -EOVERFLOW;
    }
    context->id = ++next_id;
    list_add_tail(&context->link, &contexts);
    mutex_unlock(&mb_lock);
    file->private_data = context;
    atomic64_inc(&live_contexts);
    return 0;
}

static void mb_finish(struct mb_transaction *transaction, int error)
{
    if (transaction->state >= MB_TX_REPLIED)
        return;
    if (transaction->buffered && transaction->state == MB_TX_QUEUED)
        mb_region_free(&transaction->receiver->region,
                       transaction->buffer_request.token, true);
    if (transaction->state == MB_TX_DELIVERED)
        delivered--;
    else
        queued--;
    transaction->error = error;
    transaction->receiver = NULL;
    WRITE_ONCE(transaction->state, error ? MB_TX_FAILED : MB_TX_REPLIED);
    wake_up_all(&transaction->wait);
}

enum mb_call_mode {
    MB_CALL_FIXED,
    MB_CALL_BUFFER,
    MB_CALL_PARCEL,
    MB_CALL_TRANSACTION,
    MB_CALL_NODE,
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

static int mb_ref_copy_offsets(const struct mb_ref_parcel *parcel,
                                u32 *offsets, bool empty_allowed)
{
    if (parcel->size > MB_MAX_PAYLOAD ||
        (!parcel->size && (!empty_allowed || parcel->data ||
                          parcel->offsets || parcel->offsets_count)) ||
        parcel->offsets_count > MB_MAX_OBJECTS ||
        (!parcel->offsets_count && parcel->offsets))
        return -EINVAL;
    if (parcel->offsets_count &&
        copy_from_user(offsets, u64_to_user_ptr(parcel->offsets),
                       parcel->offsets_count * sizeof(*offsets)))
        return -EFAULT;
    return 0;
}

static int mb_ref_validate_objects(const void *data, u32 size,
                                   const u32 *offsets, u32 count)
{
    unsigned int i;

    for (i = 0; i < count; i++) {
        const char *object;
        u32 offset = offsets[i], type;

        if (offset % 8 || offset > size || size - offset < 8 ||
            (i && (offset <= offsets[i - 1] ||
                   offset - offsets[i - 1] < sizeof(struct mb_ref_object))))
            return -EINVAL;
        object = (const char *)data + offset;
        type = get_unaligned_le32(object);
        if (type != MB_OBJECT_LOCAL && type != MB_OBJECT_HANDLE)
            return -EOPNOTSUPP;
        if (size - offset < sizeof(struct mb_ref_object) ||
            get_unaligned_le32(object + 4))
            return -EINVAL;
        if (type == MB_OBJECT_HANDLE &&
            (get_unaligned_le64(object + 8) > U32_MAX ||
             get_unaligned_le64(object + 16)))
            return -EINVAL;
        if (type == MB_OBJECT_LOCAL &&
            (!get_unaligned_le64(object + 8) ||
             !get_unaligned_le64(object + 16)))
            return -EINVAL;
    }
    return 0;
}

static int mb_ref_convert(struct mb_context *sender,
                           struct mb_context *recipient,
                           struct mb_slot *slot, void *object)
{
    struct mb_node *node;
    struct mb_ref *ref;
    struct mb_object_hold *hold = &slot->holds[slot->holds_count];
    char *bytes = object;
    u32 handle, count = 0;

    if (get_unaligned_le32(bytes) == MB_OBJECT_LOCAL) {
        node = mb_find_node(get_unaligned_le64(bytes + 8));
        if (!node)
            return -ENOENT;
        if (node->owner != sender)
            return -EPERM;
        if (node->cookie != get_unaligned_le64(bytes + 16))
            return -EINVAL;
    } else {
        ref = mb_find_ref(sender, get_unaligned_le64(bytes + 8));
        if (!ref)
            return -EBADF;
        node = ref->node;
        if (!node->owner)
            return -EPIPE;
    }
    if (node->owner == recipient) {
        if (node->local_object_refs == U32_MAX)
            return -EOVERFLOW;
        node->local_object_refs++;
        hold->node = node;
        hold->ref = NULL;
        put_unaligned_le32(MB_OBJECT_LOCAL, bytes);
        put_unaligned_le64(node->id, bytes + 8);
        put_unaligned_le64(node->cookie, bytes + 16);
    } else {
        ref = NULL;
        {
            struct mb_ref *entry;

            list_for_each_entry(entry, &recipient->refs, link) {
                count++;
                if (entry->node == node)
                    ref = entry;
            }
        }
        if (!ref) {
            if (count >= MB_MAX_REFS)
                return -EAGAIN;
            if (node->ref_entries == U32_MAX)
                return -EOVERFLOW;
            for (handle = 1; mb_find_ref(recipient, handle); handle++)
                ;
            ref = kzalloc(sizeof(*ref), GFP_KERNEL);
            if (!ref)
                return -ENOMEM;
            ref->handle = handle;
            ref->node = node;
            list_add_tail(&ref->link, &recipient->refs);
            node->ref_entries++;
        }
        if (ref->buffer_refs == U32_MAX)
            return -EOVERFLOW;
        ref->buffer_refs++;
        hold->node = node;
        hold->ref = ref;
        put_unaligned_le32(MB_OBJECT_HANDLE, bytes);
        put_unaligned_le64(ref->handle, bytes + 8);
        put_unaligned_le64(0, bytes + 16);
    }
    put_unaligned_le32(0, bytes + 4);
    slot->holds_count++;
    return 0;
}

/* mb_lock protects the recipient and ledger until publication or rollback. */
static int mb_ref_build(struct mb_context *sender, struct mb_context *recipient,
                         const struct mb_ref_parcel *parcel, const u32 *offsets,
                         struct mb_ref_buffer *buffer)
{
    struct mb_region *region = &recipient->region;
    struct mb_slot *slot;
    void *data;
    unsigned int i;
    int error;

    memset(buffer, 0, sizeof(*buffer));
    if (!parcel->size)
        return 0;
    error = mb_region_reserve(recipient, &buffer->buffer, &data);
    if (error)
        return error;
    slot = &region->slots[buffer->buffer.offset / MB_MAX_PAYLOAD];
    if (copy_from_user(data, u64_to_user_ptr(parcel->data), parcel->size)) {
        error = -EFAULT;
        goto rollback;
    }
    error = mb_ref_validate_objects(data, parcel->size, offsets,
                                     parcel->offsets_count);
    if (error)
        goto rollback;
    for (i = 0; i < parcel->offsets_count; i++) {
        error = mb_ref_convert(sender, recipient, slot,
                                 (char *)data + offsets[i]);
        if (error)
            goto rollback;
    }
    buffer->buffer.size = parcel->size;
    buffer->offsets_count = parcel->offsets_count;
    memcpy(buffer->offsets, offsets, parcel->offsets_count * sizeof(*offsets));
    slot->size = parcel->size;
    slot->offsets_count = parcel->offsets_count;
    memcpy(slot->offsets, buffer->offsets, sizeof(slot->offsets));
    mutex_lock(&region->lock);
    region->payload_copies++;
    region->payload_bytes += parcel->size;
    mutex_unlock(&region->lock);
    return 0;
rollback:
    mb_region_free(region, buffer->buffer.token, true);
    memset(buffer, 0, sizeof(*buffer));
    return error;
}

static long mb_enable_ref(struct mb_context *context)
{
    unsigned int i;
    int error = 0;

    mutex_lock(&mb_lock);
    if (!context->ref_enabled) {
        if (server == context) {
            error = -EPROTOTYPE;
            goto unlock;
        }
        if (context->active_calls) {
            error = -EBUSY;
            goto unlock;
        }
        for (i = 0; i < MB_MAP_SLOTS; i++)
            if (context->region.slots[i].token) {
                error = -EBUSY;
                goto unlock;
            }
    }
    if (!atomic_read(&context->region.vmas))
        error = -ENXIO;
    else
        context->ref_enabled = true;
unlock:
    mutex_unlock(&mb_lock);
    return error;
}

static long mb_export_ref(struct mb_context *context, void __user *arg)
{
    struct mb_ref_export request;
    struct mb_ref_buffer buffer;
    struct mb_context *recipient = NULL, *entry;
    u32 offsets[MB_MAX_OBJECTS];
    int error;

    if (copy_from_user(&request, arg, sizeof(request)))
        return -EFAULT;
    if (!request.recipient)
        return -EINVAL;
    error = mb_ref_copy_offsets(&request.parcel, offsets, false);
    if (error)
        return error;
    mutex_lock(&mb_lock);
    if (!context->ref_enabled)
        error = -EPERM;
    else if (server && server->mode != MB_SERVICE_REF)
        error = -EPROTOTYPE;
    else if (request.recipient == context->id)
        error = -EDEADLK;
    else {
        list_for_each_entry(entry, &contexts, link)
            if (entry->id == request.recipient)
                recipient = entry;
        error = !recipient ? -ENOENT :
            (!recipient->ref_enabled ? -EPERM : 0);
    }
    if (error)
        goto unlock;
    error = mb_ref_build(context, recipient, &request.parcel, offsets, &buffer);
    if (error)
        goto unlock;
    recipient->region.slots[buffer.buffer.offset / MB_MAX_PAYLOAD].pending = true;
    request.grant = buffer.buffer.token;
    if (copy_to_user(arg, &request, sizeof(request))) {
        mb_region_free(&recipient->region, request.grant, true);
        error = -EFAULT;
    }
unlock:
    mutex_unlock(&mb_lock);
    return error;
}

static long mb_import_ref(struct mb_context *context, void __user *arg)
{
    struct mb_ref_import request;
    unsigned int i;
    int error = -ENOENT;

    if (copy_from_user(&request, arg, sizeof(request)))
        return -EFAULT;
    mutex_lock(&mb_lock);
    if (!context->ref_enabled) {
        error = -EPERM;
        goto unlock;
    }
    for (i = 0; i < MB_MAP_SLOTS; i++) {
        struct mb_slot *slot = &context->region.slots[i];

        if (!request.grant || slot->token != request.grant || !slot->pending)
            continue;
        if (!atomic_read(&context->region.vmas)) {
            error = -ENXIO;
            break;
        }
        memset(&request.buffer, 0, sizeof(request.buffer));
        request.buffer.buffer.token = slot->token;
        request.buffer.buffer.offset = i * MB_MAX_PAYLOAD;
        request.buffer.buffer.size = slot->size;
        request.buffer.offsets_count = slot->offsets_count;
        memcpy(request.buffer.offsets, slot->offsets, sizeof(slot->offsets));
        error = copy_to_user(arg, &request, sizeof(request)) ? -EFAULT : 0;
        if (!error) {
            mutex_lock(&context->region.lock);
            slot->pending = false;
            slot->delivered = true;
            mutex_unlock(&context->region.lock);
        }
        break;
    }
unlock:
    mutex_unlock(&mb_lock);
    return error;
}

static long mb_change_ref(struct mb_context *context, void __user *arg,
                           bool acquire)
{
    struct mb_ref *ref;
    u32 handle;
    int error = 0;

    if (copy_from_user(&handle, arg, sizeof(handle)))
        return -EFAULT;
    mutex_lock(&mb_lock);
    ref = mb_find_ref(context, handle);
    if (!context->ref_enabled)
        error = -EPERM;
    else if (!ref)
        error = -EBADF;
    else if (acquire) {
        if (!ref->node->owner)
            error = -EPIPE;
        else if (ref->user_refs == U32_MAX)
            error = -EOVERFLOW;
        else
            ref->user_refs++;
    } else if (!ref->user_refs)
        error = -EINVAL;
    else {
        ref->user_refs--;
        mb_ref_reap(ref);
    }
    mutex_unlock(&mb_lock);
    return error;
}

static long mb_ref_stats(struct mb_context *context, void __user *arg)
{
    struct mb_ref_stats stats = { 0 };
    struct mb_context *entry;
    struct mb_ref *ref;
    unsigned int i, j;

    mutex_lock(&mb_lock);
    list_for_each_entry(entry, &contexts, link) {
        list_for_each_entry(ref, &entry->refs, link) {
            stats.global_ref_entries++;
            stats.global_user_refs += ref->user_refs;
            stats.global_buffer_refs += ref->buffer_refs;
            if (entry == context) {
                stats.current_ref_entries++;
                stats.current_user_refs += ref->user_refs;
                stats.current_buffer_refs += ref->buffer_refs;
            }
        }
        for (i = 0; i < MB_MAP_SLOTS; i++) {
            struct mb_slot *slot = &entry->region.slots[i];

            stats.global_pending_slots += slot->pending;
            if (entry == context)
                stats.current_pending_slots += slot->pending;
            for (j = 0; j < slot->holds_count; j++) {
                if (slot->holds[j].ref)
                    continue;
                stats.global_local_objects++;
                if (entry == context)
                    stats.current_local_objects++;
            }
        }
    }
    mutex_unlock(&mb_lock);
    return copy_to_user(arg, &stats, sizeof(stats)) ? -EFAULT : 0;
}

static long mb_call(struct mb_context *context, void __user *arg,
                    enum mb_call_mode mode)
{
    struct mb_transaction *transaction;
    struct mb_parcel_call parcel_call = { 0 };
    struct mb_transaction_call transaction_call;
    struct mb_node_call node_call = { 0 };
    struct mb_node *node = NULL;
    struct mb_buffer_call buffer_call;
    struct mb_call fixed_call;
    u32 offsets[MB_MAX_OBJECTS];
    bool buffered = mode != MB_CALL_FIXED;
    bool node_mode = mode == MB_CALL_NODE;
    bool mapped_reply = mode == MB_CALL_TRANSACTION || node_mode;
    bool active = false;
    enum mb_service_mode service_mode = node_mode ? MB_SERVICE_NODE :
        (mapped_reply ? MB_SERVICE_TRANSACTION :
         (buffered ? MB_SERVICE_BUFFER : MB_SERVICE_FIXED));
    void *target;
    int error;

    transaction = kzalloc(sizeof(*transaction), GFP_KERNEL);
    if (!transaction)
        return -ENOMEM;
    transaction->buffered = buffered;
    if (mapped_reply) {
        if (node_mode) {
            if (copy_from_user(&node_call, arg, sizeof(node_call))) {
                error = -EFAULT;
                goto free;
            }
            if (!node_call.target || node_call.reserved) {
                error = -EINVAL;
                goto free;
            }
            transaction->transaction_call = node_call.call;
        } else if (copy_from_user(&transaction->transaction_call, arg,
                                  sizeof(transaction->transaction_call))) {
            error = -EFAULT;
            goto free;
        }
        if (transaction->transaction_call.reserved) {
            error = -EINVAL;
            goto free;
        }
        transaction->buffer_call.data = transaction->transaction_call.data;
        transaction->buffer_call.size = transaction->transaction_call.size;
        transaction->transaction_call.token = 0;
        transaction->transaction_call.offset = 0;
        transaction->transaction_call.reply_size = 0;
        transaction->transaction_call.status = 0;
    } else if (mode == MB_CALL_PARCEL) {
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
    if (node_mode)
        node = mb_find_node(node_call.target);
    if (context->ref_enabled)
        error = -EPROTOTYPE;
    else if (node_mode && server && server->mode != MB_SERVICE_NODE)
        error = -EPROTOTYPE;
    else if (node_mode && (!node || !node->owner))
        error = -ENOENT;
    else if (!server)
        error = -ENOTCONN;
    else if (server == context)
        error = -EDEADLK;
    else if (server->mode != service_mode)
        error = -EPROTOTYPE;
    else if (context->exclusive_call || (!mapped_reply && context->active_calls))
        error = -EBUSY;
    else if (mapped_reply && !atomic_read(&context->region.vmas))
        error = -ENXIO;
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
    if (node_mode) {
        unsigned int slot = transaction->buffer_request.offset / MB_MAX_PAYLOAD;

        transaction->node = node;
        transaction->code = node_call.code;
        node->transaction_refs++;
        mutex_lock(&server->region.lock);
        server->region.slots[slot].node = node;
        node->slot_refs++;
        mutex_unlock(&server->region.lock);
    }
    context->active_calls++;
    context->exclusive_call = !mapped_reply;
    active = true;
    transaction->caller = context;
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
                    READ_ONCE(transaction->state) >= MB_TX_REPLIED);
    mutex_lock(&mb_lock);
    if (transaction->state >= MB_TX_REPLIED)
        error = transaction->error;
    else {
        /* Restarting the ioctl would submit a new business request. */
        if (error == -ERESTARTSYS)
            error = -EINTR;
        mb_finish(transaction, error);
    }
    list_del(&transaction->link);
    live_requests--;
    fixed_call = transaction->call;
    buffer_call = transaction->buffer_call;
    transaction_call = transaction->transaction_call;
    if (transaction->node) {
        transaction->node->transaction_refs--;
        mb_node_reap(transaction->node);
    }
    mutex_unlock(&mb_lock);
    /* After removal only this CALL owns the record; output owns local tokens. */
    kfree(transaction);
    transaction = NULL;
    if (!error) {
        if (mapped_reply) {
            unsigned long missing;

            if (node_mode) {
                node_call.call = transaction_call;
                missing = copy_to_user(arg, &node_call, sizeof(node_call));
            } else {
                missing = copy_to_user(arg, &transaction_call,
                                       sizeof(transaction_call));
            }
            if (missing) {
                if (transaction_call.token) {
                    mutex_lock(&mb_lock);
                    mb_region_free(&context->region, transaction_call.token, true);
                    mutex_unlock(&mb_lock);
                }
                error = -EFAULT;
            } else if (transaction_call.token) {
                unsigned int slot = transaction_call.offset / MB_MAX_PAYLOAD;

                mutex_lock(&context->region.lock);
                context->region.slots[slot].delivered = true;
                mutex_unlock(&context->region.lock);
            }
        } else if (mode == MB_CALL_PARCEL) {
            parcel_call.result = buffer_call.result;
            parcel_call.status = buffer_call.status;
            if (copy_to_user(arg, &parcel_call, sizeof(parcel_call)))
                error = -EFAULT;
        } else if (buffered ? copy_to_user(arg, &buffer_call, sizeof(buffer_call)) :
                       copy_to_user(arg, &fixed_call, sizeof(fixed_call)))
            error = -EFAULT;
    }
free:
    kfree(transaction);
    if (active) {
        mutex_lock(&mb_lock);
        context->active_calls--;
        if (!mapped_reply)
            context->exclusive_call = false;
        mutex_unlock(&mb_lock);
    }
    return error;
}

static long mb_call_ref(struct mb_context *context, void __user *arg)
{
    struct mb_transaction *transaction;
    struct mb_ref_call call;
    struct mb_ref *ref;
    struct mb_node *node;
    struct mb_slot *slot;
    u32 offsets[MB_MAX_OBJECTS];
    int error;

    if (copy_from_user(&call, arg, sizeof(call)))
        return -EFAULT;
    if (call.reserved)
        return -EINVAL;
    error = mb_ref_copy_offsets(&call.request, offsets, false);
    if (error)
        return error;
    transaction = kzalloc(sizeof(*transaction), GFP_KERNEL);
    if (!transaction)
        return -ENOMEM;
    memset(&call.reply, 0, sizeof(call.reply));
    call.status = 0;
    transaction->ref_call = call;
    transaction->buffered = true;
    init_waitqueue_head(&transaction->wait);
    mutex_lock(&mb_lock);
    ref = mb_find_ref(context, call.target);
    if (!context->ref_enabled)
        error = -EPERM;
    else if (server && server->mode != MB_SERVICE_REF)
        error = -EPROTOTYPE;
    else if (!ref)
        error = -EBADF;
    else if (!ref->node->owner)
        error = -EPIPE;
    else if (ref->node->owner == context)
        error = -EDEADLK;
    else if (!server)
        error = -ENOTCONN;
    else if (context->exclusive_call)
        error = -EBUSY;
    else if (!atomic_read(&context->region.vmas))
        error = -ENXIO;
    else if (live_requests >= MB_MAX_REQUESTS)
        error = -EAGAIN;
    else if (next_request == U64_MAX)
        error = -EOVERFLOW;
    else
        error = 0;
    if (error)
        goto unlock_free;
    node = ref->node;
    if (node->transaction_refs == U32_MAX || node->slot_refs == U32_MAX) {
        error = -EOVERFLOW;
        goto unlock_free;
    }
    error = mb_ref_build(context, server, &call.request, offsets,
                           &transaction->ref_request);
    if (error)
        goto unlock_free;
    transaction->caller = context;
    transaction->receiver = server;
    transaction->node = node;
    transaction->code = call.code;
    transaction->request.id = ++next_request;
    transaction->ref_request.buffer.id = transaction->request.id;
    transaction->buffer_request = transaction->ref_request.buffer;
    node->transaction_refs++;
    slot = &server->region.slots[transaction->buffer_request.offset / MB_MAX_PAYLOAD];
    slot->node = node;
    node->slot_refs++;
    context->active_calls++;
    list_add_tail(&transaction->link, &transactions);
    queued++;
    live_requests++;
    mutex_unlock(&mb_lock);
    wake_up_all(&server_wait);

    error = wait_event_interruptible(transaction->wait,
                    READ_ONCE(transaction->state) >= MB_TX_REPLIED);
    mutex_lock(&mb_lock);
    if (transaction->state >= MB_TX_REPLIED)
        error = transaction->error;
    else {
        if (error == -ERESTARTSYS)
            error = -EINTR;
        mb_finish(transaction, error);
    }
    list_del(&transaction->link);
    live_requests--;
    node->transaction_refs--;
    mb_node_reap(node);
    if (!error) {
        struct mb_ref_buffer *reply = &transaction->ref_call.reply;

        if (copy_to_user(arg, &transaction->ref_call, sizeof(call))) {
            if (reply->buffer.token)
                mb_region_free(&context->region, reply->buffer.token, true);
            error = -EFAULT;
        } else if (reply->buffer.token) {
            mutex_lock(&context->region.lock);
            context->region.slots[reply->buffer.offset / MB_MAX_PAYLOAD].delivered = true;
            mutex_unlock(&context->region.lock);
        }
    }
    context->active_calls--;
unlock_free:
    mutex_unlock(&mb_lock);
    kfree(transaction);
    return error;
}

static long mb_receive(struct mb_context *context, void __user *arg,
                       enum mb_service_mode mode)
{
    struct mb_transaction *transaction;
    bool buffered = mode != MB_SERVICE_FIXED;
    int error;

    for (;;) {
        mutex_lock(&mb_lock);
        if (server != context) {
            mutex_unlock(&mb_lock);
            return -EPERM;
        }
        if (context->mode != mode) {
            mutex_unlock(&mb_lock);
            return -EPROTOTYPE;
        }
        list_for_each_entry(transaction, &transactions, link) {
            if (transaction->state != MB_TX_QUEUED)
                continue;
            if (mode == MB_SERVICE_REF) {
                struct mb_ref_request request = {
                    .buffer = transaction->ref_request,
                    .node = transaction->node->id,
                    .cookie = transaction->node->cookie,
                    .code = transaction->code,
                };

                error = copy_to_user(arg, &request, sizeof(request)) ? -EFAULT : 0;
            } else if (mode == MB_SERVICE_NODE) {
                struct mb_node_request request = {
                    .buffer = transaction->buffer_request,
                    .node = transaction->node->id,
                    .cookie = transaction->node->cookie,
                    .code = transaction->code,
                };

                error = copy_to_user(arg, &request, sizeof(request)) ? -EFAULT : 0;
            } else {
                error = (buffered ? copy_to_user(arg, &transaction->buffer_request,
                                            sizeof(transaction->buffer_request)) :
                               copy_to_user(arg, &transaction->request,
                                            sizeof(transaction->request))) ?
                        -EFAULT : 0;
            }
            if (error) {
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
            transaction->state = MB_TX_DELIVERED;
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
    if (context->mode == MB_SERVICE_TRANSACTION || context->mode == MB_SERVICE_NODE ||
        context->mode == MB_SERVICE_REF) {
        error = -EPROTOTYPE;
        goto unlock;
    }
    list_for_each_entry(transaction, &transactions, link) {
        if (transaction->request.id != reply.id ||
            transaction->state != MB_TX_DELIVERED)
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

static long mb_reply_transaction(struct mb_context *context, void __user *arg,
                                 enum mb_service_mode mode)
{
    struct mb_transaction_reply reply;
    struct mb_buffer_request response = { 0 };
    struct mb_transaction *transaction;
    void *target;
    int error = -ENOENT;

    if (copy_from_user(&reply, arg, sizeof(reply)))
        return -EFAULT;
    if (reply.size > MB_MAX_PAYLOAD || (!reply.size && reply.data))
        return -EINVAL;
    mutex_lock(&mb_lock);
    if (context != server) {
        error = -EPERM;
        goto unlock;
    }
    if (context->mode != mode) {
        error = -EPROTOTYPE;
        goto unlock;
    }
    list_for_each_entry(transaction, &transactions, link) {
        if (transaction->request.id != reply.id ||
            transaction->state != MB_TX_DELIVERED)
            continue;
        if (reply.size) {
            struct mb_region *region = &transaction->caller->region;

            error = mb_region_reserve(transaction->caller, &response, &target);
            if (error)
                goto unlock;
            if (copy_from_user(target, u64_to_user_ptr(reply.data), reply.size)) {
                mb_region_free(region, response.token, true);
                error = -EFAULT;
                goto unlock;
            }
            mutex_lock(&region->lock);
            region->payload_copies++;
            region->payload_bytes += reply.size;
            mutex_unlock(&region->lock);
        }
        transaction->transaction_call.token = response.token;
        transaction->transaction_call.offset = response.offset;
        transaction->transaction_call.reply_size = reply.size;
        transaction->transaction_call.status = reply.status;
        mb_finish(transaction, 0);
        error = 0;
        break;
    }
unlock:
    mutex_unlock(&mb_lock);
    return error;
}

static long mb_reply_ref(struct mb_context *context, void __user *arg)
{
    struct mb_ref_reply reply;
    struct mb_transaction *transaction;
    u32 offsets[MB_MAX_OBJECTS];
    int error;

    if (copy_from_user(&reply, arg, sizeof(reply)))
        return -EFAULT;
    if (reply.reserved)
        return -EINVAL;
    error = mb_ref_copy_offsets(&reply.parcel, offsets, true);
    if (error)
        return error;
    mutex_lock(&mb_lock);
    if (server != context) {
        error = -EPERM;
        goto unlock;
    }
    if (context->mode != MB_SERVICE_REF) {
        error = -EPROTOTYPE;
        goto unlock;
    }
    error = -ENOENT;
    list_for_each_entry(transaction, &transactions, link) {
        if (transaction->request.id != reply.id ||
            transaction->state != MB_TX_DELIVERED)
            continue;
        error = mb_ref_build(context, transaction->caller, &reply.parcel,
                               offsets, &transaction->ref_call.reply);
        if (error)
            break;
        transaction->ref_call.status = reply.status;
        mb_finish(transaction, 0);
        break;
    }
unlock:
    mutex_unlock(&mb_lock);
    return error;
}

static long mb_publish_node(struct mb_context *context, void __user *arg)
{
    struct mb_node_publish publish;
    struct mb_node *node;
    unsigned int owned = 0;
    bool created = false;
    int error = 0;

    mutex_lock(&mb_lock);
    if (context != server) {
        error = -EPERM;
        goto unlock;
    }
    if (context->mode != MB_SERVICE_NODE && context->mode != MB_SERVICE_REF) {
        error = -EPROTOTYPE;
        goto unlock;
    }
    if (copy_from_user(&publish, arg, sizeof(publish))) {
        error = -EFAULT;
        goto unlock;
    }
    if (!publish.cookie) {
        error = -EINVAL;
        goto unlock;
    }
    list_for_each_entry(node, &nodes, link) {
        if (node->owner != context)
            continue;
        owned++;
        if (node->cookie == publish.cookie)
            goto output;
    }
    if (owned >= MB_MAX_NODES) {
        error = -EAGAIN;
        goto unlock;
    }
    if (next_node == U64_MAX) {
        error = -EOVERFLOW;
        goto unlock;
    }
    node = kzalloc(sizeof(*node), GFP_KERNEL);
    if (!node) {
        error = -ENOMEM;
        goto unlock;
    }
    node->id = ++next_node;
    node->cookie = publish.cookie;
    node->owner = context;
    list_add_tail(&node->link, &nodes);
    created = true;
output:
    publish.id = node->id;
    if (copy_to_user(arg, &publish, sizeof(publish))) {
        if (created) {
            node->owner = NULL;
            mb_node_reap(node);
        }
        error = -EFAULT;
    }
unlock:
    mutex_unlock(&mb_lock);
    return error;
}

static long mb_remove_node(struct mb_context *context, void __user *arg)
{
    struct mb_node *node;
    u64 id;
    int error;

    mutex_lock(&mb_lock);
    if (context != server) {
        error = -EPERM;
        goto unlock;
    }
    if (context->mode != MB_SERVICE_NODE && context->mode != MB_SERVICE_REF) {
        error = -EPROTOTYPE;
        goto unlock;
    }
    if (copy_from_user(&id, arg, sizeof(id))) {
        error = -EFAULT;
        goto unlock;
    }
    node = mb_find_node(id);
    if (!node || !node->owner)
        error = -ENOENT;
    else if (node->owner != context)
        error = -EPERM;
    else if (node->transaction_refs || node->slot_refs ||
             node->ref_entries || node->local_object_refs)
        error = -EBUSY;
    else {
        node->owner = NULL;
        mb_node_reap(node);
        error = 0;
    }
unlock:
    mutex_unlock(&mb_lock);
    return error;
}

static long mb_node_stats(struct mb_context *context, void __user *arg)
{
    struct mb_node_stats stats = { 0 };
    struct mb_node *node;

    mutex_lock(&mb_lock);
    list_for_each_entry(node, &nodes, link) {
        stats.live_nodes++;
        if (node->owner == context) {
            stats.owned_nodes++;
            stats.transaction_refs += node->transaction_refs;
            stats.slot_refs += node->slot_refs;
        }
    }
    mutex_unlock(&mb_lock);
    return copy_to_user(arg, &stats, sizeof(stats)) ? -EFAULT : 0;
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
        query.features = MB_FEATURE_RPC | MB_FEATURE_MMAP | MB_FEATURE_PARCEL |
                         MB_FEATURE_TRANSACTION | MB_FEATURE_NODE | MB_FEATURE_REF;
        query.open_id = context->id;
        query.live_contexts = atomic64_read(&live_contexts);
        return copy_to_user(user, &query, sizeof(query)) ? -EFAULT : 0;
    case MB_IOC_REGISTER:
    case MB_IOC_REGISTER_BUFFER:
    case MB_IOC_REGISTER_TRANSACTION:
    case MB_IOC_REGISTER_NODE:
    case MB_IOC_REGISTER_REF:
        mutex_lock(&mb_lock);
        error = context->ref_enabled && command != MB_IOC_REGISTER_REF ?
            -EPROTOTYPE : (server || context->active_calls ? -EBUSY : 0);
        if (!error && command == MB_IOC_REGISTER_REF && !context->ref_enabled)
            error = -EPERM;
        if (!error && command != MB_IOC_REGISTER &&
            !atomic_read(&context->region.vmas))
            error = -ENXIO;
        if (!error) {
            context->mode = command == MB_IOC_REGISTER_REF ? MB_SERVICE_REF :
                (command == MB_IOC_REGISTER_NODE ? MB_SERVICE_NODE :
                (command == MB_IOC_REGISTER ? MB_SERVICE_FIXED :
                (command == MB_IOC_REGISTER_BUFFER ? MB_SERVICE_BUFFER :
                 MB_SERVICE_TRANSACTION)));
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
    case MB_IOC_CALL_TRANSACTION:
        return mb_call(context, user, MB_CALL_TRANSACTION);
    case MB_IOC_CALL_NODE:
        return mb_call(context, user, MB_CALL_NODE);
    case MB_IOC_RECEIVE:
        return mb_receive(context, user, MB_SERVICE_FIXED);
    case MB_IOC_RECEIVE_BUFFER:
        return mb_receive(context, user, MB_SERVICE_BUFFER);
    case MB_IOC_RECEIVE_TRANSACTION:
        return mb_receive(context, user, MB_SERVICE_TRANSACTION);
    case MB_IOC_RECEIVE_NODE:
        return mb_receive(context, user, MB_SERVICE_NODE);
    case MB_IOC_REPLY:
        return mb_reply(context, user);
    case MB_IOC_REPLY_TRANSACTION:
        return mb_reply_transaction(context, user, MB_SERVICE_TRANSACTION);
    case MB_IOC_REPLY_NODE:
        return mb_reply_transaction(context, user, MB_SERVICE_NODE);
    case MB_IOC_PUBLISH_NODE:
        return mb_publish_node(context, user);
    case MB_IOC_REMOVE_NODE:
        return mb_remove_node(context, user);
    case MB_IOC_NODE_STATS:
        return mb_node_stats(context, user);
    case MB_IOC_ENABLE_REF:
        return mb_enable_ref(context);
    case MB_IOC_EXPORT_REF:
        return mb_export_ref(context, user);
    case MB_IOC_IMPORT_REF:
        return mb_import_ref(context, user);
    case MB_IOC_ACQUIRE_REF:
        return mb_change_ref(context, user, true);
    case MB_IOC_RELEASE_REF:
        return mb_change_ref(context, user, false);
    case MB_IOC_CALL_REF:
        return mb_call_ref(context, user);
    case MB_IOC_RECEIVE_REF:
        return mb_receive(context, user, MB_SERVICE_REF);
    case MB_IOC_REPLY_REF:
        return mb_reply_ref(context, user);
    case MB_IOC_REF_STATS:
        return mb_ref_stats(context, user);
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
        mutex_lock(&mb_lock);
        error = mb_region_free(&context->region, token, false);
        mutex_unlock(&mb_lock);
        return error;
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
    struct mb_node *node, *next;
    struct mb_ref *ref, *ref_next;
    unsigned int slot;

    mutex_lock(&mb_lock);
    list_del(&context->link);
    if (server == context) {
        server = NULL;
        list_for_each_entry(transaction, &transactions, link) {
            if (transaction->state < MB_TX_REPLIED)
                mb_finish(transaction, -EPIPE);
        }
        wake_up_all(&server_wait);
    }
    list_for_each_entry_safe(node, next, &nodes, link) {
        if (node->owner != context)
            continue;
        node->owner = NULL;
        mb_node_reap(node);
    }
    for (slot = 0; slot < MB_MAP_SLOTS; ++slot)
        if (context->region.slots[slot].token)
            mb_region_free(&context->region,
                           context->region.slots[slot].token, true);
    list_for_each_entry_safe(ref, ref_next, &context->refs, link) {
        ref->user_refs = 0;
        mb_ref_reap(ref);
    }
    mutex_unlock(&mb_lock);
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
    pr_info("mini_binder: nodes_empty=%u\n", list_empty(&nodes));
}

module_init(mb_init);
module_exit(mb_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Incrementally designed teaching IPC device");
