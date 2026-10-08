/* SPDX-License-Identifier: MIT */
#include <pacha/syscall.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

/* Probe runs lost stdio diagnostics even though the exec router grants log
 * streams. Mirror the existing loader's diagnostics through LOG to separate
 * that transport failure from the module rejection. This file is linked only
 * with TERMD_MODULE_DIAGNOSTICS, preserving normal builds and stdio results. */
int __wrap_fprintf(FILE *stream, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (stream == stderr) {
        char line[512];
        va_list copy;
        va_copy(copy, args);
        const int length = vsnprintf(line, sizeof(line), format, copy);
        va_end(copy);
        if (length > 0) {
            const size_t count = (size_t)length < sizeof(line)
                ? (size_t)length : sizeof(line) - 1;
            (void)pacha_syscall2(1, (uint64_t)(uintptr_t)line, count);
        }
    }
    const int result = vfprintf(stream, format, args);
    va_end(args);
    return result;
}
