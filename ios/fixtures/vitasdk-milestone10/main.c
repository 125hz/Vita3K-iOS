#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/types.h>

void _start(void) __attribute__((weak, alias("module_start")));

int module_start(SceSize args, void *argp) {
    (void)args;
    (void)argp;

    const int thread_id = sceKernelGetThreadId();
    sceKernelExitThread(thread_id & 0xFF);
    return 0;
}
