// SPDX-License-Identifier: BSD-2-Clause
// Copyright © 2021 Billy Laws
// libadrenotools' file-redirect hook (bylaws/libadrenotools src/hook/file_redirect_hook.c): loaded into the driver's namespace with ADRENOTOOLS_DRIVER_FILE_REDIRECT, its
// fopen answers the driver's. Here hook_fopen redirects only Qualcomm's qgl_config.txt (hook_impl.cpp; dm/gfx/vk_android/vka_driver.c DM_QGL)
#include "hook_impl.h"

__attribute__((visibility("default"))) FILE *fopen(const char *filename, const char *mode) {
	return hook_fopen(filename, mode);
}
