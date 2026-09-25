#pragma once
#include <stdint.h>
int64_t lpr_linux_setreuid(uint64_t, uint64_t);
int64_t lpr_linux_setregid(uint64_t, uint64_t);

int64_t lpr_linux_getuid(void);
int64_t lpr_linux_geteuid(void);
int64_t lpr_linux_getgid(void);
int64_t lpr_linux_getegid(void);
int64_t lpr_linux_getgroups(uint64_t count, uint64_t groups);
int64_t lpr_linux_setgroups(uint64_t count, uint64_t groups);
int64_t lpr_linux_getresuid(uint64_t real, uint64_t effective, uint64_t saved);
int64_t lpr_linux_getresgid(uint64_t real, uint64_t effective, uint64_t saved);
/* All identity transitions are validated by the authenticated supervisor. */
int64_t lpr_linux_setuid(uint64_t id);
int64_t lpr_linux_setgid(uint64_t id);
int64_t lpr_linux_setresuid(uint64_t real, uint64_t effective, uint64_t saved);
int64_t lpr_linux_setresgid(uint64_t real, uint64_t effective, uint64_t saved);
