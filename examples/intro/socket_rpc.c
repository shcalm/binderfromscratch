#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

enum { OP_ADD = 1, STATUS_OK = 0, STATUS_OVERFLOW = 1, STATUS_BAD_OP = 2 };

static volatile sig_atomic_t interrupted;

static void on_termination(int signal_number)
{
    interrupted = signal_number;
}

static int parse_u32(const char *text, uint32_t *value)
{
    char *end;
    unsigned long long parsed;

    if (*text < '0' || *text > '9')
        return -1;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno || *end != '\0' || parsed > UINT32_MAX)
        return -1;
    *value = (uint32_t)parsed;
    return 0;
}

/* Normal packets have three words; fault injection also sends two or four. */
static int send_words(int fd, const uint32_t *words, size_t count)
{
    uint32_t wire[4];
    ssize_t size;

    for (size_t i = 0; i < count; ++i)
        wire[i] = htonl(words[i]);
    do {
        size = send(fd, wire, count * sizeof(*wire), MSG_NOSIGNAL);
    } while (size < 0 && errno == EINTR);
    return size == (ssize_t)(count * sizeof(*wire)) ? 0 : -1;
}

static int send_packet(int fd, const uint32_t words[3])
{
    return send_words(fd, words, 3);
}

static int receive_packet(int fd, uint32_t words[3])
{
    uint32_t wire[3];
    ssize_t size;

    do {
        size = recv(fd, wire, sizeof(wire), MSG_TRUNC);
    } while (size < 0 && errno == EINTR);
    if (size != (ssize_t)sizeof(wire))
        return -1;
    for (int i = 0; i < 3; ++i)
        words[i] = ntohl(wire[i]);
    return 0;
}

static int serve_once(int fd, int control_fd)
{
    uint32_t request[3];
    uint32_t reply[3] = { STATUS_OK, 0, (uint32_t)getpid() };

    if (receive_packet(fd, request) < 0) {
        fprintf(stderr, "server protocol error: malformed request\n");
        return 1;
    }
    if (control_fd >= 0) {
        char byte = 'R';
        ssize_t size;

        if (write(control_fd, &byte, 1) != 1)
            return 1;
        /* Deliberately send no reply; parent close is the cleanup handshake. */
        do {
            size = read(control_fd, &byte, 1);
        } while (size < 0 && errno == EINTR);
        return size == 0 ? 0 : 1;
    }
    if (request[0] != OP_ADD)
        reply[0] = STATUS_BAD_OP;
    else if (request[1] > UINT32_MAX - request[2])
        reply[0] = STATUS_OVERFLOW;
    else
        reply[1] = request[1] + request[2];
    return send_packet(fd, reply) < 0 ? 1 : 0;
}

static int wait_without_deadline(int fd, const sigset_t *original_mask)
{
    int ready;
    fd_set reads;

    do {
        FD_ZERO(&reads);
        FD_SET(fd, &reads);
        /* Unblock SIGTERM atomically with waiting; there is no RPC deadline. */
        ready = pselect(fd + 1, &reads, NULL, NULL, NULL, original_mask);
    } while (ready < 0 && errno == EINTR && !interrupted);
    return ready > 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
    uint32_t request[4] = { OP_ADD, 0, 0, 0 }, reply[3];
    int sockets[2], control[2] = { -1, -1 }, status, result = 0;
    int server_exit = argc == 4 && strcmp(argv[3], "--server-exit") == 0;
    int no_reply = argc == 4 && strcmp(argv[3], "--server-no-reply") == 0;
    int short_request = argc == 4 && strcmp(argv[3], "--short-request") == 0;
    int long_request = argc == 4 && strcmp(argv[3], "--long-request") == 0;
    int unknown_op = argc == 4 && strcmp(argv[3], "--unknown-op") == 0;
    sigset_t original_mask;
    pid_t child;

    if ((argc != 3 && !(server_exit || no_reply || short_request ||
                        long_request || unknown_op)) ||
        parse_u32(argv[1], &request[1]) < 0 ||
        parse_u32(argv[2], &request[2]) < 0) {
        fprintf(stderr, "invalid argument: use socket_rpc U32 U32 "
                "[--server-exit|--server-no-reply|--short-request|"
                "--long-request|--unknown-op]\n");
        return 2;
    }
    if (unknown_op)
        request[0] = 99;
    if (no_reply) {
        struct sigaction action = { 0 };
        sigset_t blocked;

        action.sa_handler = on_termination;
        sigemptyset(&action.sa_mask);
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGTERM);
        if (sigaction(SIGTERM, &action, NULL) < 0 ||
            sigprocmask(SIG_BLOCK, &blocked, &original_mask) < 0 ||
            socketpair(AF_UNIX, SOCK_STREAM, 0, control) < 0) {
            perror("no-reply synchronization setup");
            return 3;
        }
        /* Even an inherited blocked SIGTERM must reach the harness handler. */
        sigdelset(&original_mask, SIGTERM);
    }
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) < 0) {
        perror("socketpair");
        if (no_reply) {
            close(control[0]);
            close(control[1]);
        }
        return 3;
    }
    child = fork();
    if (child < 0) {
        perror("fork");
        close(sockets[0]);
        close(sockets[1]);
        if (no_reply) {
            close(control[0]);
            close(control[1]);
        }
        return 3;
    }
    if (child == 0) {
        close(sockets[0]);
        if (no_reply) {
            close(control[0]);
            signal(SIGTERM, SIG_DFL);
            sigprocmask(SIG_SETMASK, &original_mask, NULL);
        }
        result = server_exit ? 0 : serve_once(sockets[1], control[1]);
        close(sockets[1]);
        if (no_reply)
            close(control[1]);
        _exit(result);
    }
    close(sockets[1]);
    if (no_reply)
        close(control[1]);
    if (send_words(sockets[0], request,
                   short_request ? 2 : long_request ? 4 : 3) < 0) {
        fprintf(stderr, "transport error: request send failed\n");
        result = 3;
    }
    if (result == 0 && no_reply) {
        char ready;
        ssize_t size;

        do {
            size = read(control[0], &ready, 1);
        } while (size < 0 && errno == EINTR);
        if (size != 1 || ready != 'R') {
            fprintf(stderr, "transport error: server readiness failed\n");
            result = 3;
        } else {
            printf("event=request_received_no_reply server_pid=%ld\n", (long)child);
            fflush(stdout);
            if (wait_without_deadline(sockets[0], &original_mask) < 0) {
                if (interrupted) {
                    fprintf(stderr, "harness interruption: no protocol timeout\n");
                    result = 124;
                } else {
                    perror("pselect");
                    result = 3;
                }
            }
        }
    }
    if (result == 0) {
        if (receive_packet(sockets[0], reply) < 0) {
            fprintf(stderr, "transport error: no complete reply\n");
            result = 3;
        } else if (reply[0] != STATUS_OK) {
            fprintf(stderr, "remote error: %s\n",
                    reply[0] == STATUS_OVERFLOW ? "overflow" : "unknown operation");
            result = 2;
        } else {
            printf("client_pid=%ld server_pid=%" PRIu32 " result=%" PRIu32 "\n",
                   (long)getpid(), reply[2], reply[1]);
        }
    }
    close(sockets[0]);
    /* Releasing the control endpoint lets the no-reply child exit before wait. */
    if (no_reply)
        close(control[0]);
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            perror("waitpid");
            return 3;
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return 3;
    return result;
}
