#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/types.h>

int _start(SceSize args, void *argp) __attribute__((weak, alias("module_start")));

int module_start(SceSize args, void *argp) {
    (void)args;
    (void)argp;

    return sceKernelGetThreadId() & 0xFF;
}
