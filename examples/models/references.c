#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONTEXTS 3
#define NODES 3
#define MAX_REFS 32
#define MAX_SLOTS 16
#define MAX_OBJECTS 16

enum object_type { LOCAL = 1, HANDLE = 2 };
enum slot_kind { GRANT, REQUEST, RESPONSE };
enum tx_state { NONE, QUEUED, DELIVERED, REPLIED, FAILED };

struct node {
    unsigned int id, cookie, published, transactions, target_slots, refs, local;
    int owner, allocated, value;
};
struct ref {
    struct node *node;
    unsigned int handle;
    uint32_t user, buffer;
};
struct hold {
    enum object_type type;
    struct node *node;
    struct ref *ref;
};
struct slot {
    unsigned int token, delivered, count;
    enum slot_kind kind;
    struct node *target;
    struct hold holds[MAX_OBJECTS];
};
struct context {
    int alive, mapped;
    struct ref refs[MAX_REFS];
    struct slot slots[MAX_SLOTS];
};
struct object { unsigned int type, value, cookie; };
struct transaction {
    enum tx_state state;
    int live;
    struct node *target;
    unsigned int request, reply;
};
struct model {
    struct node nodes[NODES];
    struct context contexts[CONTEXTS];
    struct transaction tx;
    unsigned int next_token, allocations, frees, xgrant, zgrant, owner_grant;
    unsigned int xhandle, converted, rolled_back, observed_node;
    int observed_value, has_value;
};
struct totals {
    unsigned int nodes, refs, slots, pending, buffers, locals;
    uint64_t users;
};

static const char *const context_names[] = { "A", "B", "C" };
static const char *const state_names[] = { "NONE", "QUEUED", "DELIVERED", "REPLIED", "FAILED" };
static const char *const kind_names[] = { "GRANT", "REQUEST", "REPLY" };
static const char *const operations[] = {
    "send_x", "send_twice", "send_max", "send_bad", "send_bad_twice", "send_fault",
    "send_unknown", "forge_local", "wrong_cookie", "forged_handle", "wrong_import",
    "import_x", "import_fault", "acquire_x", "release_x", "free_x", "use_x", "stale_x",
    "return_owner", "import_owner", "free_owner", "send_z", "import_z", "free_z",
    "call", "deliver", "receive_fault", "cancel", "reap", "free_request",
    "reply", "reply_object", "reply_fault", "return_reply", "call_output_fault", "free_reply",
    "close_a", "close_b", "close_c", "unmap_c", "remap_c"
};

static void require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "reference invariant failed: %s\n", message);
        exit(1);
    }
}

static void collect(struct model *model, struct node *node)
{
    if (node->allocated && !node->published && !node->transactions &&
        !node->target_slots && !node->refs && !node->local) {
        node->allocated = 0;
        ++model->frees;
    }
}

static struct ref *find_ref(struct context *context, unsigned int handle)
{
    for (unsigned int i = 0; i < MAX_REFS; ++i)
        if (context->refs[i].node && context->refs[i].handle == handle)
            return &context->refs[i];
    return NULL;
}

static void drop_ref(struct model *model, struct ref *ref)
{
    if (!ref->user && !ref->buffer) {
        struct node *node = ref->node;

        require(node && node->refs, "Ref entry owns Node");
        --node->refs;
        memset(ref, 0, sizeof(*ref));
        collect(model, node);
    }
}

static struct slot *find_slot(struct model *model, unsigned int context,
                              unsigned int token)
{
    for (unsigned int i = 0; i < MAX_SLOTS; ++i)
        if (token && model->contexts[context].slots[i].token == token)
            return &model->contexts[context].slots[i];
    return NULL;
}

static void destroy_slot(struct model *model, struct slot *slot, int rollback)
{
    while (slot->count) {
        struct hold *hold = &slot->holds[--slot->count];

        if (hold->type == HANDLE) {
            require(hold->ref->node == hold->node && hold->ref->buffer,
                    "slot owns precise buffer unit");
            --hold->ref->buffer;
            drop_ref(model, hold->ref);
        } else {
            require(hold->type == LOCAL && hold->node->local, "slot owns LOCAL unit");
            --hold->node->local;
            collect(model, hold->node);
        }
        if (rollback)
            ++model->rolled_back;
    }
    if (slot->target) {
        require(slot->target->target_slots != 0, "slot owns target unit");
        --slot->target->target_slots;
        collect(model, slot->target);
    }
    memset(slot, 0, sizeof(*slot));
}

static const char *reserve_slot(struct model *model, unsigned int context,
                                 enum slot_kind kind, struct slot **out)
{
    struct context *recipient = &model->contexts[context];

    if (!recipient->alive)
        return "ENOENT";
    if (!recipient->mapped)
        return "ENXIO";
    for (unsigned int i = 0; i < MAX_SLOTS; ++i) {
        struct slot *slot = &recipient->slots[i];

        if (slot->token)
            continue;
        if (model->next_token == UINT_MAX)
            return "EOVERFLOW";
        *slot = (struct slot){ .token = ++model->next_token, .kind = kind };
        *out = slot;
        return "OK";
    }
    return "EAGAIN";
}

static const char *source_node(struct model *model, unsigned int sender,
                                struct object object, struct node **out)
{
    struct node *node = NULL;

    if (object.type == HANDLE) {
        struct ref *ref = find_ref(&model->contexts[sender], object.value);

        if (!ref)
            return "EBADF";
        require(ref->user || ref->buffer, "live Ref has a unit");
        node = ref->node;
        if (node->owner < 0)
            return "EPIPE";
    } else {
        for (unsigned int i = 0; i < NODES; ++i)
            if (model->nodes[i].allocated && model->nodes[i].id == object.value)
                node = &model->nodes[i];
        if (!node)
            return "ENOENT";
        if (node->owner != (int)sender)
            return "EPERM";
        if (node->cookie != object.cookie)
            return "EINVAL";
    }
    *out = node;
    return "OK";
}

static const char *convert_one(struct model *model, unsigned int sender,
                                unsigned int receiver, struct object object,
                                struct slot *slot)
{
    struct context *context = &model->contexts[receiver];
    struct node *node;
    struct hold *hold = &slot->holds[slot->count];
    const char *status = source_node(model, sender, object, &node);

    if (strcmp(status, "OK"))
        return status;
    if (node->owner == (int)receiver) {
        if (node->local == UINT_MAX)
            return "EOVERFLOW";
        ++node->local;
        *hold = (struct hold){ .type = LOCAL, .node = node };
    } else {
        struct ref *ref = NULL, *available = NULL;

        for (unsigned int i = 0; i < MAX_REFS; ++i) {
            if (context->refs[i].node == node)
                ref = &context->refs[i];
            if (!context->refs[i].node && !available)
                available = &context->refs[i];
        }
        if (!ref) {
            unsigned int handle = 1;

            if (!available)
                return "EAGAIN";
            if (node->refs == UINT_MAX)
                return "EOVERFLOW";
            while (find_ref(context, handle))
                ++handle;
            ref = available;
            *ref = (struct ref){ .node = node, .handle = handle };
            ++node->refs;
        }
        if (ref->buffer == UINT32_MAX)
            return "EOVERFLOW";
        ++ref->buffer;
        *hold = (struct hold){ .type = HANDLE, .node = node, .ref = ref };
    }
    ++slot->count;
    ++model->converted;
    return "OK";
}

static const char *parcel(struct model *model, unsigned int sender,
                           unsigned int receiver, const struct object *objects,
                           unsigned int count, enum slot_kind kind, int fault,
                           unsigned int *token)
{
    struct slot *slot;
    const char *status;

    if (!model->contexts[sender].alive)
        return "EBADF";
    if (count > MAX_OBJECTS)
        return "EINVAL";
    status = reserve_slot(model, receiver, kind, &slot);
    if (strcmp(status, "OK"))
        return status;
    /* Validate the complete logical object table before changing any Ref. */
    for (unsigned int i = 0; i < count; ++i) {
        if (objects[i].type != LOCAL && objects[i].type != HANDLE) {
            status = "EOPNOTSUPP";
            goto rollback;
        }
        if ((objects[i].type == HANDLE && objects[i].cookie) ||
            (objects[i].type == LOCAL && (!objects[i].value || !objects[i].cookie))) {
            status = "EINVAL";
            goto rollback;
        }
    }
    for (unsigned int i = 0; i < count; ++i) {
        status = convert_one(model, sender, receiver, objects[i], slot);
        if (strcmp(status, "OK"))
            goto rollback;
    }
    if (fault) {
        status = "EFAULT";
        goto rollback;
    }
    *token = slot->token;
    return "OK";
rollback:
    destroy_slot(model, slot, 1);
    return status;
}

static const char *import_grant(struct model *model, unsigned int context,
                                 unsigned int token, int fault)
{
    struct slot *slot = find_slot(model, context, token);

    if (!slot || slot->kind != GRANT || slot->delivered)
        return "ENOENT";
    if (!model->contexts[context].mapped)
        return "ENXIO";
    if (fault)
        return "EFAULT";
    slot->delivered = 1;
    return "OK";
}

static const char *free_buffer(struct model *model, unsigned int context,
                                unsigned int token)
{
    struct slot *slot = find_slot(model, context, token);

    if (!slot)
        return "ENOENT";
    if (!slot->delivered)
        return "EBUSY";
    destroy_slot(model, slot, 0);
    return "OK";
}

static const char *user_ref(struct model *model, unsigned int context,
                             unsigned int handle, int acquire)
{
    struct ref *ref = find_ref(&model->contexts[context], handle);

    if (!ref)
        return "EBADF";
    if (acquire) {
        if (ref->node->owner < 0)
            return "EPIPE";
        if (ref->user == UINT32_MAX)
            return "EOVERFLOW";
        ++ref->user;
    } else {
        if (!ref->user)
            return "EINVAL";
        --ref->user;
        drop_ref(model, ref);
    }
    return "OK";
}

static const char *dispatch(struct model *model, unsigned int context,
                             unsigned int handle, unsigned int amount)
{
    struct ref *ref = find_ref(&model->contexts[context], handle);

    if (!ref)
        return "EBADF";
    model->observed_node = ref->node->id;
    if (ref->node->owner < 0)
        return "EPIPE";
    if (amount > (unsigned int)(INT_MAX - ref->node->value))
        return "EOVERFLOW";
    ref->node->value += (int)amount;
    model->observed_value = ref->node->value;
    model->has_value = 1;
    return "OK";
}

static int terminal(const struct transaction *tx)
{
    return tx->state == REPLIED || tx->state == FAILED;
}

static void finish(struct model *model, enum tx_state state)
{
    struct transaction *tx = &model->tx;

    require(tx->live && !terminal(tx), "transaction terminates once");
    if (tx->state == QUEUED) {
        struct slot *request = find_slot(model, 0, tx->request);

        require(request != NULL, "queued transaction owns request until cancellation");
        destroy_slot(model, request, 0);
    }
    tx->state = state;
}

static void reap(struct model *model)
{
    struct transaction *tx = &model->tx;

    require(tx->live && terminal(tx) && tx->target->transactions, "reap owns target");
    --tx->target->transactions;
    collect(model, tx->target);
    tx->live = 0;
}

static const char *begin_call(struct model *model)
{
    struct ref *ref = find_ref(&model->contexts[2], model->xhandle);
    struct object object = { HANDLE, model->xhandle, 0 };
    unsigned int token;
    struct slot *slot;
    const char *status;

    if (!ref)
        return "EBADF";
    model->observed_node = ref->node->id;
    if (ref->node->owner < 0)
        return "EPIPE";
    slot = find_slot(model, 2, model->tx.reply);
    if (model->tx.live || (slot && !slot->delivered))
        return "EBUSY";
    if (!model->contexts[2].mapped)
        return "ENXIO";
    status = parcel(model, 2, 0, &object, 1, REQUEST, 0, &token);
    if (strcmp(status, "OK"))
        return status;
    slot = find_slot(model, 0, token);
    slot->target = ref->node;
    ++ref->node->target_slots;
    ++ref->node->transactions;
    model->tx = (struct transaction){ .state = QUEUED, .live = 1,
        .target = ref->node, .request = token };
    return "OK";
}

static const char *reply(struct model *model, int with_object, int fault)
{
    struct transaction *tx = &model->tx;
    struct object object = { LOCAL, 102, 22 };
    const char *status;

    if (!tx->live || tx->state != DELIVERED)
        return "ENOENT";
    if (tx->target->value == INT_MAX)
        return "EOVERFLOW";
    if (with_object) {
        status = parcel(model, 0, 2, &object, 1, RESPONSE, fault, &tx->reply);
        if (strcmp(status, "OK"))
            return status;
    }
    model->observed_node = tx->target->id;
    model->observed_value = ++tx->target->value;
    model->has_value = 1;
    finish(model, REPLIED);
    return "OK";
}

static const char *close_context(struct model *model, unsigned int index)
{
    struct context *context = &model->contexts[index];

    if (!context->alive)
        return "ENOENT";
    if ((index == 0 || index == 2) && model->tx.live && !terminal(&model->tx))
        finish(model, FAILED);
    /* close_c represents process exit: cancellation/reap precede final close. */
    if (index == 2 && model->tx.live)
        reap(model);
    context->alive = 0;
    context->mapped = 0;
    for (unsigned int i = 0; i < NODES; ++i) {
        struct node *node = &model->nodes[i];

        if (node->owner == (int)index) {
            node->owner = -1;
            node->published = 0;
            collect(model, node);
        }
    }
    for (unsigned int i = 0; i < MAX_SLOTS; ++i)
        if (context->slots[i].token)
            destroy_slot(model, &context->slots[i], 0);
    for (unsigned int i = 0; i < MAX_REFS; ++i) {
        struct ref *ref = &context->refs[i];

        if (ref->node) {
            require(!ref->buffer, "final slots freed before refs");
            ref->user = 0;
            drop_ref(model, ref);
        }
    }
    return "OK";
}

static void initialize(struct model *model, int log_bootstrap)
{
    const unsigned int owners[] = { 1, 1, 2 };
    const unsigned int nodes[] = { 101, 102, 102 };
    const unsigned int cookies[] = { 11, 22, 22 };

    memset(model, 0, sizeof(*model));
    for (unsigned int i = 0; i < CONTEXTS; ++i)
        model->contexts[i].alive = model->contexts[i].mapped = 1;
    for (unsigned int i = 0; i < NODES; ++i)
        model->nodes[i] = (struct node){ .id = 101 + i, .cookie = 11 * (i + 1),
            .published = 1, .owner = 0, .allocated = 1,
            .value = i == 2 ? 1000 : (i == 1 ? 100 : 0) };
    model->allocations = NODES;
    model->xhandle = 2;
    for (unsigned int i = 0; i < 3; ++i) {
        struct object object = { LOCAL, nodes[i], cookies[i] };
        unsigned int token, handle;
        struct slot *slot;

        require(!strcmp(parcel(model, 0, owners[i], &object, 1, GRANT, 0, &token), "OK"),
                "bootstrap export");
        slot = find_slot(model, owners[i], token);
        handle = slot->holds[0].ref->handle;
        require(!strcmp(import_grant(model, owners[i], token, 0), "OK"), "bootstrap import");
        require(!strcmp(user_ref(model, owners[i], handle, 1), "OK"), "bootstrap acquire");
        require(!strcmp(free_buffer(model, owners[i], token), "OK"), "bootstrap free");
        if (log_bootstrap)
            printf("event=bootstrap source=LOCAL context=%s node=%u handle=%u\n",
                   context_names[owners[i]], nodes[i], handle);
    }
    model->converted = 0;
}

static struct totals totals(const struct model *model)
{
    struct totals result = { 0 };

    for (unsigned int i = 0; i < NODES; ++i) {
        result.nodes += (unsigned int)model->nodes[i].allocated;
        result.locals += model->nodes[i].local;
    }
    for (unsigned int i = 0; i < CONTEXTS; ++i) {
        const struct context *context = &model->contexts[i];

        for (unsigned int j = 0; j < MAX_REFS; ++j)
            if (context->refs[j].node) {
                ++result.refs;
                result.users += context->refs[j].user;
                result.buffers += context->refs[j].buffer;
            }
        for (unsigned int j = 0; j < MAX_SLOTS; ++j)
            if (context->slots[j].token) {
                ++result.slots;
                result.pending += context->slots[j].kind == GRANT &&
                                  !context->slots[j].delivered;
            }
    }
    return result;
}

static void snapshot(const struct model *model, unsigned int step,
                     const char *operation, const char *status)
{
    struct totals count = totals(model);

    printf("event=step step=%u operation=%s status=%s nodes=%u refs=%u users=%llu "
           "buffers=%u locals=%u slots=%u pending=%u allocs=%u frees=%u "
           "state=%s node=%u value=", step, operation, status, count.nodes,
           count.refs, (unsigned long long)count.users, count.buffers, count.locals,
           count.slots, count.pending, model->allocations, model->frees,
           state_names[model->tx.state], model->observed_node);
    if (model->has_value)
        printf("%d", model->observed_value);
    else
        printf("none");
    printf(" converted=%u rolled_back=%u\n", model->converted, model->rolled_back);
    for (unsigned int i = 0; i < NODES; ++i) {
        const struct node *node = &model->nodes[i];

        printf("event=node id=%u cookie=%u owner=%s allocated=%d published=%u "
               "transactions=%u target_slots=%u refs=%u local=%u\n", node->id,
               node->cookie, node->owner < 0 ? "none" : context_names[node->owner],
               node->allocated, node->published, node->transactions,
               node->target_slots, node->refs, node->local);
    }
    for (unsigned int i = 0; i < CONTEXTS; ++i) {
        const struct context *context = &model->contexts[i];

        for (unsigned int j = 0; j < MAX_REFS; ++j) {
            const struct ref *ref = &context->refs[j];

            if (ref->node)
                printf("event=ref context=%s handle=%u node=%u user=%u buffer=%u\n",
                       context_names[i], ref->handle, ref->node->id,
                       (unsigned int)ref->user, (unsigned int)ref->buffer);
        }
        for (unsigned int j = 0; j < MAX_SLOTS; ++j) {
            const struct slot *slot = &context->slots[j];

            if (!slot->token)
                continue;
            printf("event=slot context=%s token=%u kind=%s delivered=%u objects=%u target=%u\n",
                   context_names[i], slot->token, kind_names[slot->kind], slot->delivered,
                   slot->count, slot->target ? slot->target->id : 0);
            for (unsigned int k = 0; k < slot->count; ++k) {
                const struct hold *hold = &slot->holds[k];

                printf("event=hold context=%s token=%u index=%u type=%s node=%u handle=%u cookie=%u\n",
                       context_names[i], slot->token, k, hold->type == LOCAL ? "LOCAL" : "HANDLE",
                       hold->node->id, hold->ref ? hold->ref->handle : 0,
                       hold->type == LOCAL ? hold->node->cookie : 0);
            }
        }
    }
    if (model->tx.live)
        printf("event=tx node=%u state=%s\n", model->tx.target->id, state_names[model->tx.state]);
}

static const char *step(struct model *model, const char *operation)
{
    struct object objects[MAX_OBJECTS];
    unsigned int count = 1, token;
    const char *status;

    model->converted = model->rolled_back = model->observed_node = 0;
    model->has_value = 0;
    if (!strncmp(operation, "send_", 5)) {
        for (unsigned int i = 0; i < MAX_OBJECTS; ++i)
            objects[i] = (struct object){ HANDLE, 1, 0 };
        if (!strcmp(operation, "send_twice"))
            count = 2;
        if (!strcmp(operation, "send_max"))
            count = MAX_OBJECTS;
        if (!strcmp(operation, "send_bad") || !strcmp(operation, "send_unknown")) {
            count = 2;
            objects[1] = (struct object){ HANDLE, 99, 0 };
        }
        if (!strcmp(operation, "send_bad_twice")) {
            count = 3;
            objects[2] = (struct object){ HANDLE, 99, 0 };
        }
        if (!strcmp(operation, "send_unknown"))
            objects[1].type = 99;
        if (!strcmp(operation, "send_z")) {
            objects[0] = (struct object){ LOCAL, 103, 33 };
            return parcel(model, 0, 2, objects, 1, GRANT, 0, &model->zgrant);
        }
        status = parcel(model, 1, 2, objects, count, GRANT,
                        !strcmp(operation, "send_fault"), &token);
        if (!strcmp(status, "OK")) {
            struct slot *slot = find_slot(model, 2, token);

            model->xgrant = token;
            model->xhandle = slot->holds[0].ref->handle;
        }
        return status;
    }
    if (!strcmp(operation, "forge_local") || !strcmp(operation, "wrong_cookie")) {
        struct object object = { LOCAL, 101, !strcmp(operation, "wrong_cookie") ? 99 : 11 };

        return parcel(model, !strcmp(operation, "wrong_cookie") ? 0 : 1,
                      2, &object, 1, GRANT, 0, &token);
    }
    if (!strcmp(operation, "forged_handle"))
        return dispatch(model, 2, 99, 1);
    if (!strcmp(operation, "wrong_import"))
        return import_grant(model, 1, model->xgrant, 0);
    if (!strcmp(operation, "import_x") || !strcmp(operation, "import_fault"))
        return import_grant(model, 2, model->xgrant, !strcmp(operation, "import_fault"));
    if (!strcmp(operation, "acquire_x") || !strcmp(operation, "release_x"))
        return user_ref(model, 2, model->xhandle, !strcmp(operation, "acquire_x"));
    if (!strcmp(operation, "free_x"))
        return free_buffer(model, 2, model->xgrant);
    if (!strcmp(operation, "use_x") || !strcmp(operation, "stale_x"))
        return dispatch(model, 2, model->xhandle, 1);
    if (!strcmp(operation, "return_owner")) {
        struct object object = { HANDLE, 1, 0 };

        return parcel(model, 1, 0, &object, 1, GRANT, 0, &model->owner_grant);
    }
    if (!strcmp(operation, "import_owner"))
        return import_grant(model, 0, model->owner_grant, 0);
    if (!strcmp(operation, "free_owner"))
        return free_buffer(model, 0, model->owner_grant);
    if (!strcmp(operation, "import_z"))
        return import_grant(model, 2, model->zgrant, 0);
    if (!strcmp(operation, "free_z"))
        return free_buffer(model, 2, model->zgrant);
    if (!strcmp(operation, "call"))
        return begin_call(model);
    if (!strcmp(operation, "deliver") || !strcmp(operation, "receive_fault")) {
        struct slot *slot;

        if (!model->tx.live || model->tx.state != QUEUED)
            return "ENOENT";
        if (!strcmp(operation, "receive_fault"))
            return "EFAULT";
        slot = find_slot(model, 0, model->tx.request);
        require(slot != NULL, "queued transaction owns request");
        slot->delivered = 1;
        model->tx.state = DELIVERED;
        return "OK";
    }
    if (!strcmp(operation, "cancel")) {
        if (!model->tx.live || terminal(&model->tx))
            return "ENOENT";
        finish(model, FAILED);
        return "OK";
    }
    if (!strcmp(operation, "reap")) {
        if (!model->tx.live || !terminal(&model->tx))
            return "ENOENT";
        reap(model);
        return "OK";
    }
    if (!strcmp(operation, "free_request"))
        return free_buffer(model, 0, model->tx.request);
    if (!strcmp(operation, "reply") || !strcmp(operation, "reply_object") ||
        !strcmp(operation, "reply_fault"))
        return reply(model, strcmp(operation, "reply") != 0,
                     !strcmp(operation, "reply_fault"));
    if (!strcmp(operation, "return_reply") || !strcmp(operation, "call_output_fault")) {
        struct slot *slot = find_slot(model, 2, model->tx.reply);

        if (model->tx.state != REPLIED || !slot || slot->delivered)
            return "ENOENT";
        if (!strcmp(operation, "call_output_fault")) {
            destroy_slot(model, slot, 1);
            return "EFAULT";
        }
        slot->delivered = 1;
        return "OK";
    }
    if (!strcmp(operation, "free_reply"))
        return free_buffer(model, 2, model->tx.reply);
    if (!strcmp(operation, "close_a"))
        return close_context(model, 0);
    if (!strcmp(operation, "close_b"))
        return close_context(model, 1);
    if (!strcmp(operation, "close_c"))
        return close_context(model, 2);
    if (!strcmp(operation, "unmap_c")) {
        if (!model->contexts[2].alive || !model->contexts[2].mapped)
            return "ENOENT";
        model->contexts[2].mapped = 0;
        return "OK";
    }
    if (!strcmp(operation, "remap_c"))
        return model->contexts[2].alive ? "EBUSY" : "EBADF";
    require(0, "validated operation has implementation");
    return "EINVAL";
}

static void result(struct model *model, unsigned int context,
                   unsigned int handle, unsigned int amount)
{
    const char *status;

    model->has_value = 0;
    model->observed_node = 0;
    status = dispatch(model, context, handle, amount);
    printf("event=result context=%s handle=%u status=%s node=%u value=",
           context_names[context], handle, status, model->observed_node);
    if (model->has_value)
        printf("%d\n", model->observed_value);
    else
        printf("none\n");
}

static int usage(void)
{
    fprintf(stderr, "usage: references raw|transfer|trace EVENT...\n");
    return 2;
}

int main(int argc, char **argv)
{
    struct model model;

    if (argc < 2)
        return usage();
    if (!strcmp(argv[1], "trace")) {
        if (argc < 3)
            return usage();
        for (int i = 2; i < argc; ++i) {
            unsigned int j;

            for (j = 0; j < sizeof(operations) / sizeof(operations[0]); ++j)
                if (!strcmp(argv[i], operations[j]))
                    break;
            if (j == sizeof(operations) / sizeof(operations[0]))
                return usage();
        }
        initialize(&model, 0);
        snapshot(&model, 0, "init", "OK");
        for (int i = 2; i < argc; ++i) {
            const char *status = step(&model, argv[i]);

            snapshot(&model, (unsigned int)(i - 1), argv[i], status);
        }
        return 0;
    }
    if (argc != 2 || (strcmp(argv[1], "raw") && strcmp(argv[1], "transfer")))
        return usage();
    initialize(&model, 1);
    if (!strcmp(argv[1], "raw")) {
        result(&model, 2, 1, 1);
        result(&model, 2, 2, 1);
    } else {
        const char *const transfer[] = { "send_x", "import_x", "acquire_x", "free_x" };

        for (unsigned int i = 0; i < sizeof(transfer) / sizeof(transfer[0]); ++i) {
            const char *status = step(&model, transfer[i]);

            require(!strcmp(status, "OK"), "transfer demonstration");
            snapshot(&model, i + 1, transfer[i], status);
        }
        result(&model, 2, 2, 1);
        result(&model, 1, 1, 2);
        result(&model, 2, 1, 1);
    }
    for (unsigned int i = 0; i < CONTEXTS; ++i)
        require(!strcmp(close_context(&model, i), "OK"), "demonstration final close");
    {
        struct totals count = totals(&model);

        printf("event=summary nodes=%u refs=%u slots=%u pending=%u\n",
               count.nodes, count.refs, count.slots, count.pending);
    }
    return 0;
}
