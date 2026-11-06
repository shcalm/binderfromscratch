#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void show(const char *event, const int *result)
{
    printf("event=%s result=%d address=%p pid=%ld\n",
           event, *result, (const void *)result, (long)getpid());
}

static void *worker(void *argument)
{
    int *result = argument;

    *result = 1 + 2;
    show("worker_write", result);
    return NULL;
}

int main(void)
{
    int result = -1;
    pthread_t thread;
    int error;

    show("before_create", &result);
    error = pthread_create(&thread, NULL, worker, &result);
    if (error != 0) {
        fprintf(stderr, "pthread_create: %s\n", strerror(error));
        return 1;
    }
    /* Join establishes completion and visibility before the next read. */
    error = pthread_join(thread, NULL);
    if (error != 0) {
        fprintf(stderr, "pthread_join: %s\n", strerror(error));
        return 1;
    }
    show("after_join", &result);
    return result == 3 ? 0 : 1;
}
