/* libc_extra.c - PS5 port: what Amiberry and the SDK's libc++ want from a C
 * library and the PS5's (libSceLibcInternal, libkernel) doesn't have.
 *
 *   locales     libc++ asks for FreeBSD's per-locale functions (strtod_l and
 *               so on); there is one locale here, "C": each calls the plain one
 *   emutls      thread-local variables (the SDK builds with -femulated-tls):
 *               compiler-rt's __emutls_get_address, on pthread keys
 *   the rest    host-shell, networking-database and message-catalogue calls
 *               that Amiberry has but doesn't need here: they fail politely
 *
 * Declarations are written out by hand (not from the SDK's headers, which
 * declare some of these with other types).
 */

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

typedef void *loc_t;
typedef int wint;     /* wint_t */
typedef int wchr;     /* wchar_t on the PS5: 4 bytes */
typedef struct { char opaque[128]; } mbstate; /* (only passed through) */

/* the plain functions, from the PS5's C library */
double strtod(const char *, char **);
float strtof(const char *, char **);
long double strtold(const char *, char **);
long long strtoll(const char *, char **, int);
unsigned long long strtoull(const char *, char **, int);
size_t strftime(char *, size_t, const char *, const struct tm *);
int strcoll(const char *, const char *);
size_t strxfrm(char *, const char *, size_t);
int wcscoll(const wchr *, const wchr *);
size_t wcsxfrm(wchr *, const wchr *, size_t);
int iswctype(wint, unsigned long);
wint btowc(int);
int wctob(wint);
size_t mbrlen(const char *, size_t, mbstate *);
size_t mbrtowc(wchr *, const char *, size_t, mbstate *);
int mbtowc(wchr *, const char *, size_t);
size_t mbsrtowcs(wchr *, const char **, size_t, mbstate *);
size_t wcrtomb(char *, wchr, mbstate *);
struct lconv *localeconv(void);
int vsnprintf(char *, size_t, const char *, va_list);
int vsscanf(const char *, const char *, va_list);
int tolower(int);
int toupper(int);
void *malloc(size_t);
void free(void *);
void *calloc(size_t, size_t);
void abort(void);
int printf(const char *, ...);
struct tm *gmtime(const time_t *);
struct tm *localtime(const time_t *);

/* ---------------------------------------------------------------- locales */
static int theLocale;
loc_t newlocale(int mask, const char *name, loc_t base) {
  (void)mask; (void)name; (void)base;
  return &theLocale;
}
void freelocale(loc_t l) { (void)l; }
struct lconv *localeconv_l(loc_t l) { (void)l; return localeconv(); }

double strtod_l(const char *s, char **e, loc_t l) { (void)l; return strtod(s, e); }
float strtof_l(const char *s, char **e, loc_t l) { (void)l; return strtof(s, e); }
long double strtold_l(const char *s, char **e, loc_t l) { (void)l; return strtold(s, e); }
long long strtoll_l(const char *s, char **e, int b, loc_t l) { (void)l; return strtoll(s, e, b); }
unsigned long long strtoull_l(const char *s, char **e, int b, loc_t l) { (void)l; return strtoull(s, e, b); }
size_t strftime_l(char *s, size_t n, const char *f, const struct tm *t, locale_t l) { (void)l; return strftime(s, n, f, t); }
int strcoll_l(const char *a, const char *b, locale_t l) { (void)l; return strcoll(a, b); }
size_t strxfrm_l(char *d, const char *s, size_t n, locale_t l) { (void)l; return strxfrm(d, s, n); }
int wcscoll_l(const wchr *a, const wchr *b, loc_t l) { (void)l; return wcscoll(a, b); }
size_t wcsxfrm_l(wchr *d, const wchr *s, size_t n, loc_t l) { (void)l; return wcsxfrm(d, s, n); }
int iswctype_l(wint c, unsigned long t, loc_t l) { (void)l; return iswctype(c, t); }
wint btowc_l(int c, loc_t l) { (void)l; return btowc(c); }
int wctob_l(wint c, loc_t l) { (void)l; return wctob(c); }
size_t mbrlen_l(const char *s, size_t n, mbstate *ps, loc_t l) { (void)l; return mbrlen(s, n, ps); }
size_t mbrtowc_l(wchr *w, const char *s, size_t n, mbstate *ps, loc_t l) { (void)l; return mbrtowc(w, s, n, ps); }
int mbtowc_l(wchr *w, const char *s, size_t n, loc_t l) { (void)l; return mbtowc(w, s, n); }
size_t mbsrtowcs_l(wchr *d, const char **s, size_t n, mbstate *ps, loc_t l) { (void)l; return mbsrtowcs(d, s, n, ps); }
size_t wcrtomb_l(char *s, wchr w, mbstate *ps, loc_t l) { (void)l; return wcrtomb(s, w, ps); }

/* (not in the PS5's C library at all: on mbrtowc / wcrtomb) */
size_t mbsnrtowcs(wchr *dst, const char **src, size_t nms, size_t len, mbstate *ps) {
  const char *s = *src;
  size_t n = 0;
  while ((!dst || n < len) && nms > 0) {
    wchr w;
    size_t r = mbrtowc(&w, s, nms, ps);
    if (r == (size_t)-1) { *src = s; return (size_t)-1; }
    if (r == (size_t)-2) break;
    if (r == 0) { if (dst) { dst[n] = 0; *src = NULL; } return n; }
    if (dst) dst[n] = w;
    s += r;
    nms -= r;
    n++;
  }
  if (dst) *src = s;
  return n;
}
size_t wcsnrtombs(char *dst, const wchr **src, size_t nwc, size_t len, mbstate *ps) {
  const wchr *s = *src;
  size_t n = 0;
  char buf[8];
  while (nwc > 0) {
    size_t r = wcrtomb(buf, *s, ps);
    if (r == (size_t)-1) { if (dst) *src = s; return (size_t)-1; }
    if (dst) {
      if (n + r > len) break;
      memcpy(dst + n, buf, r);
    }
    if (*s == 0) { if (dst) *src = NULL; return n + r - 1; }
    n += r;
    s++;
    nwc--;
  }
  if (dst) *src = s;
  return n;
}
size_t mbsnrtowcs_l(wchr *d, const char **s, size_t nms, size_t n, mbstate *ps, loc_t l) {
  (void)l;
  return mbsnrtowcs(d, s, nms, n, ps);
}
size_t wcsnrtombs_l(char *d, const wchr **s, size_t nwc, size_t n, mbstate *ps, loc_t l) {
  (void)l;
  return wcsnrtombs(d, s, nwc, n, ps);
}

int snprintf_l(char *s, size_t n, loc_t l, const char *f, ...) {
  (void)l;
  va_list a;
  va_start(a, f);
  int r = vsnprintf(s, n, f, a);
  va_end(a);
  return r;
}
int asprintf_l(char **out, loc_t l, const char *f, ...) {
  (void)l;
  va_list a, b;
  va_start(a, f);
  va_copy(b, a);
  int n = vsnprintf(NULL, 0, f, a);
  va_end(a);
  *out = n >= 0 ? (char *)malloc((size_t)n + 1) : NULL;
  if (*out) vsnprintf(*out, (size_t)n + 1, f, b);
  va_end(b);
  return *out ? n : -1;
}
int sscanf_l(const char *s, loc_t l, const char *f, ...) {
  (void)l;
  va_list a;
  va_start(a, f);
  int r = vsscanf(s, f, a);
  va_end(a);
  return r;
}

/* FreeBSD's ctype internals (for characters past the 256 in its table) */
extern char _DefaultRuneLocale[];
void *__runes_for_locale(loc_t l, int *mbCurMax) {
  (void)l;
  if (mbCurMax) *mbCurMax = 1;
  return _DefaultRuneLocale;
}
unsigned long ___runetype_l(int c, loc_t l) { (void)c; (void)l; return 0; }
int ___tolower_l(int c, loc_t l) { (void)l; return c < 256 ? tolower(c) : c; }
int ___toupper_l(int c, loc_t l) { (void)l; return c < 256 ? toupper(c) : c; }
int ___mb_cur_max_l(loc_t l) { (void)l; return 1; }

/* ---------------------------------------------------------------- emutls */
/* compiler-rt's layout: each thread-local variable has one of these */
typedef struct {
  size_t size, align;
  union { uintptr_t index; void *address; } object;
  void *value; /* its first value, or NULL for zeros */
} emutls_control;

int pthread_key_create(unsigned *, void (*)(void *));
void *pthread_getspecific(unsigned);
int pthread_setspecific(unsigned, const void *);
int pthread_mutex_lock(void *);
int pthread_mutex_unlock(void *);

typedef struct { size_t n; void *slot[]; } emutls_array;
static unsigned emutlsKey;
static volatile int emutlsKeyReady;
static volatile uintptr_t emutlsNext;
static volatile int emutlsLock;

static void emutlsLockOn(void) { while (__sync_lock_test_and_set(&emutlsLock, 1)) {} }
static void emutlsLockOff(void) { __sync_lock_release(&emutlsLock); }

static void emutlsFree(void *p) {
  emutls_array *a = (emutls_array *)p;
  if (!a) return;
  for (size_t i = 0; i < a->n; i++)
    if (a->slot[i]) free(((void **)a->slot[i])[-1]);
  free(a);
}

void *__emutls_get_address(emutls_control *c) {
  uintptr_t index = __atomic_load_n(&c->object.index, __ATOMIC_ACQUIRE);
  if (!index) {
    emutlsLockOn();
    if (!emutlsKeyReady) {
      pthread_key_create(&emutlsKey, emutlsFree);
      emutlsKeyReady = 1;
    }
    index = c->object.index;
    if (!index) {
      index = ++emutlsNext;
      __atomic_store_n(&c->object.index, index, __ATOMIC_RELEASE);
    }
    emutlsLockOff();
  }
  emutls_array *a = (emutls_array *)pthread_getspecific(emutlsKey);
  if (!a || a->n < index) {
    size_t n = index + 16;
    emutls_array *b = (emutls_array *)calloc(1, sizeof(emutls_array) + n * sizeof(void *));
    if (!b) abort();
    if (a) {
      memcpy(b->slot, a->slot, a->n * sizeof(void *));
      free(a);
    }
    b->n = n;
    a = b;
    pthread_setspecific(emutlsKey, a);
  }
  void *v = a->slot[index - 1];
  if (!v) {
    size_t align = c->align < sizeof(void *) ? sizeof(void *) : c->align;
    char *raw = (char *)malloc(c->size + align + sizeof(void *));
    if (!raw) abort();
    char *p = (char *)(((uintptr_t)raw + sizeof(void *) + align - 1) & ~(uintptr_t)(align - 1));
    ((void **)p)[-1] = raw;
    if (c->value) memcpy(p, c->value, c->size);
    else memset(p, 0, c->size);
    a->slot[index - 1] = v = p;
  }
  return v;
}

/* ---------------------------------------------------------------- the rest */
void __assert(const char *func, const char *file, int line, const char *expr) {
  printf("assertion failed: %s (%s, %s:%d)\n", expr, func ? func : "?", file, line);
  abort();
}

static int hErrno;
int *__h_errno(void) { return &hErrno; }
void *gethostbyaddr(const void *a, unsigned len, int type) { (void)a; (void)len; (void)type; hErrno = 1; return NULL; }
void *getprotobyname(const char *n) { (void)n; return NULL; }
void *getprotobynumber(int n) { (void)n; return NULL; }
void *getservbyport(int p, const char *proto) { (void)p; (void)proto; return NULL; }

int forkpty(int *m, char *n, const void *t, const void *w) {
  (void)m; (void)n; (void)t; (void)w;
  errno = ENOSYS;
  return -1;
}
int execl(const char *p, const char *a, ...) {
  (void)p; (void)a;
  errno = ENOSYS;
  return -1;
}
void *popen(const char *c, const char *m) { (void)c; (void)m; errno = ENOSYS; return NULL; }
int pclose(void *f) { (void)f; errno = ENOSYS; return -1; }
char *tmpnam(char *s) {
  static char own[64];
  static unsigned n;
  char *out = s ? s : own;
  /* (the app's own storage: the one place it can write) */
  const char *dir = "/download0/amiberry/tmp";
  size_t k = strlen(dir);
  memcpy(out, dir, k);
  unsigned v = ++n;
  for (int i = 0; i < 6; i++) out[k + i] = (char)('0' + (v >> (i * 3) & 7));
  out[k + 6] = 0;
  return out;
}

int utimensat(int fd, const char *p, const void *t, int flags) { (void)fd; (void)p; (void)t; (void)flags; return 0; }

void *catopen(const char *n, int f) { (void)n; (void)f; return (void *)-1; }
char *catgets(void *c, int set, int num, const char *s) { (void)c; (void)set; (void)num; return (char *)s; }
int catclose(void *c) { (void)c; return 0; }

struct tm *gmtime_r(const time_t *t, struct tm *out) {
  struct tm *g = gmtime(t);
  if (!g) return NULL;
  *out = *g;
  return out;
}
struct tm *localtime_r(const time_t *t, struct tm *out) {
  struct tm *g = localtime(t);
  if (!g) return NULL;
  *out = *g;
  return out;
}
time_t timegm(struct tm *t) {
  /* days from 1970-01-01 to the civil date (Howard Hinnant's algorithm) */
  long y = t->tm_year + 1900, m = t->tm_mon + 1;
  y += (m - 1) / 12;
  m = (m - 1) % 12 + 1;
  if (m <= 0) m += 12, y--;
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  long yoe = y - era * 400;
  long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t->tm_mday - 1;
  long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097 + doe - 719468;
  return (time_t)(days * 86400 + t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec);
}
