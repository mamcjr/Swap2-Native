// Swap2-Native: the runtime-profile host (NX_RUNTIME_PROFILE) reads the pack's vk_prewarm.bin at start (vka_pso.c pw_load) instead of compiling a game's seed in.
#pragma once
#include <stddef.h>
static const unsigned char dm_vk_prewarm_seed[1] __attribute__((aligned(8))) = {0};
static const size_t dm_vk_prewarm_seed_len = 0;
