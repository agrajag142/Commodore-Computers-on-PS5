/* threads.c - PS5 port: a PS5 thread made without a stack size gets a small
 * stack (the app's own threads ask for theirs), too small for Amiberry's
 * drawing and other threads; one that overflows takes the app down without
 * a word. The link wraps pthread_create: 4 MB unless asked otherwise. */
#include <pthread.h>
#include <stddef.h>

int __real_pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);

void ps5portLogThread(void *fn, int result);

int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
  pthread_attr_t own;
  size_t size = 0;
  if (attr) {
    pthread_attr_getstacksize(attr, &size);
    if (size >= (size_t)1 << 20) {
      int r = __real_pthread_create(t, attr, fn, arg);
      ps5portLogThread(fn, r);
      return r;
    }
    own = *attr;
  } else {
    pthread_attr_init(&own);
  }
  pthread_attr_setstacksize(&own, (size_t)4 << 20);
  int r = __real_pthread_create(t, &own, fn, arg);
  ps5portLogThread(fn, r);
  if (!attr) pthread_attr_destroy(&own);
  return r;
}
