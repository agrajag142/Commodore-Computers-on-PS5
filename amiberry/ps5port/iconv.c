/* iconv.c - PS5 port: UTF-8 <-> ISO-8859-1 only (see iconv.h) */
#include "iconv.h"
#include <errno.h>
#include <string.h>
#include <strings.h>

static int isUtf8(const char *c) { return strncasecmp(c, "UTF-8", 5) == 0 || strcasecmp(c, "UTF8") == 0; }
static int isLatin1(const char *c) {
  return strcasecmp(c, "ISO-8859-1") == 0 || strcasecmp(c, "LATIN1") == 0 || strcasecmp(c, "ISO8859-1") == 0 ||
         strncasecmp(c, "ISO-8859-1//", 12) == 0;
}

/* handles: 1 UTF-8 -> Latin-1, 2 Latin-1 -> UTF-8 */
iconv_t iconv_open(const char *to, const char *from) {
  if (isLatin1(to) && isUtf8(from)) return (iconv_t)1;
  if (isUtf8(to) && isLatin1(from)) return (iconv_t)2;
  errno = EINVAL;
  return (iconv_t)-1;
}

int iconv_close(iconv_t cd) {
  (void)cd;
  return 0;
}

size_t iconv(iconv_t cd, char **inbuf, size_t *inleft, char **outbuf, size_t *outleft) {
  if (!inbuf || !*inbuf) return 0; /* reset */
  unsigned char *in = (unsigned char *)*inbuf;
  size_t n = *inleft, done = 0;
  while (n) {
    unsigned c = in[0], len = 1;
    if (cd == (iconv_t)1) {
      if (c >= 0x80) {
        if ((c & 0xe0) == 0xc0 && n >= 2) c = ((c & 0x1f) << 6) | (in[1] & 0x3f), len = 2;
        else if ((c & 0xf0) == 0xe0 && n >= 3) c = '?', len = 3;
        else if ((c & 0xf8) == 0xf0 && n >= 4) c = '?', len = 4;
        else { errno = n < 4 ? EINVAL : EILSEQ; break; }
        if (c > 0xff) c = '?';
      }
      if (*outleft < 1) { errno = E2BIG; break; }
      *(*outbuf)++ = (char)c;
      (*outleft)--;
    } else {
      unsigned need = c < 0x80 ? 1 : 2;
      if (*outleft < need) { errno = E2BIG; break; }
      if (need == 1) *(*outbuf)++ = (char)c;
      else { *(*outbuf)++ = (char)(0xc0 | (c >> 6)); *(*outbuf)++ = (char)(0x80 | (c & 0x3f)); }
      *outleft -= need;
    }
    in += len;
    n -= len;
    done++;
  }
  *inbuf = (char *)in;
  *inleft = n;
  return n ? (size_t)-1 : 0;
  (void)done;
}
