#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>

void *FcPatternCreate(void);
void *FcPatternDuplicate(const void *);
void FcPatternDestroy(void *);
int FcPatternAddString(void *, const char *, const unsigned char *);
int FcPatternAddLangSet(void *, const char *, const void *);
void *FcFontSetCreate(void);
void FcFontSetDestroy(void *);
int FcFontSetAdd(void *, void *);
void *FcLangSetCreate(void);
void FcLangSetDestroy(void *);
int FcLangSetAdd(void *, const unsigned char *);
void FcDefaultSubstitute(void *);
int FcConfigSubstitute(void *, void *, int);
void *FcPatternFilter(void *, const void *);
int FcPatternRemove(void *, const char *, int);

static void *const token = (void *)0x1234;
#ifdef FONT_PROVIDER
static void entered(void) { assert(errno == EDOM); errno = ERANGE; }
void *FcPatternCreate(void) { entered(); return token; }
void *FcPatternDuplicate(const void *p) { assert(p == token); entered(); return token; }
void FcPatternDestroy(void *p) { assert(p == token); entered(); }
int FcPatternAddString(void *p, const char *key, const unsigned char *s)
{ assert(p == token && key[0] == 'k' && s[0] == 'v'); entered(); return 1; }
int FcPatternAddLangSet(void *p, const char *key, const void *ls)
{ assert(p == token && key[0] == 'k' && ls == token); entered(); return 1; }
void *FcFontSetCreate(void) { entered(); return token; }
void FcFontSetDestroy(void *s)
{
    assert(s == token); entered();
    errno = EDOM;
    FcPatternDestroy(s); /* Nested wrapper accounting must remain separate. */
}
int FcFontSetAdd(void *s, void *p) { assert(s == token && p == token); entered(); return 1; }
void *FcLangSetCreate(void) { entered(); return token; }
void FcLangSetDestroy(void *ls) { assert(ls == token); entered(); }
int FcLangSetAdd(void *ls, const unsigned char *s)
{ assert(ls == token && s[0] == 'v'); entered(); return 1; }
void FcDefaultSubstitute(void *p) { assert(p == token); entered(); }
int FcConfigSubstitute(void *c, void *p, int kind)
{ assert(c == token && p == token && kind == 2); entered(); return 1; }
void *FcPatternFilter(void *p, const void *objects)
{ assert(p == token && objects == token); entered(); return token; }
int FcPatternRemove(void *p, const char *object, int id)
{ assert(p == token && object[0] == 'k' && id == 1); entered(); return 1; }
#else
#define CHECK(expr) do { errno = EDOM; expr; assert(errno == ERANGE); } while (0)
int main(void)
{
    CHECK(assert(FcPatternCreate() == token));
    CHECK(assert(FcPatternDuplicate(token) == token));
    CHECK(FcPatternDestroy(token));
    CHECK(assert(FcPatternAddString(token, "key", (const unsigned char *)"value") == 1));
    CHECK(assert(FcPatternAddLangSet(token, "key", token) == 1));
    CHECK(assert(FcFontSetCreate() == token));
    CHECK(FcFontSetDestroy(token));
    CHECK(assert(FcFontSetAdd(token, token) == 1));
    CHECK(assert(FcLangSetCreate() == token));
    CHECK(FcLangSetDestroy(token));
    CHECK(assert(FcLangSetAdd(token, (const unsigned char *)"value") == 1));
    CHECK(FcDefaultSubstitute(token));
    CHECK(assert(FcConfigSubstitute(token, token, 2) == 1));
    CHECK(assert(FcPatternFilter(token, token) == token));
    for (unsigned i = 0; i < 256; ++i)
        CHECK(assert(FcPatternRemove(token, "key", 1) == 1));
    /* Trigger the existing bounded summary after the calls, not during them. */
    void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(page != MAP_FAILED && munmap(page, 4096) == 0);
    puts("font stage probe arguments, results, errno and nesting: PASS");
}
#endif
