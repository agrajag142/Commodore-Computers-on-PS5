/* iconv.h - PS5 port: the PS5 has no iconv. Just what Amiberry uses: UTF-8
 * to and from ISO-8859-1 (Latin-1), the Amiga's character set. */
#ifndef PS5PORT_ICONV_H
#define PS5PORT_ICONV_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void *iconv_t;
iconv_t iconv_open(const char *tocode, const char *fromcode);
size_t iconv(iconv_t cd, char **inbuf, size_t *inleft, char **outbuf, size_t *outleft);
int iconv_close(iconv_t cd);
#ifdef __cplusplus
}
#endif
#endif
