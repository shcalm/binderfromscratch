#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NODES 16

struct node {
    unsigned int id, cookie, owner, published, tx_refs, slot_refs;
    int allocated;
};

struct registry {
    struct node nodes[MAX_NODES];
    unsigned int next_id, allocations, frees;
};

enum kind { COUNTER, ADDER };
struct local_object {
    unsigned int node, cookie, active;
    enum kind kind;
    int value, alive;
};

enum state { NONE, QUEUED, DELIVERED, REPLIED, FAILED };
enum request_owner { NO_REQUEST, DRIVER, SERVER };
enum event {
    PUBLISH, PUBLISH_FAULT, REMOVE, WRONG_REMOVE, CALL, CALL_FAULT,
    CALL_STALE, DELIVER, RECEIVE_FAULT, REPLY, CANCEL, REAP,
    FREE_REQUEST, LOCAL_BEGIN, LOCAL_END, DESTROY, OWNER_EXIT, EVENT_COUNT
};

static const char *const event_names[] = {
    "publish", "publish_fault", "remove", "wrong_remove", "call", "call_fault",
    "call_stale", "deliver", "receive_fault", "reply", "cancel", "reap",
    "free_request", "local_begin", "local_end", "destroy", "owner_exit"
};
static const char *const state_names[] = { "NONE", "QUEUED", "DELIVERED", "REPLIED", "FAILED" };
static const char *const request_names[] = { "NONE", "DRIVER", "SERVER" };

struct trace {
    struct registry registry;
    struct node *node;
    struct local_object object;
    enum state state;
    enum request_owner request;
    unsigned int tx_live, terminal_count, descriptor_node, descriptor_cookie;
    unsigned int tx_acquires, tx_releases, slot_acquires, slot_releases;
    int owner_alive, descriptor_claimed;
};

static unsigned int references(const struct node *node)
{
    return node->published + node->tx_refs + node->slot_refs;
}

static void collect(struct registry *registry, struct node *node)
{
    if (node->allocated && !references(node)) {
        node->allocated = 0;
        ++registry->frees;
    }
}

static struct node *find_id(struct registry *registry, unsigned int id)
{
    for (unsigned int i = 0; i < MAX_NODES; ++i)
        if (registry->nodes[i].allocated && registry->nodes[i].published &&
            registry->nodes[i].id == id)
            return &registry->nodes[i];
    return NULL;
}

static struct node *publish(struct registry *registry, unsigned int owner,
                            unsigned int cookie, int *created)
{
    struct node *available = NULL;

    *created = 0;
    for (unsigned int i = 0; i < MAX_NODES; ++i) {
        struct node *node = &registry->nodes[i];
        if (node->allocated && node->published && node->owner == owner && node->cookie == cookie)
            return node;
        if (!node->allocated && !available)
            available = node;
    }
    if (!cookie || !available)
        return NULL;
    *available = (struct node){ .id = ++registry->next_id, .cookie = cookie,
                               .owner = owner, .published = 1, .allocated = 1 };
    ++registry->allocations;
    *created = 1;
    return available;
}

static const char *remove_node(struct registry *registry, unsigned int owner, unsigned int id)
{
    struct node *node = find_id(registry, id);

    if (!node)
        return "ENOENT";
    if (node->owner != owner)
        return "EPERM";
    if (references(node) != 1)
        return "EBUSY";
    node->published = 0;
    collect(registry, node);
    return "OK";
}

static void require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "node invariant failed: %s\n", message);
        exit(1);
    }
}

static int is_terminal(enum state state)
{
    return state == REPLIED || state == FAILED;
}

static void free_request(struct trace *trace)
{
    require(trace->request != NO_REQUEST && trace->node->slot_refs == 1, "request owner");
    trace->request = NO_REQUEST;
    --trace->node->slot_refs;
    ++trace->slot_releases;
    collect(&trace->registry, trace->node);
}

static void check_trace(const struct trace *trace)
{
    const struct node *node = trace->node;

    require(trace->registry.allocations - trace->registry.frees == (unsigned int)node->allocated,
            "node allocation ledger");
    require((unsigned int)node->allocated == (unsigned int)(references(node) != 0), "node refs");
    require(node->tx_refs == trace->tx_live && trace->tx_live <= 1, "transaction refs");
    require(node->slot_refs == (unsigned int)(trace->request != NO_REQUEST), "slot refs");
    require(trace->tx_acquires - trace->tx_releases == node->tx_refs, "transaction releases");
    require(trace->slot_acquires - trace->slot_releases == node->slot_refs, "slot releases");
    require(trace->terminal_count == (unsigned int)is_terminal(trace->state), "terminal once");
    require(trace->request != DRIVER || trace->state == QUEUED, "queued slot");
    require(trace->request != SERVER || trace->state != QUEUED, "delivered slot");
    require(trace->object.alive || !trace->object.active, "local object lifetime");
    require(trace->owner_alive || (!node->published && !node->slot_refs && !trace->object.alive),
            "owner exit cleanup");
}

static const char *start_call(struct trace *trace, unsigned int target, int fault)
{
    struct node *node = find_id(&trace->registry, target);

    if (!node || !trace->owner_alive)
        return "ENOENT";
    /* A trace observes one request at a time, not the driver's whole queue. */
    if (trace->tx_live || trace->request != NO_REQUEST)
        return "EBUSY";
    ++node->slot_refs;
    ++trace->slot_acquires;
    trace->request = DRIVER;
    if (fault) {
        free_request(trace);
        return "EFAULT";
    }
    ++node->tx_refs;
    ++trace->tx_acquires;
    trace->tx_live = 1;
    trace->state = QUEUED;
    trace->terminal_count = 0;
    return "OK";
}

static void observe(const struct trace *trace, const char *operation, const char *status, int created)
{
    const struct node *node = trace->node;

    printf("event=step operation=%s status=%s accepted=%d created=%d node=%u cookie=%u "
           "published=%u tx_refs=%u slot_refs=%u refs=%u live_nodes=%d "
           "state=%s tx_live=%u request=%s terminal_count=%u "
           "local_active=%u local_alive=%d owner_alive=%d "
           "node_allocs=%u node_frees=%u descriptor_node=%u "
           "tx_acquires=%u tx_releases=%u slot_acquires=%u slot_releases=%u\n",
           operation, status, strcmp(status, "OK") == 0, created, node->id, node->cookie,
           node->published, node->tx_refs, node->slot_refs, references(node), node->allocated,
           state_names[trace->state], trace->tx_live, request_names[trace->request],
           trace->terminal_count, trace->object.active, trace->object.alive, trace->owner_alive,
           trace->registry.allocations, trace->registry.frees, trace->descriptor_node,
           trace->tx_acquires, trace->tx_releases, trace->slot_acquires, trace->slot_releases);
}

static void step(struct trace *trace, enum event event)
{
    const char *status = "OK";
    int created = 0;

    switch (event) {
    case PUBLISH:
    case PUBLISH_FAULT:
        if (!trace->owner_alive) {
            status = "EPIPE";
        } else if (!trace->node->published && trace->object.alive) {
            status = "EBUSY";
        } else {
            trace->node = publish(&trace->registry, 1, 7, &created);
            require(trace->node != NULL, "bounded trace publication");
            if (event == PUBLISH_FAULT) {
                if (created) {
                    trace->node->published = 0;
                    collect(&trace->registry, trace->node);
                }
                status = "EFAULT";
            } else if (created) {
                trace->object = (struct local_object){ .node = trace->node->id,
                    .cookie = 7, .kind = COUNTER, .alive = 1 };
            }
        }
        break;
    case REMOVE:
    case WRONG_REMOVE:
        status = remove_node(&trace->registry, event == REMOVE ? 1 : 2, trace->node->id);
        break;
    case CALL:
    case CALL_STALE:
    case CALL_FAULT:
        status = start_call(trace, event == CALL_STALE ? 101 : trace->node->id, event == CALL_FAULT);
        break;
    case DELIVER:
    case RECEIVE_FAULT:
        if (!trace->tx_live || trace->state != QUEUED) {
            status = "ENOENT";
        } else if (event == RECEIVE_FAULT) {
            status = "EFAULT";
        } else {
            trace->state = DELIVERED;
            trace->request = SERVER;
            trace->descriptor_node = trace->node->id;
            trace->descriptor_cookie = trace->node->cookie;
            trace->descriptor_claimed = 0;
        }
        break;
    case REPLY:
        if (!trace->tx_live || trace->state != DELIVERED) {
            status = "ENOENT";
        } else {
            trace->state = REPLIED;
            ++trace->terminal_count;
        }
        break;
    case CANCEL:
        if (!trace->tx_live || is_terminal(trace->state)) {
            status = "ENOENT";
        } else {
            if (trace->state == QUEUED)
                free_request(trace);
            trace->state = FAILED;
            ++trace->terminal_count;
        }
        break;
    case REAP:
        if (!trace->tx_live || !is_terminal(trace->state)) {
            status = "ENOENT";
        } else {
            trace->tx_live = 0;
            --trace->node->tx_refs;
            ++trace->tx_releases;
            collect(&trace->registry, trace->node);
        }
        break;
    case FREE_REQUEST:
        if (trace->request != SERVER)
            status = trace->request == DRIVER ? "EBUSY" : "ENOENT";
        else
            free_request(trace);
        break;
    case LOCAL_BEGIN:
        if (!trace->descriptor_node || !trace->object.alive ||
            trace->object.node != trace->descriptor_node ||
            trace->object.cookie != trace->descriptor_cookie) {
            status = "ESTALE";
        } else if (trace->descriptor_claimed || trace->request != SERVER) {
            status = "EBUSY";
        } else {
            ++trace->object.active;
            trace->descriptor_claimed = 1;
        }
        break;
    case LOCAL_END:
        if (!trace->object.active)
            status = "ENOENT";
        else
            --trace->object.active;
        break;
    case DESTROY:
        if (!trace->object.alive)
            status = "ENOENT";
        else if (trace->node->published || trace->object.active)
            status = "EBUSY";
        else
            trace->object.alive = 0;
        break;
    case OWNER_EXIT:
        if (!trace->owner_alive) {
            status = "EPIPE";
        } else {
            trace->owner_alive = 0;
            if (trace->tx_live && !is_terminal(trace->state)) {
                trace->state = FAILED;
                ++trace->terminal_count;
            }
            if (trace->request != NO_REQUEST)
                free_request(trace);
            trace->node->published = 0;
            trace->node->owner = 0;
            trace->object.alive = 0;
            trace->object.active = 0;
            collect(&trace->registry, trace->node);
        }
        break;
    case EVENT_COUNT:
        require(0, "invalid event");
        break;
    }
    check_trace(trace);
    observe(trace, event_names[event], status, created);
}

static void run_trace(int count, char **events)
{
    struct trace trace = { .registry.next_id = 100, .owner_alive = 1 };
    int created;

    trace.node = publish(&trace.registry, 1, 7, &created);
    trace.object = (struct local_object){ .node = trace.node->id, .cookie = 7,
                                          .kind = COUNTER, .alive = 1 };
    for (int i = 0; i < count; ++i)
        for (int j = 0; j < EVENT_COUNT; ++j)
            if (strcmp(events[i], event_names[j]) == 0) {
                step(&trace, (enum event)j);
                break;
            }
    printf("event=summary live_nodes=%d published=%u tx_refs=%u slot_refs=%u local_active=%u\n",
           trace.node->allocated, trace.node->published, trace.node->tx_refs,
           trace.node->slot_refs, trace.object.active);
}

static void method_counterexample(void)
{
    const char *const objects[] = { "A", "B", "A" };
    const int expected[] = { 1, 1, 2 };
    int shared_counter = 0;

    for (unsigned int i = 0; i < 3; ++i) {
        ++shared_counter;
        printf("event=method_call object=%s code=1 expected=%d observed=%d\n",
               objects[i], expected[i], shared_counter);
    }
    require(shared_counter != expected[2], "method-only counterexample");
}

static const char *dispatch(struct registry *registry, struct local_object *objects,
                            unsigned int target, unsigned int code, int a, int b, int *value)
{
    struct node *node = find_id(registry, target);

    if (!node)
        return "ENOENT";
    for (unsigned int i = 0; i < 3; ++i) {
        struct local_object *object = &objects[i];
        if (!object->alive || object->node != node->id || object->cookie != node->cookie)
            continue;
        ++object->active;
        if (object->kind == COUNTER && code == 1) {
            object->value += a;
            *value = object->value;
        } else if (object->kind == COUNTER && code == 2) {
            *value = object->value;
        } else if (object->kind == ADDER && code == 3) {
            *value = a + b;
        } else {
            --object->active;
            return "ENOSYS";
        }
        --object->active;
        return "OK";
    }
    return "ESTALE";
}

static void registry_demo(void)
{
    struct registry registry = { .next_id = 100 };
    struct local_object objects[3] = { 0 };
    const unsigned int cookies[] = { 11, 22, 33, 11 };
    const unsigned int targets[] = { 101, 102, 101, 102, 103, 101 };
    const unsigned int codes[] = { 1, 1, 1, 2, 3, 999 };
    const int arguments[] = { 1, 1, 2, 0, 19, 0 };
    const int expected[] = { 1, 101, 3, 101, 42, 0 };

    for (unsigned int i = 0; i < 4; ++i) {
        int created;
        struct node *node = publish(&registry, 1, cookies[i], &created);
        require(node != NULL, "registry publication");
        printf("event=publish owner=1 cookie=%u node=%u created=%d\n", cookies[i], node->id, created);
        if (created)
            objects[i] = (struct local_object){ .node = node->id, .cookie = cookies[i],
                .kind = i == 2 ? ADDER : COUNTER, .value = i == 1 ? 100 : 0, .alive = 1 };
    }
    require(registry.allocations == 3, "publication deduplication");
    for (unsigned int i = 0; i < 6; ++i) {
        int value = 0;
        const char *status = dispatch(&registry, objects, targets[i], codes[i], arguments[i], 23, &value);
        require(strcmp(status, i == 5 ? "ENOSYS" : "OK") == 0, "user dispatch status");
        require(i == 5 || value == expected[i], "user dispatch value");
        printf("event=result node=%u code=%u status=%s value=", targets[i], codes[i], status);
        if (strcmp(status, "OK") == 0)
            printf("%d\n", value);
        else
            puts("none");
    }
    for (unsigned int i = 0; i < 3; ++i) {
        require(strcmp(remove_node(&registry, 1, objects[i].node), "OK") == 0, "registry removal");
        require(!objects[i].active, "local dispatch released");
        objects[i].alive = 0;
    }
    require(registry.allocations == registry.frees, "registry cleanup");
    puts("event=summary live_nodes=0 published=0 local_active=0");
}

static int known_event(const char *name)
{
    for (int i = 0; i < EVENT_COUNT; ++i)
        if (strcmp(name, event_names[i]) == 0)
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "method") == 0) {
        method_counterexample();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "registry") == 0) {
        registry_demo();
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "trace") == 0) {
        for (int i = 2; i < argc; ++i)
            if (!known_event(argv[i]))
                goto usage;
        run_trace(argc - 2, argv + 2);
        return 0;
    }
usage:
    fprintf(stderr, "usage: nodes method | registry | trace EVENT...\n");
    return 2;
}
