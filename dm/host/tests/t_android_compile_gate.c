#include "../../gfx/vk_android/vka_compile_gate.h"
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
static vkc_gate gate = VKC_GATE_INIT;
static _Atomic int limit_now, entered, release_now, finished;
static int limit(void *p) { (void)p; return atomic_load(&limit_now); }
static void *worker(void *p) {
    (void)p; vkc_enter(&gate, limit, NULL); atomic_fetch_add(&entered, 1);
    while (!atomic_load(&release_now)) usleep(1000);
    vkc_leave(&gate); atomic_fetch_add(&finished, 1); return NULL;
}
static void await(_Atomic int *value, int n) { for (int i = 0; i < 5000 && atomic_load(value) < n; i++) usleep(1000); assert(atomic_load(value) >= n); }
int main(void) {
    pthread_t threads[6]; for (int i = 0; i < 6; i++) assert(!pthread_create(&threads[i], NULL, worker, NULL));
    pthread_mutex_lock(&gate.mu); assert(!gate.active); pthread_mutex_unlock(&gate.mu);
    atomic_store(&limit_now, 2); vkc_wake(&gate); await(&entered, 2);
    pthread_mutex_lock(&gate.mu); assert(gate.active == 2 && entered == 2); pthread_mutex_unlock(&gate.mu);
    atomic_store(&limit_now, 0); atomic_store(&release_now, 1); await(&finished, 2);
    assert(entered == 2); // admitted work can finish, but demand/pressure prevents new speculative work
    atomic_store(&limit_now, 1); vkc_wake(&gate);
    for (int i = 0; i < 6; i++) assert(!pthread_join(threads[i], NULL));
    assert(finished == 6 && !gate.active);
    puts("compile gate: bounded parallel work, demand/pressure pause, completion and resume passed");
    return 0;
}
