/* threads.c - PS5 port: a PS5 thread made without a stack size gets a small
 * stack (the app's own threads ask for theirs), too small for Amiberry's
 * drawing and other threads; one that overflows takes the app down without
 * a word. The link wraps pthread_create: 4 MB unless asked otherwise. */
#include <pthread.h>
#include <stddef.h>

int __real_pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);

void ps5portLogThread(void *fn, int result);

// (each thread started through here gets its own signal stack, so a crash
// in it, even a stack overflow, is reported; and a mark in the trace)
#include <signal.h>
#include <stdlib.h>
void ps5portTraceThread(void);
struct start { void *(*fn)(void *); void *arg; };
static void *threadStart(void *p) {
  struct start s = *(struct start *)p;
  free(p);
  stack_t ss = {0};
  ss.ss_size = 65536;
  ss.ss_sp = malloc(ss.ss_size);
  if (ss.ss_sp) sigaltstack(&ss, 0);
  ps5portTraceThread();
  return s.fn(s.arg);
}

int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn0)(void *), void *arg0) {
  struct start *st = malloc(sizeof *st);
  if (!st) return 12;
  st->fn = fn0;
  st->arg = arg0;
  void *(*fn)(void *) = threadStart;
  void *arg = st;
  pthread_attr_t own;
  size_t size = 0;
  if (attr) {
    pthread_attr_getstacksize(attr, &size);
    if (size >= (size_t)1 << 20) {
      int r = __real_pthread_create(t, attr, fn, arg);
      ps5portLogThread(fn0, r);
      return r;
    }
    own = *attr;
  } else {
    pthread_attr_init(&own);
  }
  pthread_attr_setstacksize(&own, (size_t)4 << 20);
  int r = __real_pthread_create(t, &own, fn, arg);
  ps5portLogThread(fn0, r);
  if (!attr) pthread_attr_destroy(&own);
  return r;
}
