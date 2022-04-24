#define _POSIX_C_SOURCE 199309L
#include <assert.h>
#include <err.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include "hp.h"

#define N_THREADS 4000
#define N_READERS 2000
#define N_WRITERS 2000
#define N_ITERS 20
#define ARRAY_SIZE(x) sizeof(x) / sizeof(*x)

typedef struct {
    unsigned int v1, v2, v3;
} config_t;

static config_t *shared_config;
static domain_t *config_dom;

config_t *create_config()
{
    config_t *out = calloc(1, sizeof(config_t));
    if (!out)
        err(EXIT_FAILURE, "calloc");
    return out;
}

void delete_config(void *arg)
{
    config_t *conf = (config_t *) arg;
    assert(conf);
    free(conf);
}

static void print_config(const char *name, const config_t *conf)
{
    printf("%s : { 0x%08x, 0x%08x, 0x%08x }\n", name, conf->v1, conf->v2,
           conf->v3);
}

void init()
{
    shared_config = create_config();
    config_dom = domain_new(delete_config);
    if (!config_dom)
        err(EXIT_FAILURE, "domain_new");
}

void deinit()
{
    delete_config(shared_config);
    domain_free(config_dom);
}

static void *reader_thread(void *arg)
{
    (void) arg;

    for (int i = 0; i < N_ITERS; ++i) {
        config_t *safe_config =
            (config_t *) load(config_dom, (uintptr_t *) &shared_config);
        if (!safe_config)
            err(EXIT_FAILURE, "load");

        print_config("read config    ", safe_config);
        drop(config_dom, (uintptr_t) safe_config);
    }

    return NULL;
}

static void *writer_thread(void *arg)
{
    (void) arg;

    for (int i = 0; i < N_ITERS / 2; ++i) {
        config_t *new_config = create_config();
        new_config->v1 = rand();
        new_config->v2 = rand();
        new_config->v3 = rand();
        print_config("updating config", new_config);

        config_t *old_config = (config_t *) swap(
            config_dom, (uintptr_t *) &shared_config, (uintptr_t) new_config);
        if (!old_config) {
            free(new_config);
            err(EXIT_FAILURE, "swap");
        } else {
            print_config("updated config ", new_config);
            drop(config_dom, (uintptr_t) new_config);
            cleanup_ptr(config_dom, (uintptr_t) old_config, 0);
        }
    }

    return NULL;
}

static void *cleaner_thread(void *arg)
{
    bool *stop = (bool *) arg;
    struct itimerspec new_value;
    new_value.it_value.tv_sec = 0;
    new_value.it_value.tv_nsec = 1000;

    new_value.it_interval.tv_sec = 1;
    new_value.it_interval.tv_nsec = 0;

    int fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (fd == -1)
        warn("timerfd_create");

    if (timerfd_settime(fd, TFD_TIMER_ABSTIME, &new_value, NULL))
        warn("timerfd_settime");

    while (!atomic_load(stop, __ATOMIC_RELAXED)) {
        uint64_t exp;
        ssize_t s = read(fd, &exp, sizeof(uint64_t));
        if (s != sizeof(uint64_t)) {
            warn("read failed");
            return NULL;
        }
        printf("cleanup\n");
        cleanup(config_dom, DEFER_DEALLOC);
    }
    close(fd);
    return NULL;
}

int main()
{
    // pthread_t readers[N_READERS], writers[N_WRITERS], cleaner;
    // bool stop = false;
    pthread_t rwthread[N_THREADS];

    init();

    for (size_t i = 0; i < ARRAY_SIZE(rwthread); i++) {
        if (pthread_create(rwthread + i, NULL,
                           i & 1 ? reader_thread : writer_thread, NULL))
            warn("pthread_create");
    }
    for (size_t i = 0; i < ARRAY_SIZE(rwthread); i++) {
        if (pthread_join(rwthread[i], NULL))
            warn("pthread_join");
    }
    /* Start threads */
    // for (size_t i = 0; i < ARRAY_SIZE(readers); ++i) {
    //     if (pthread_create(readers + i, NULL, reader_thread, NULL))
    //         warn("pthread_create");
    // }
    // for (size_t i = 0; i < ARRAY_SIZE(writers); ++i) {
    //     if (pthread_create(writers + i, NULL, writer_thread, NULL))
    //         warn("pthread_create");
    // }

    // if (pthread_create(&cleaner, NULL, cleaner_thread, &stop))
    //     warn("pthread_create");

    /* Wait for threads to finish */
    // for (size_t i = 0; i < ARRAY_SIZE(readers); ++i) {
    //     if (pthread_join(readers[i], NULL))
    //         warn("pthread_join");
    // }
    // for (size_t i = 0; i < ARRAY_SIZE(writers); ++i) {
    //     if (pthread_join(writers[i], NULL))
    //         warn("pthread_join");
    // }

    // while (!atomic_test_and_set(&stop, __ATOMIC_RELAXED))
    //     ;
    // if (pthread_join(cleaner, NULL))
    //     warn("pthread_join");

    deinit();

    TRACE_PRINT();

    return EXIT_SUCCESS;
}
