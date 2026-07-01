#ifndef LPR_SUPPORT_STRING_H
#define LPR_SUPPORT_STRING_H

#include <stddef.h>

void *lpr_memcpy(void *dst, const void *src, size_t n);
void *lpr_memmove(void *dst, const void *src, size_t n);
void *lpr_memset(void *dst, int c, size_t n);
int lpr_memcmp(const void *a, const void *b, size_t n);
void *lpr_memchr(const void *src, int c, size_t n);
size_t lpr_strlen(const char *s);
size_t lpr_strnlen(const char *s, size_t n);
int lpr_strcmp(const char *a, const char *b);
int lpr_strncmp(const char *a, const char *b, size_t n);
char *lpr_strchr(const char *s, int c);
char *lpr_strrchr(const char *s, int c);

#endif
