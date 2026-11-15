// Admission gate for speculative compiles. Demand builds never enter this gate.
#pragma once
#include <pthread.h>
#include <time.h>
#include <assert.h>
typedef struct { pthread_mutex_t mu; pthread_cond_t cv; int active; } vkc_gate;
#define VKC_GATE_INIT {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0}
static inline void vkc_enter(vkc_gate *g, int (*limit)(void *), void *ctx) {
    pthread_mutex_lock(&g->mu);
    while (g->active >= limit(ctx)) {
        struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 250000000; if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&g->cv, &g->mu, &deadline);
    }
    g->active++; pthread_mutex_unlock(&g->mu);
}
static inline void vkc_leave(vkc_gate *g) {
    pthread_mutex_lock(&g->mu); assert(g->active > 0); g->active--; pthread_cond_broadcast(&g->cv); pthread_mutex_unlock(&g->mu);
}
static inline void vkc_wake(vkc_gate *g) { pthread_mutex_lock(&g->mu); pthread_cond_broadcast(&g->cv); pthread_mutex_unlock(&g->mu); }
