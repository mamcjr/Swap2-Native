// Loading the built host runs the real NVN registry constructors before any game or save is opened.
// This catches imported no-op registrations colliding with local implementations.
#include <dlfcn.h>
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "host load failed: %s\n", dlerror()); return 1; }
    puts("host load: constructors and NVN registry initialized successfully");
    // Process exit owns teardown; this test must not invoke game lifecycle methods.
    return 0;
}
