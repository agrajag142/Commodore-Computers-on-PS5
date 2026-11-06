// ps5_sce.h
// The PS5 system functions the app uses (from the payload SDK's stub
// libraries). The PC simulator (c64/sim) has stand-ins for all of them.

#ifndef PS5_SCE_H
#define PS5_SCE_H

#include <stddef.h>
#include <stdint.h>

extern "C" {
size_t sceKernelGetDirectMemorySize();
int sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length,
                                  size_t alignment, int memory_type, int64_t *physical_address);
int sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags,
                             int64_t physical_address, size_t alignment);
int sceKernelSendNotificationRequest(uint32_t device, void *request, size_t size, int blocking);
int sceKernelUsleep(uint32_t microseconds);
int sceSystemServiceHideSplashScreen();
// "exit": closes the app (the kit's way; returning from main would crash)
int sceSystemServiceLoadExec(const char *path, const char **arguments);
int sceVideoOutOpen(int32_t user_id, int32_t bus_type, int32_t index, const void *param);
int sceVideoOutSetFlipRate(int32_t handle, int32_t rate);
int sceVideoOutSubmitFlip(int32_t handle, int32_t buffer_index, uint32_t flip_mode, int64_t flip_argument);
int sceVideoOutWaitVblank(int32_t handle);
void sceVideoOutSetBufferAttribute2(void *attribute, uint64_t pixel_format, uint32_t tiling_mode,
                                    uint32_t width, uint32_t height, uint64_t option,
                                    uint32_t dcc_control, uint64_t dcc_clear_color);
int sceVideoOutRegisterBuffers2(int32_t handle, int32_t set_index, int32_t buffer_index_start,
                                void *buffers, int32_t buffer_count, void *attribute,
                                int32_t category, void *option);
int sceUserServiceInitialize(const void *params);
int sceUserServiceGetInitialUser(int32_t *user_id);
int scePadInit();
int scePadOpen(int32_t user_id, int32_t type, int32_t index, const void *param);
int scePadGetHandle(int32_t user_id, int32_t type, int32_t index);
int scePadRead(int32_t handle, void *data, int32_t count);
// loading a system library at run time (the USB keyboard test)
int sceKernelLoadStartModule(const char *path, size_t argc, const void *argv, uint32_t flags, void *option,
                             int *result);
int sceKernelDlsym(int handle, const char *symbol, void **address);
int sceAudioOutInit();
int sceAudioOutOpen(int user, int type, int index, unsigned grain, unsigned rate, unsigned format);
int sceAudioOutOutput(int handle, const void *samples);
}

#endif // PS5_SCE_H
