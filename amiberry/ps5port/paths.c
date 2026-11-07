/* paths.c - PS5 port: relative paths. The PS5 refuses them (stat gives
 * EINVAL), where Amiberry expects them to be found from its home folder
 * (its current folder elsewhere). The link wraps these calls (--wrap):
 * a relative path is taken from the app's storage, /download0/amiberry,
 * and stat's EINVAL becomes "not there", as std::filesystem expects. */
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>

struct stat;
typedef struct FILE FILE;
int __real_stat(const char *, struct stat *);
int __real_lstat(const char *, struct stat *);
int __real_open(const char *, int, ...);
FILE *__real_fopen(const char *, const char *);
int __real_mkdir(const char *, unsigned);

static const char *kBase = "/download0/amiberry/";

/* the path to use: as it is when absolute, else from kBase (in buf) */
static const char *fix(const char *p, char *buf, size_t n) {
  if (!p || p[0] == '/' || p[0] == 0) return p;
  while (p[0] == '.' && p[1] == '/') p += 2;
  size_t b = strlen(kBase), k = strlen(p);
  if (b + k + 1 > n) return p;
  memcpy(buf, kBase, b);
  memcpy(buf + b, p, k + 1);
  if (!strcmp(p, ".")) buf[b - 1] = 0;
  return buf;
}

int __wrap_stat(const char *p, struct stat *st) {
  char buf[1024];
  int r = __real_stat(fix(p, buf, sizeof buf), st);
  if (r != 0 && errno == EINVAL) errno = ENOENT;
  return r;
}
int __wrap_lstat(const char *p, struct stat *st) {
  char buf[1024];
  int r = __real_lstat(fix(p, buf, sizeof buf), st);
  if (r != 0 && errno == EINVAL) errno = ENOENT;
  return r;
}
int __wrap_open(const char *p, int flags, ...) {
  va_list a;
  va_start(a, flags);
  int mode = va_arg(a, int);
  va_end(a);
  char buf[1024];
  return __real_open(fix(p, buf, sizeof buf), flags, mode);
}
FILE *__wrap_fopen(const char *p, const char *m) {
  char buf[1024];
  return __real_fopen(fix(p, buf, sizeof buf), m);
}
int __wrap_mkdir(const char *p, unsigned mode) {
  char buf[1024];
  return __real_mkdir(fix(p, buf, sizeof buf), mode);
}
