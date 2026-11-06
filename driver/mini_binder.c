// SPDX-License-Identifier: GPL-2.0-only
#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "mini_binder.h"

struct mb_context {
    struct pid *owner;
    u64 id;
};

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

static long mb_ioctl(struct file *file, unsigned int command, unsigned long arg)
{
    struct mb_context *context = file->private_data;
    struct mb_query query = { 0 };

    if (task_tgid(current) != context->owner)
        return -EPERM;
    if (command != MB_IOC_QUERY)
        return -ENOTTY;
    query.version = MB_ABI_VERSION;
    query.open_id = context->id;
    query.live_contexts = atomic64_read(&live_contexts);
    if (copy_to_user((void __user *)arg, &query, sizeof(query)))
        return -EFAULT;
    return 0;
}

static int mb_release(struct inode *inode, struct file *file)
{
    struct mb_context *context = file->private_data;

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
    pr_info("mini_binder: device removed live_contexts=%lld\n",
            atomic64_read(&live_contexts));
}

module_init(mb_init);
module_exit(mb_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Incrementally designed teaching IPC device");
