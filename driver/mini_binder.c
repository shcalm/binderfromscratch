// SPDX-License-Identifier: GPL-2.0-only
#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include "mini_binder.h"

struct mb_context {
    struct pid *owner;
    u64 id;
    bool busy;
};

struct mb_transaction {
    struct list_head link;
    wait_queue_head_t wait;
    struct mb_request request;
    struct mb_call call;
    bool delivered;
    bool done;
    int error;
};

static DEFINE_MUTEX(mb_lock);
static LIST_HEAD(transactions);
static DECLARE_WAIT_QUEUE_HEAD(server_wait);
static struct mb_context *server;
static u64 next_request;
static unsigned int queued, delivered, live_requests;

static atomic64_t next_id = ATOMIC64_INIT(0);
static atomic64_t live_contexts = ATOMIC64_INIT(0);

static int mb_open(struct inode *inode, struct file *file)
{
    struct mb_context *context;

    context = kzalloc(sizeof(*context), GFP_KERNEL);
    if (!context)
        return -ENOMEM;
    context->owner = get_pid(task_tgid(current));
    context->id = atomic64_inc_return(&next_id);
    file->private_data = context;
    atomic64_inc(&live_contexts);
    return 0;
}

static void mb_finish(struct mb_transaction *transaction, int error)
{
    if (transaction->delivered)
        delivered--;
    else
        queued--;
    transaction->error = error;
    WRITE_ONCE(transaction->done, true);
    wake_up_all(&transaction->wait);
}

static long mb_call(struct mb_context *context, void __user *arg)
{
    struct mb_transaction *transaction;
    int error;

    transaction = kzalloc(sizeof(*transaction), GFP_KERNEL);
    if (!transaction)
        return -ENOMEM;
    if (copy_from_user(&transaction->call, arg, sizeof(transaction->call))) {
        error = -EFAULT;
        goto free;
    }
    transaction->call.result = 0;
    transaction->call.status = 0;
    init_waitqueue_head(&transaction->wait);
    mutex_lock(&mb_lock);
    if (!server)
        error = -ENOTCONN;
    else if (server == context)
        error = -EDEADLK;
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
    context->busy = true;
    transaction->request.id = ++next_request;
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
    if (!error && copy_to_user(arg, &transaction->call, sizeof(transaction->call)))
        error = -EFAULT;
free:
    kfree(transaction);
    return error;
}

static long mb_receive(struct mb_context *context, void __user *arg)
{
    struct mb_transaction *transaction;
    int error;

    for (;;) {
        mutex_lock(&mb_lock);
        if (server != context) {
            mutex_unlock(&mb_lock);
            return -EPERM;
        }
        list_for_each_entry(transaction, &transactions, link) {
            if (transaction->done || transaction->delivered)
                continue;
            if (copy_to_user(arg, &transaction->request,
                             sizeof(transaction->request))) {
                mutex_unlock(&mb_lock);
                return -EFAULT;
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
    void __user *user = (void __user *)arg;
    int error;

    if (task_tgid(current) != context->owner)
        return -EPERM;
    switch (command) {
    case MB_IOC_QUERY:
        query.version = MB_ABI_VERSION;
        query.features = MB_FEATURE_RPC;
        query.open_id = context->id;
        query.live_contexts = atomic64_read(&live_contexts);
        return copy_to_user(user, &query, sizeof(query)) ? -EFAULT : 0;
    case MB_IOC_REGISTER:
        mutex_lock(&mb_lock);
        error = server || context->busy ? -EBUSY : 0;
        if (!error)
            server = context;
        mutex_unlock(&mb_lock);
        return error;
    case MB_IOC_CALL:
        return mb_call(context, user);
    case MB_IOC_RECEIVE:
        return mb_receive(context, user);
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
    default:
        return -ENOTTY;
    }
}

static int mb_release(struct inode *inode, struct file *file)
{
    struct mb_context *context = file->private_data;
    struct mb_transaction *transaction;

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

    if (!error)
        pr_info("mini_binder: device registered ABI=%u\n", MB_ABI_VERSION);
    return error;
}

static void __exit mb_exit(void)
{
    misc_deregister(&mb_device);
    pr_info("mini_binder: device removed live_contexts=%lld requests=%u\n",
            atomic64_read(&live_contexts), live_requests);
}

module_init(mb_init);
module_exit(mb_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Incrementally designed teaching IPC device");
