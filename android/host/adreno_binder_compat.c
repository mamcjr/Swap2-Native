// libbinder_compat_hook.so: AIBinder_Class_setTransactionCodeToFunctionNameMap for Qualcomm's 891.x custom drivers on Android 15 and older (API < 36).
// Qualcomm 891.x (built for Android 16) imports this Binder tracing annotation, which libbinder_ndk.so exports only from API 36. On older systems the driver's
// namespace cannot resolve it, the custom driver fails to load, and hook_impl silently falls back to the system driver (logcat hook_impl: cannot locate symbol
// "AIBinder_Class_setTransactionCodeToFunctionNameMap", then "falling back!"). The stub in main_hook.c does not help: main_hook lives in the Vulkan loader's
// namespace, while hook_impl.cpp loads the driver into a namespace of its own, parented to the vendor one. hook_impl.cpp loads this library into that namespace
// with RTLD_GLOBAL (and it is linked with -z global) before the driver, and only for a custom driver on API < 36, so on Android 16 and with the system driver
// nothing changes. Only the optional transaction-name annotation (used for tracing) is lost; no Binder transaction is replaced. Where the system does have the
// function, the call is forwarded to it unchanged.
// After Banana Native f71720e0 (MIT, android/host/adreno_binder_compat.c; there it is built into hook_impl with -z global, which also put hook_impl's
// statically linked libc++ exports into every driver namespace's global group on every API level; here it is a library of its own, loaded only where needed).
#include <dlfcn.h>
#include <pthread.h>
#include <stddef.h>

typedef struct AIBinder_Class AIBinder_Class;
typedef void (*set_names_fn)(AIBinder_Class *, const char *const *, size_t);
static set_names_fn system_set_names;
static pthread_once_t names_once = PTHREAD_ONCE_INIT;

static void find_system_set_names(void) {
    // the library stays loaded for the pointer's lifetime (never dlclosed)
    void *lib = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_LOCAL);
    if (lib) system_set_names = (set_names_fn)dlsym(lib, "AIBinder_Class_setTransactionCodeToFunctionNameMap");
}

__attribute__((visibility("default")))
void AIBinder_Class_setTransactionCodeToFunctionNameMap(AIBinder_Class *clazz, const char *const *names, size_t length) {
    pthread_once(&names_once, find_system_set_names);
    if (system_set_names && system_set_names != AIBinder_Class_setTransactionCodeToFunctionNameMap) system_set_names(clazz, names, length);
}
