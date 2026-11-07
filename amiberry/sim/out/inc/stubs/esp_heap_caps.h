// esp_heap_caps.h - PS5 stand-in for the ESP32's memory allocator, as the
// Amiga emulator's board code (built with ARDUINO) uses it. Blocks come
// straight from the system (PlatformPS5.cpp: mmap), not from the app's small
// malloc heap.
#ifndef PS5_ESP_HEAP_CAPS_H
#define PS5_ESP_HEAP_CAPS_H
#include <stddef.h>
#define MALLOC_CAP_INTERNAL 0x1
#define MALLOC_CAP_SPIRAM 0x2
#define MALLOC_CAP_8BIT 0x4
#ifdef __cplusplus
extern "C" {
#endif
void *heap_caps_malloc(size_t n, unsigned caps);
void heap_caps_free(void *p);
size_t heap_caps_get_largest_free_block(unsigned caps);
#ifdef __cplusplus
}
#endif
#endif
