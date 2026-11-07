/* bigalloc.c - PS5 port: the PS5's malloc heap is small, and a large block
 * (Amiberry's 64 MB picture buffer) comes back NULL, which Amiberry doesn't
 * check: it then writes over the app's own code and the PS5 closes it.
 * The link wraps malloc, calloc, realloc and free: blocks of 4 MB and more
 * are mapped straight from the system (as the Amiga's memory already is),
 * and kept in a small table so free and realloc know them. */
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <malloc_np.h>

void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);

void ps5portLogLine(const char *line);

#define BIG ((size_t)4 << 20)
#define SLOTS 64
static struct { void *p; size_t size; } big[SLOTS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int bigCount; /* (for free: no lock when there are none) */

static void logBig(const char *what, size_t size, void *p) {
  char line[96];
  static const char hex[] = "0123456789abcdef";
  /* (no snprintf: this runs inside malloc) */
  int n = 0;
  for (const char *s = what; *s && n < 40; s++) line[n++] = *s;
  line[n++] = ' ';
  for (int i = 60; i >= 0; i -= 4) line[n++] = hex[(size >> i) & 15];
  line[n++] = ' ';
  for (int i = 60; i >= 0; i -= 4) line[n++] = hex[((uintptr_t)p >> i) & 15];
  line[n++] = '\n';
  line[n] = 0;
  ps5portLogLine(line);
}

static void *bigMap(size_t size) {
  size_t len = (size + 16383) & ~(size_t)16383;
  void *p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (p == MAP_FAILED) {
    logBig("PS5: big block: no memory for", size, 0);
    errno = ENOMEM;
    return 0;
  }
  pthread_mutex_lock(&lock);
  int i;
  for (i = 0; i < SLOTS && big[i].p; i++) {
  }
  if (i == SLOTS) {
    pthread_mutex_unlock(&lock);
    munmap(p, len);
    logBig("PS5: big block: table full, size", size, 0);
    errno = ENOMEM;
    return 0;
  }
  big[i].p = p;
  big[i].size = len;
  bigCount++;
  pthread_mutex_unlock(&lock);
  logBig("PS5: big block", size, p);
  return p;
}

/* the block's mapped size, removed from the table if take; 0: not one of ours */
static size_t bigFind(void *p, int take) {
  if (!bigCount) return 0;
  size_t size = 0;
  pthread_mutex_lock(&lock);
  for (int i = 0; i < SLOTS; i++)
    if (big[i].p == p) {
      size = big[i].size;
      if (take) {
        big[i].p = 0;
        bigCount--;
      }
      break;
    }
  pthread_mutex_unlock(&lock);
  return size;
}

void *__wrap_malloc(size_t size) {
  if (size >= BIG) return bigMap(size);
  return __real_malloc(size);
}

void *__wrap_calloc(size_t n, size_t size) {
  if (size && n > (size_t)-1 / size) {
    errno = ENOMEM;
    return 0;
  }
  if (n * size >= BIG) return bigMap(n * size); /* (fresh mappings are zero) */
  return __real_calloc(n, size);
}

void __wrap_free(void *p) {
  if (!p) return;
  size_t size = bigFind(p, 1);
  if (size) {
    munmap(p, size);
    return;
  }
  __real_free(p);
}

void *__wrap_realloc(void *p, size_t size) {
  if (!p) return __wrap_malloc(size);
  size_t old = bigFind(p, 0);
  if (old) {
    if (size <= old && size >= BIG / 2) return p;
    void *q = size >= BIG ? bigMap(size) : __real_malloc(size);
    if (!q) return 0;
    memcpy(q, p, size < old ? size : old);
    __wrap_free(p);
    return q;
  }
  if (size >= BIG) {
    /* (a small block grown big: into a mapping, as much as the heap
     * says the old block holds) */
    void *q = bigMap(size);
    if (!q) return 0;
    size_t had = malloc_usable_size(p);
    memcpy(q, p, had < size ? had : size);
    __real_free(p);
    return q;
  }
  return __real_realloc(p, size);
}
