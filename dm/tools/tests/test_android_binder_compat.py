"""libbinder_compat_hook.so (android/host/adreno_binder_compat.c): an absent system API is optional, while an available one receives every argument, the length
included, unchanged, and is looked up once. Adapted from Banana Native f71720e0 (MIT, dm/tools/tests/test_android_binder_compat.py). The gate that loads the library
(hook_impl.cpp: a custom driver on API < 36 only) and the build target are checked by reading the sources."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'android/host/adreno_binder_compat.c'
HOOK_IMPL = ROOT / 'dm/third_party/libadrenotools/src/hook/hook_impl.cpp'
CMAKE = ROOT / 'android/host/CMakeLists.txt'
GRADLE = ROOT / 'android/app/build.gradle'


class BinderCompatWiring(unittest.TestCase):
    def test_loaded_for_custom_driver_below_api_36(self):
        src = HOOK_IMPL.read_text(encoding='utf-8')
        m = re.search(r'if \(\(hook_params->featureFlags & ADRENOTOOLS_DRIVER_CUSTOM\) && android_get_device_api_level\(\) < 36\) \{\s*'
                      r'if \(linkernsbypass_namespace_dlopen\("libbinder_compat_hook\.so", RTLD_GLOBAL, driverNs\)\)', src)
        self.assertIsNotNone(m)
        assert m is not None   # (for the type checker)
        # before the driver (and anything else) is loaded into the namespace
        self.assertLess(m.start(), src.index('auto newExtinfo{*extinfo};'))

    def test_built_and_packed(self):
        cmake = CMAKE.read_text(encoding='utf-8')
        self.assertIn('add_library(binder_compat_hook SHARED ${CMAKE_CURRENT_SOURCE_DIR}/adreno_binder_compat.c)', cmake)
        self.assertIn('target_link_options(binder_compat_hook PRIVATE -Wl,-z,global)', cmake)
        self.assertRegex(GRADLE.read_text(encoding='utf-8'), r"targets [^\n]*'binder_compat_hook'")


@unittest.skipUnless(shutil.which('clang'), 'no clang')
class BinderCompat(unittest.TestCase):
    def test_annotation(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / 'test.c'
            src.write_text(r'''
#include <assert.h>
#include <dlfcn.h>
#include <stddef.h>
#include <string.h>
static int available, opens, resolves, calls;
static void *seen_class;
static const char *const *seen_names;
static size_t seen_length;
static void annotation(void *clazz, const char *const *names, size_t length) {
    calls++; seen_class = clazz; seen_names = names; seen_length = length;
}
static void *probe_open(const char *name, int flags) {
    assert(!strcmp(name, "libbinder_ndk.so"));
    assert(flags == (RTLD_NOW | RTLD_LOCAL));
    opens++; return (void *)1;
}
static void *probe_symbol(void *lib, const char *name) {
    assert(lib == (void *)1);
    assert(!strcmp(name, "AIBinder_Class_setTransactionCodeToFunctionNameMap"));
    resolves++; return available ? (void *)annotation : NULL;
}
#define dlopen probe_open
#define dlsym probe_symbol
''' + '#include "' + SOURCE.as_posix() + '"\n' + r'''
int main(int argc, char **argv) {
    (void)argv;
    available = argc > 1;
    const char *names[] = {"first", NULL, "third"};
    AIBinder_Class *clazz = (AIBinder_Class *)(void *)names;
    AIBinder_Class_setTransactionCodeToFunctionNameMap(clazz, names, 3);
    AIBinder_Class_setTransactionCodeToFunctionNameMap(clazz, names, 1);
    assert(opens == 1 && resolves == 1);
    assert(calls == (available ? 2 : 0));
    if (available) assert(seen_class == clazz && seen_names == names && seen_length == 1);
    return 0;
}
''')
            exe = Path(tmp) / 'test'
            subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-pthread', str(src), '-o', str(exe)],
                           check=True, capture_output=True, text=True)
            for args in ([], ['available']):
                subprocess.run([str(exe), *args], check=True, capture_output=True, text=True)


if __name__ == '__main__':
    unittest.main()
