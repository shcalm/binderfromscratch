#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum state { NEW, QUEUED, DELIVERED, COMPLETED, LOST };

struct request {
    unsigned int id;
    unsigned int client;
    uint32_t a, b;
    enum state state;
};

struct queue {
    struct request *items[2];
    size_t count;
};

static void deliver(struct request *request)
{
    request->state = DELIVERED;
    printf("event=delivered request_id=%u\n", request->id);
}

static int reply(struct request *requests, size_t count,
                 unsigned int id, uint32_t result)
{
    for (size_t i = 0; i < count; ++i) {
        struct request *request = &requests[i];

        if (request->id != id)
            continue;
        if (request->state != DELIVERED) {
            printf("event=reply_rejected request_id=%u reason=%s\n", id,
                   request->state == COMPLETED ? "duplicate" : "not_delivered");
            return -1;
        }
        request->state = COMPLETED;
        printf("event=client_result client=%u request_id=%u result=%" PRIu32 "\n",
               request->client, id, result);
        return 0;
    }
    printf("event=reply_rejected request_id=%u reason=unknown\n", id);
    return -1;
}

static int enqueue(struct queue *queue, struct request *request)
{
    if (queue->count == sizeof(queue->items) / sizeof(queue->items[0])) {
        printf("event=refused request_id=%u reason=queue_full\n", request->id);
        return -1;
    }
    request->state = QUEUED;
    queue->items[queue->count++] = request;
    printf("event=queued request_id=%u\n", request->id);
    return 0;
}

static struct request *receive(struct queue *queue)
{
    struct request *request;

    if (queue->count == 0)
        return NULL;
    request = queue->items[0];
    for (size_t i = 1; i < queue->count; ++i)
        queue->items[i - 1] = queue->items[i];
    --queue->count;
    deliver(request);
    return request;
}

static void run_slot(struct request requests[3], int overwrite)
{
    struct request *slot = &requests[0];

    slot->state = QUEUED;
    printf("event=queued request_id=%u\n", slot->id);
    if (overwrite) {
        printf("event=overwritten request_id=%u client=%u\n", slot->id, slot->client);
        slot->state = LOST;
        slot = &requests[1];
        slot->state = QUEUED;
        printf("event=queued request_id=%u\n", slot->id);
    } else {
        printf("event=refused request_id=%u reason=slot_full\n", requests[1].id);
    }
    deliver(slot);
    reply(requests, 3, slot->id, slot->a + slot->b);
    slot = NULL;
    if (!overwrite) {
        printf("event=retry request_id=%u\n", requests[1].id);
        slot = &requests[1];
        slot->state = QUEUED;
        printf("event=queued request_id=%u\n", slot->id);
        deliver(slot);
        reply(requests, 3, slot->id, slot->a + slot->b);
    }
}

static int run_fifo(struct request requests[3])
{
    struct queue queue = { { NULL, NULL }, 0 };
    struct request *first, *second;

    if (enqueue(&queue, &requests[0]) || enqueue(&queue, &requests[1]))
        return 1;
    if (enqueue(&queue, &requests[2]) == 0)
        return 1;
    if (reply(requests, 3, 999, 0) == 0 ||
        reply(requests, 3, requests[0].id, 0) == 0)
        return 1;
    first = receive(&queue);
    second = receive(&queue);
    if (!first || !second || receive(&queue) != NULL)
        return 1;
    /* Delivery order need not be reply order; each ID selects its own client. */
    if (reply(requests, 3, second->id, second->a + second->b) ||
        reply(requests, 3, second->id, 999) == 0 ||
        reply(requests, 3, first->id, first->a + first->b))
        return 1;
    return 0;
}

int main(int argc, char **argv)
{
    struct request requests[3] = {
        { 101, 1, 1, 2, NEW },
        { 102, 2, 19, 23, NEW },
        { 103, 3, 5, 8, NEW },
    };

    if (argc != 2 || (strcmp(argv[1], "overwrite") != 0 &&
                      strcmp(argv[1], "refuse") != 0 &&
                      strcmp(argv[1], "fifo") != 0)) {
        fprintf(stderr, "usage: request_queue overwrite|refuse|fifo\n");
        return 2;
    }
    if (strcmp(argv[1], "fifo") == 0)
        return run_fifo(requests);
    run_slot(requests, strcmp(argv[1], "overwrite") == 0);
    return 0;
}
