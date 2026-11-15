// Exercise the actual production wait path without a Vulkan driver.
#include "../../gfx/vk_android/vka_pso.c"
#include <assert.h>
vka_dev V; vka_fns vk;
uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static gval pipeline = {.busy=1};
static _Atomic int returned;
static void *draw(void *unused) {
    (void)unused; pthread_mutex_lock(&g_mu); busy_wait(&pipeline);
    assert(!pipeline.busy && (uintptr_t)pipeline.p == 123);
    atomic_store(&returned, 1); pthread_mutex_unlock(&g_mu); return NULL;
}
int main(void) {
    pthread_t worker; assert(!pthread_create(&worker, NULL, draw, NULL));
    for (int i = 0; i < 5000 && !atomic_load(&g_demand_waiters); i++) usleep(1000);
    assert(atomic_load(&g_demand_waiters) == 1 && !returned);
    pthread_mutex_lock(&g_mu); pipeline.p = (VkPipeline)(uintptr_t)123; pipeline.busy = 0;
    pthread_cond_broadcast(&g_busy_cv); pthread_mutex_unlock(&g_mu);
    assert(!pthread_join(worker, NULL) && returned && !atomic_load(&g_demand_waiters));
    puts("pending pipeline: native draw waits for a completed pipeline and clears demand priority without dropping the draw");
    return 0;
}
