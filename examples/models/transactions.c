#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum state { QUEUED, DELIVERED, REPLIED, FAILED };
enum request_owner { REQUEST_DRIVER, REQUEST_SERVER, REQUEST_NONE };
enum reply_owner { REPLY_NONE, REPLY_PREPARED, REPLY_CLIENT };
enum output_state { OUTPUT_NONE, OUTPUT_PENDING, OUTPUT_DELIVERED, OUTPUT_FAULT };
enum display { QUIET, WITH_RESULTS, TABLE_ONLY };
enum event {
    EV_DELIVER, EV_RECEIVE_FAULT, EV_REPLY, EV_REPLY_EMPTY, EV_REPLY_FAULT,
    EV_CANCEL, EV_SERVER_EXIT, EV_WRONG_REPLY, EV_OUTPUT_OK, EV_OUTPUT_FAULT,
    EV_FREE_REQUEST, EV_FREE_REPLY, EV_REAP, EVENT_COUNT
};

static const char *const state_names[] = {"QUEUED", "DELIVERED", "REPLIED", "FAILED"};
static const char *const request_names[] = {"DRIVER", "SERVER", "NONE"};
static const char *const reply_names[] = {"NONE", "PREPARED", "CLIENT"};
static const char *const output_names[] = {"NONE", "PENDING", "DELIVERED", "FAULT"};
static const char *const event_names[] = {
    "deliver", "receive_fault", "reply", "reply_empty", "reply_fault",
    "cancel", "server_exit", "wrong_reply", "output_ok", "output_fault",
    "free_request", "free_reply", "reap"
};

struct transaction {
    unsigned int id, logical_call, client, sequence, result;
    enum state state;
    enum request_owner request;
    enum reply_owner reply;
    enum output_state output;
    unsigned int terminal_count, request_frees, reply_allocs, reply_frees;
    unsigned int record_live, record_reaps;
    int server_alive;
};

static struct transaction make_transaction(unsigned int id, unsigned int call,
                                           unsigned int result)
{
    struct transaction transaction = {0};
    transaction.id = id;
    transaction.logical_call = call;
    transaction.client = 1;
    transaction.sequence = 7;
    transaction.result = result;
    transaction.state = QUEUED;
    transaction.request = REQUEST_DRIVER;
    transaction.record_live = 1;
    transaction.server_alive = 1;
    return transaction;
}

static int terminal(enum state state)
{
    return state == REPLIED || state == FAILED;
}

static int valid(const struct transaction *transaction)
{
    return transaction->terminal_count == (unsigned int)terminal(transaction->state) &&
        transaction->request_frees == (unsigned int)(transaction->request == REQUEST_NONE) &&
        transaction->reply_allocs >= transaction->reply_frees &&
        transaction->reply_allocs - transaction->reply_frees ==
            (unsigned int)(transaction->reply != REPLY_NONE) &&
        transaction->record_reaps == 1 - transaction->record_live &&
        (transaction->record_live || terminal(transaction->state)) &&
        (transaction->request != REQUEST_DRIVER || transaction->state == QUEUED) &&
        (transaction->request != REQUEST_SERVER || transaction->state != QUEUED) &&
        (transaction->server_alive || transaction->request == REQUEST_NONE) &&
        ((transaction->output != OUTPUT_NONE) == (transaction->state == REPLIED)) &&
        (transaction->reply != REPLY_PREPARED || transaction->output == OUTPUT_PENDING) &&
        (transaction->reply != REPLY_CLIENT || transaction->output == OUTPUT_DELIVERED);
}

static void finish_transaction(struct transaction *transaction, enum state state)
{
    transaction->state = state;
    ++transaction->terminal_count;
}

static void release_request(struct transaction *transaction)
{
    if (transaction->request != REQUEST_NONE) {
        transaction->request = REQUEST_NONE;
        ++transaction->request_frees;
    }
}

static int needs_record(enum event event)
{
    return event == EV_DELIVER || event == EV_RECEIVE_FAULT || event == EV_REPLY ||
        event == EV_REPLY_EMPTY || event == EV_REPLY_FAULT || event == EV_CANCEL ||
        event == EV_WRONG_REPLY;
}

static int step(struct transaction *transaction, enum event event, enum display display)
{
    enum state before = transaction->state;
    const char *reason = "ok";
    int accepted = 0;

    if (needs_record(event) && !transaction->record_live) {
        reason = "unknown";
        goto observed;
    }
    switch (event) {
    case EV_DELIVER:
    case EV_RECEIVE_FAULT:
        if (transaction->state != QUEUED) {
            reason = terminal(transaction->state) ? "terminal" : "not_queued";
            break;
        }
        if (event == EV_DELIVER) {
            transaction->state = DELIVERED;
            transaction->request = REQUEST_SERVER;
        } else {
            reason = "receive_rollback";
        }
        accepted = 1;
        break;
    case EV_REPLY:
    case EV_REPLY_EMPTY:
    case EV_REPLY_FAULT:
        if (transaction->state != DELIVERED) {
            reason = terminal(transaction->state) ? "terminal" : "not_delivered";
            break;
        }
        if (event == EV_REPLY_FAULT) {
            ++transaction->reply_allocs;
            ++transaction->reply_frees;
            reason = "reply_rollback";
        } else {
            if (event == EV_REPLY) {
                transaction->reply = REPLY_PREPARED;
                ++transaction->reply_allocs;
            }
            transaction->output = OUTPUT_PENDING;
            finish_transaction(transaction, REPLIED);
        }
        accepted = 1;
        break;
    case EV_CANCEL:
        if (terminal(transaction->state)) {
            reason = "terminal";
            break;
        }
        if (transaction->state == QUEUED)
            release_request(transaction);
        finish_transaction(transaction, FAILED);
        accepted = 1;
        break;
    case EV_SERVER_EXIT:
        if (!transaction->server_alive) {
            reason = "server_gone";
            break;
        }
        transaction->server_alive = 0;
        release_request(transaction);
        if (!terminal(transaction->state))
            finish_transaction(transaction, FAILED);
        accepted = 1;
        reason = "server_cleanup";
        break;
    case EV_WRONG_REPLY:
        reason = "wrong_server";
        break;
    case EV_OUTPUT_OK:
    case EV_OUTPUT_FAULT:
        if (transaction->output != OUTPUT_PENDING) {
            reason = "no_pending_output";
            break;
        }
        if (event == EV_OUTPUT_OK) {
            transaction->output = OUTPUT_DELIVERED;
            if (transaction->reply == REPLY_PREPARED)
                transaction->reply = REPLY_CLIENT;
        } else {
            transaction->output = OUTPUT_FAULT;
            if (transaction->reply == REPLY_PREPARED) {
                transaction->reply = REPLY_NONE;
                ++transaction->reply_frees;
            }
            reason = "output_rollback";
        }
        accepted = 1;
        break;
    case EV_FREE_REQUEST:
        if (transaction->request != REQUEST_SERVER) {
            reason = transaction->request == REQUEST_DRIVER ? "not_delivered" : "not_owned";
            break;
        }
        release_request(transaction);
        accepted = 1;
        break;
    case EV_FREE_REPLY:
        if (transaction->reply != REPLY_CLIENT) {
            reason = transaction->reply == REPLY_PREPARED ? "not_delivered" : "not_owned";
            break;
        }
        transaction->reply = REPLY_NONE;
        ++transaction->reply_frees;
        accepted = 1;
        break;
    case EV_REAP:
        if (!terminal(transaction->state) || !transaction->record_live) {
            reason = transaction->record_live ? "not_terminal" : "already_reaped";
            break;
        }
        transaction->record_live = 0;
        ++transaction->record_reaps;
        accepted = 1;
        break;
    case EVENT_COUNT:
        reason = "invalid_event";
        break;
    }

observed:
    if (!valid(transaction)) {
        fprintf(stderr, "transaction invariant failed id=%u event=%s\n",
                transaction->id, event_names[event]);
        exit(1);
    }
    if (display != QUIET) {
        printf("event=step id=%u logical_call=%u client=%u sequence=%u operation=%s "
               "accepted=%d reason=%s from=%s state=%s terminal_count=%u "
               "request=%s reply=%s output=%s record_live=%u request_frees=%u "
               "reply_allocs=%u reply_frees=%u record_reaps=%u server_alive=%d\n",
               transaction->id, transaction->logical_call, transaction->client,
               transaction->sequence, event_names[event], accepted, reason,
               state_names[before], state_names[transaction->state], transaction->terminal_count,
               request_names[transaction->request], reply_names[transaction->reply],
               output_names[transaction->output], transaction->record_live,
               transaction->request_frees, transaction->reply_allocs, transaction->reply_frees,
               transaction->record_reaps, transaction->server_alive);
        if (display == WITH_RESULTS && accepted && event == EV_OUTPUT_OK) {
            printf("event=client_result id=%u logical_call=%u client=%u sequence=%u result=",
                   transaction->id, transaction->logical_call, transaction->client,
                   transaction->sequence);
            if (transaction->reply == REPLY_CLIENT)
                printf("%u\n", transaction->result);
            else
                puts("none");
        }
    }
    return accepted;
}

static void expect_step(struct transaction *transaction, enum event event, int accepted)
{
    if (step(transaction, event, WITH_RESULTS) != accepted) {
        fprintf(stderr, "unexpected event outcome id=%u event=%s\n",
                transaction->id, event_names[event]);
        exit(1);
    }
}

static int reply_id(struct transaction *transactions, size_t count, unsigned int id)
{
    for (size_t i = 0; i < count; ++i)
        if (transactions[i].id == id && transactions[i].record_live)
            return step(&transactions[i], EV_REPLY, WITH_RESULTS);
    printf("event=rejected id=%u reason=unknown\n", id);
    return 0;
}

static void summary(const struct transaction *transactions, size_t count)
{
    unsigned int records = 0, requests = 0, replies = 0, terminals = 0;
    for (size_t i = 0; i < count; ++i) {
        records += transactions[i].record_live;
        requests += transactions[i].request != REQUEST_NONE;
        replies += transactions[i].reply != REPLY_NONE;
        terminals += transactions[i].terminal_count;
    }
    printf("event=summary records=%u request_slots=%u reply_slots=%u terminal_total=%u\n",
           records, requests, replies, terminals);
}

static void run_slot(void)
{
    unsigned int slot = 42;
    printf("event=slot_write logical_call=2 client=1 sequence=7 result=%u overwritten_call=none\n", slot);
    slot = 3;
    printf("event=slot_write logical_call=1 client=1 sequence=7 result=%u overwritten_call=2\n", slot);
    printf("event=slot_read logical_call=1 client=1 sequence=7 expected=3 observed=%u\n", slot);
    printf("event=slot_read logical_call=2 client=1 sequence=7 expected=42 observed=%u\n", slot);
}

static void run_reorder(void)
{
    struct transaction transactions[2] = {
        make_transaction(101, 1, 3), make_transaction(102, 2, 42)
    };
    if (reply_id(transactions, 2, 101) || reply_id(transactions, 2, 999))
        exit(1);
    expect_step(&transactions[0], EV_DELIVER, 1);
    expect_step(&transactions[1], EV_DELIVER, 1);
    expect_step(&transactions[1], EV_WRONG_REPLY, 0);
    expect_step(&transactions[0], EV_FREE_REQUEST, 1);
    if (!reply_id(transactions, 2, 102) || reply_id(transactions, 2, 102))
        exit(1);
    expect_step(&transactions[1], EV_REAP, 1);
    expect_step(&transactions[1], EV_OUTPUT_OK, 1);
    expect_step(&transactions[1], EV_FREE_REQUEST, 1);
    expect_step(&transactions[1], EV_FREE_REPLY, 1);
    if (!reply_id(transactions, 2, 101))
        exit(1);
    expect_step(&transactions[0], EV_CANCEL, 0);
    expect_step(&transactions[0], EV_REAP, 1);
    expect_step(&transactions[0], EV_OUTPUT_OK, 1);
    expect_step(&transactions[0], EV_FREE_REPLY, 1);
    summary(transactions, 2);
}

static void run_matrix(void)
{
    for (enum state state = QUEUED; state <= FAILED; ++state) {
        for (enum event event = EV_DELIVER; event < EVENT_COUNT; ++event) {
            struct transaction transaction = make_transaction(101, 1, 3);
            if (state != QUEUED)
                step(&transaction, EV_DELIVER, QUIET);
            if (state == REPLIED)
                step(&transaction, EV_REPLY, QUIET);
            else if (state == FAILED)
                step(&transaction, EV_CANCEL, QUIET);
            step(&transaction, event, TABLE_ONLY);
        }
    }
}

static enum event parse_event(const char *name)
{
    for (enum event event = EV_DELIVER; event < EVENT_COUNT; ++event)
        if (!strcmp(name, event_names[event]))
            return event;
    return EVENT_COUNT;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "slot")) {
        run_slot();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "reorder")) {
        run_reorder();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "matrix")) {
        run_matrix();
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "trace")) {
        struct transaction transaction = make_transaction(101, 1, 3);
        for (int i = 2; i < argc; ++i)
            if (parse_event(argv[i]) == EVENT_COUNT)
                goto usage;
        for (int i = 2; i < argc; ++i)
            step(&transaction, parse_event(argv[i]), WITH_RESULTS);
        summary(&transaction, 1);
        return 0;
    }
usage:
    fprintf(stderr, "usage: transactions slot|reorder|matrix|trace EVENT...\n");
    return 2;
}
