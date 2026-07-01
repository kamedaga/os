/*
 * Private LPR string substrate.
 *
 * These implementations are imported from musl upstream src/string and renamed
 * into the lpr_* namespace so the guest Linux process never sees runtime
 * support symbols. See musl/COPYRIGHT.
 */

#define weak_alias(old, new)

#define memcpy lpr_memcpy
#define memmove lpr_memmove
#define memset lpr_memset
#define memcmp lpr_memcmp
#define memchr lpr_memchr
#define strlen lpr_strlen
#define strnlen lpr_strnlen
#define strcmp lpr_strcmp
#define strncmp lpr_strncmp
#define strchr lpr_strchr
#define strrchr lpr_strrchr
#define __strchrnul lpr_strchrnul
#define strchrnul lpr_strchrnul_public_alias_suppressed
#define __memrchr lpr_memrchr
#define memrchr lpr_memrchr_public_alias_suppressed

#include "../../../../../musl/upstream/src/string/memcpy.c"
#undef LS
#undef RS

#include "../../../../../musl/upstream/src/string/memmove.c"
#undef WT
#undef WS

#include "../../../../../musl/upstream/src/string/memset.c"
#include "../../../../../musl/upstream/src/string/memcmp.c"

#include "../../../../../musl/upstream/src/string/memchr.c"
#undef SS
#undef ALIGN
#undef ONES
#undef HIGHS
#undef HASZERO

#include "../../../../../musl/upstream/src/string/strlen.c"
#undef ALIGN
#undef ONES
#undef HIGHS
#undef HASZERO

#include "../../../../../musl/upstream/src/string/strnlen.c"
#include "../../../../../musl/upstream/src/string/strcmp.c"
#include "../../../../../musl/upstream/src/string/strncmp.c"

#include "../../../../../musl/upstream/src/string/strchrnul.c"
#undef ALIGN
#undef ONES
#undef HIGHS
#undef HASZERO

#include "../../../../../musl/upstream/src/string/strchr.c"

#include "../../../../../musl/upstream/src/string/memrchr.c"
#include "../../../../../musl/upstream/src/string/strrchr.c"
