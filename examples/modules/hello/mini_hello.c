// SPDX-License-Identifier: GPL-2.0-only
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

static bool fail_init;
module_param(fail_init, bool, 0444);
MODULE_PARM_DESC(fail_init, "Return an error after allocating and freeing state");
static void *state;

static int __init mini_hello_init(void)
{
    state = kzalloc(64, GFP_KERNEL);
    if (!state)
        return -ENOMEM;
    if (fail_init) {
        kfree(state);
        state = NULL;
        pr_info("mini_hello: init rollback\n");
        return -EINVAL;
    }
    pr_info("mini_hello: init ok\n");
    return 0;
}

static void __exit mini_hello_exit(void)
{
    kfree(state);
    pr_info("mini_hello: exit ok\n");
}

module_init(mini_hello_init);
module_exit(mini_hello_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Teaching module with a controlled initialization rollback");
