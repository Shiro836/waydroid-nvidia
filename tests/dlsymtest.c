#include <dlfcn.h>
#include <stdio.h>
int main(void) {
    void *h = dlopen(NULL, RTLD_NOW);
    printf("dlopen(NULL)=%p\n", h);
    const char *names[] = {"AHardwareBuffer_acquire", "AHardwareBuffer_describe", "AHardwareBuffer_release"};
    for (int i = 0; i < 3; i++) {
        void *p = dlsym(h, names[i]);
        printf("dlsym(main, %s) = %p (%s)\n", names[i], p, p ? "ok" : dlerror());
    }
    void *g = dlsym(RTLD_DEFAULT, "AHardwareBuffer_acquire");
    printf("dlsym(RTLD_DEFAULT, acquire) = %p\n", g);
    return 0;
}
