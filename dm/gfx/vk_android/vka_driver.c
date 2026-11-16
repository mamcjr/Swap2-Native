// Custom GPU drivers: DM_VK_DRIVER names a Vulkan driver the launcher installed into the app's private files (android/README.md "Custom GPU drivers"), such as a Mesa Turnip
// build or another Qualcomm release, and the replay runs on it instead of the system's. The driver is loaded as Eden does it, through libadrenotools (BSD-2-Clause,
// dm/third_party/libadrenotools): a private copy of the system libvulkan.so (the loader) in a linker namespace of its own whose hook answers the loader's request for the
// vendor driver with the custom one. libadrenotools is a library of its own, libnxadreno.so, opened here only when a driver is named: its constructor (liblinkernsbypass)
// changes dlopen's page protection, which the system driver path must not see.
//   DM_VK_DRIVER=<folder>   the folder holds the driver and its meta.json ("libraryName": the .so to load), as the driver zips Eden and others use
//   DM_VK_DRIVER=<path>.so  that library (its folder must hold whatever else it loads)
//   unset, empty, "system"  the system libvulkan.so, as before
// DM_VK_HOOKDIR overrides where the hook libraries are (by default the folder libnxhost.so was loaded from, inside the APK). Restart only (vka_ctl.c g_restart).
// A driver that fails to load falls back to the system one, with a [RED] line saying why.
//
// Qualcomm's driver settings (DM_QGL): Qualcomm's proprietary driver reads settings from qgl_config.txt in /data/vendor/gpu (or /data/misc/gpu), which an app cannot
// write. DM_QGL=<key>=<value>[;<key>=<value>...] writes those lines into the app's own files (DM_TEMP/qgl/data/{vendor,misc}/gpu/qgl_config.txt, after the file's
// header line) and loads the driver through adrenotools with ADRENOTOOLS_DRIVER_FILE_REDIRECT: libfile_redirect_hook.so's fopen gives the driver that copy when it opens
// one of the two paths, and every other file as it is (hook_impl.cpp). The system driver is then loaded through adrenotools too, which it otherwise never is. Keys the
// community reports for the Adreno driver (after Erick's Ocarina fork, 2392d1f): rendermode (0 system memory without binning, 1 GMEM with binning, 2 GMEM without, 3
// system memory with binning), forcegmemload, forcegmemstore (0 / 1). The driver may ignore any of them; Turnip reads none. Unset: nothing of this, as before. Restart only
#if defined(__ANDROID__)
#include "vka.h"
#include <ctype.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#define ADRENOTOOLS_DRIVER_CUSTOM 1   // dm/third_party/libadrenotools/include/adrenotools/priv.h
#define ADRENOTOOLS_DRIVER_FILE_REDIRECT 2
typedef void *(*open_libvulkan_fn)(int dlopen_mode, int feature_flags, const char *tmp_lib_dir, const char *hook_lib_dir, const char *custom_driver_dir,
                                   const char *custom_driver_name, const char *file_redirect_dir, void **user_mapping_handle);

static char g_tag[96];   // the custom driver in use (file-name safe), empty for the system's

const char *vka_driver_tag(void) { return g_tag[0] ? g_tag : NULL; }

// "libraryName" from a driver folder's meta.json: the one string value the loader needs (a small JSON file; no parser for one key)
static int meta_library(const char *dir, char *out, size_t cap) {
    char p[1024]; snprintf(p, sizeof p, "%s/meta.json", dir);
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    char buf[8192]; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    char *k = strstr(buf, "\"libraryName\""); if (!k) return 0;
    char *c = strchr(k + 13, ':'); if (!c) return 0;
    char *q = strchr(c, '"'); if (!q) return 0;
    char *e = strchr(q + 1, '"'); if (!e || (size_t)(e - q - 1) >= cap || e == q + 1) return 0;
    memcpy(out, q + 1, (size_t)(e - q - 1)); out[e - q - 1] = 0;
    return !strchr(out, '/');
}

static void set_tag(const char *s) {
    size_t n = 0;
    for (; *s && n + 1 < sizeof g_tag; s++) g_tag[n++] = isalnum((unsigned char)*s) || *s == '-' || *s == '_' || *s == '.' ? *s : '_';
    g_tag[n] = 0;
}

// The loader's lines (DLOG, DRED), kept and written at every vka_driver_open: the readiness check opens the driver in the launcher before the host's log is set up,
// where they went nowhere (a custom driver's line, and DM_QGL's, were missing from the game log); the replay's open writes them again into it
enum { NNOTE = 8 };
static char g_note[NNOTE][480]; static int g_note_red[NNOTE], g_nnote;
#define DNOTE(red, ...) do { if (g_nnote < NNOTE) { snprintf(g_note[g_nnote], sizeof g_note[0], __VA_ARGS__); g_note_red[g_nnote++] = (red); } } while (0)
#define DLOG(...) DNOTE(0, __VA_ARGS__)
#define DRED(...) DNOTE(1, __VA_ARGS__)

// Opened once per process: the readiness check (vka_readiness.c) may open it before the replay's vka_init does, and both get the same library (adrenotools would
// otherwise load the custom driver into a second namespace)
static void *driver_open(void);
void *vka_driver_open(void) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static int tried; static void *lib;
    pthread_mutex_lock(&mu);
    if (!tried) { tried = 1; lib = driver_open(); }
    for (int i = 0; i < g_nnote; i++) { if (g_note_red[i]) VRED("%s", g_note[i]); else VLOG("%s", g_note[i]); }
    pthread_mutex_unlock(&mu);
    return lib;
}
// DM_QGL's settings written as qgl_config.txt under DM_TEMP/qgl (the redirect folder into out): 1 when there is something to redirect ("Qualcomm's driver settings")
static int mkdirs(char *p) {   // every folder of p, a path with no trailing slash
    for (char *s = strchr(p + 1, '/');; s = strchr(s + 1, '/')) {
        if (s) *s = 0;
        struct stat st;   // (a folder that exists counts whatever mkdir says: Android answers EACCES, not EEXIST, for /storage/emulated, which the app may not write)
        int ok = !mkdir(p, 0700) || errno == EEXIST || (!stat(p, &st) && S_ISDIR(st.st_mode));
        if (s) *s = '/';
        if (!ok) return 0;
        if (!s) return 1;
    }
}
static int qgl_prepare(char *out, size_t cap) {
    const char *q = getenv("DM_QGL"), *t = getenv("DM_TEMP");
    if (!q || !*q) return 0;
    if (!t || !*t) { DRED("DM_QGL: no DM_TEMP to write qgl_config.txt in: the driver's own settings"); return 0; }
    char body[1024]; size_t n = (size_t)snprintf(body, sizeof body, "0x0=0x8675309\n");   // the file's header line, as the driver's own settings file has it
    for (const char *p = q; *p && n < sizeof body;) {   // key=value items, ';' or ',' between them; keys of [A-Za-z0-9_], values of [A-Za-z0-9_.-]
        const char *e = p + strcspn(p, ";,"), *eq = memchr(p, '=', (size_t)(e - p));
        int ok = eq && eq > p && eq + 1 < e;
        for (const char *c = p; ok && c < e; c++) if (c != eq && !(isalnum((unsigned char)*c) || *c == '_' || (c > eq && (*c == '.' || *c == '-')))) ok = 0;
        if (!ok) { DRED("DM_QGL=%s: \"%.*s\" is not key=value: the driver's own settings", q, (int)(e - p), p); return 0; }
        n += (size_t)snprintf(body + n, sizeof body - n, "%.*s\n", (int)(e - p), p);
        p = *e ? e + 1 : e;
    }
    if (n >= sizeof body) { DRED("DM_QGL: too long: the driver's own settings"); return 0; }
    snprintf(out, cap, "%s%sqgl", t, t[strlen(t) - 1] == '/' ? "" : "/");
    for (int i = 0; i < 2; i++) {
        char d[1200]; snprintf(d, sizeof d, "%s/data/%s/gpu", out, i ? "misc" : "vendor");
        if (!mkdirs(d)) { DRED("DM_QGL: cannot make %s (%s): the driver's own settings", d, strerror(errno)); return 0; }
        char f[1300]; snprintf(f, sizeof f, "%s/qgl_config.txt", d);
        FILE *o = fopen(f, "wb"); if (!o || fwrite(body, 1, n, o) != n) { if (o) fclose(o); DRED("DM_QGL: cannot write %s: the driver's own settings", f); return 0; }
        fclose(o);
    }
    for (char *c = body; (c = strchr(c, '\n')); ) *c = ' ';
    DLOG("Qualcomm driver settings (DM_QGL): %sin %s/data/{vendor,misc}/gpu/qgl_config.txt, given to the driver through adrenotools' file redirect", body, out);
    return 1;
}
static void *driver_open(void) {
    const char *drv = getenv("DM_VK_DRIVER");
    int custom = drv && *drv && strcmp(drv, "system");
    char qgl[1024]; int redirect = qgl_prepare(qgl, sizeof qgl);
    if (!custom && !redirect) return NULL;
    char dir[1024] = "", name[256] = "";
    if (custom) {
        size_t l = strlen(drv);
        if (l > 3 && !strcmp(drv + l - 3, ".so")) {
            const char *sl = strrchr(drv, '/');
            if (!sl || (size_t)(sl - drv) + 2 > sizeof dir || strlen(sl + 1) >= sizeof name) { DRED("DM_VK_DRIVER=%s: not a full path: the system driver is used", drv); custom = 0; }
            else { snprintf(dir, sizeof dir, "%.*s/", (int)(sl - drv), drv); snprintf(name, sizeof name, "%s", sl + 1); }
        } else {
            snprintf(dir, sizeof dir, "%s%s", drv, drv[l - 1] == '/' ? "" : "/");
            if (!meta_library(drv, name, sizeof name)) { DRED("DM_VK_DRIVER=%s: no meta.json with a libraryName there: the system driver is used", drv); custom = 0; }
        }
        if (custom && (!strncmp(dir, "/sdcard/", 8) || !strncmp(dir, "/storage/", 9)))   // shared storage is mounted without exec: dlopen fails there (adrenotools' own note)
            DRED("DM_VK_DRIVER=%s is on shared storage, where libraries cannot be loaded: install the driver from the launcher (it goes into the app's private files)", drv);
        if (!custom && !redirect) return NULL;
    }
    char hook[1024];
    const char *hd = getenv("DM_VK_HOOKDIR");
    if (hd && *hd) snprintf(hook, sizeof hook, "%s%s", hd, hd[strlen(hd) - 1] == '/' ? "" : "/");
    else {   // the folder libnxhost.so came from: inside the APK ("…/base.apk!/lib/arm64-v8a"), where the hook libraries are beside it; the linker searches zip paths too
        Dl_info di; const char *sl;
        if (!dladdr((void *)vka_driver_open, &di) || !di.dli_fname || !(sl = strrchr(di.dli_fname, '/'))) { DRED("DM_VK_DRIVER: cannot tell where libnxhost.so is: the system driver is used"); return NULL; }
        snprintf(hook, sizeof hook, "%.*s/", (int)(sl - di.dli_fname), di.dli_fname);
    }
    void *at = dlopen("libnxadreno.so", RTLD_NOW | RTLD_LOCAL);
    open_libvulkan_fn open_vk = at ? (open_libvulkan_fn)dlsym(at, "adrenotools_open_libvulkan") : NULL;
    if (!open_vk) { DRED("DM_VK_DRIVER: libnxadreno.so not loaded (%s): the system driver is used", dlerror()); return NULL; }
    void *lib = open_vk(RTLD_NOW, (custom ? ADRENOTOOLS_DRIVER_CUSTOM : 0) | (redirect ? ADRENOTOOLS_DRIVER_FILE_REDIRECT : 0), NULL, hook, custom ? dir : NULL, custom ? name : NULL,
                        redirect ? qgl : NULL, NULL);
    if (!lib && redirect && custom) {
        DRED("DM_QGL: adrenotools could not load %s%s with the file redirect (logcat tags adrenotools / hook_impl say why): the driver without its settings", dir, name);
        lib = open_vk(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, NULL, hook, dir, name, NULL, NULL);
    }
    if (!lib && !custom) { DRED("DM_QGL: adrenotools could not load the system driver with the file redirect (logcat tags adrenotools / hook_impl say why): the system driver without its settings"); return NULL; }
    if (!lib) { DRED("DM_VK_DRIVER: adrenotools could not load %s%s (hooks in %s; logcat tags adrenotools / hook_impl say why): the system driver is used", dir, name, hook); return NULL; }
    if (!custom) { DLOG("system GPU driver loaded through adrenotools for its settings file (DM_QGL; hooks in %s)", hook); return lib; }
    const char *base = dir + strlen(dir) - 1; while (base > dir && base[-1] != '/') base--;   // the folder's name, else the library's
    char b[128]; snprintf(b, sizeof b, "%.*s", (int)(dir + strlen(dir) - 1 - base), base);
    set_tag(b[0] ? b : name);
    DLOG("custom GPU driver: %s%s (hooks in %s), its own pipeline cache file (vk_pipelines-%s" VKA_PCACHE_GEN "[" VKA_PCACHE_PFB "].bin unless DM_VK_PCACHE names one)", dir, name, hook, g_tag);
    return lib;
}
#endif
