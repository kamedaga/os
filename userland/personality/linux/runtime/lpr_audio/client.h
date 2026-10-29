#pragma once
#include <stdint.h>
#include <pacha/abi.h>
int64_t lpr_audio_install(pacha_fd_t native, uint64_t flags);
int64_t lpr_audio_write(uint64_t fd, uint64_t buffer, uint64_t count);
int64_t lpr_audio_ioctl(uint64_t fd, uint64_t command, uint64_t argument);
int64_t lpr_audio_poll_events(uint64_t fd, uint32_t events);
int64_t lpr_audio_status_flags(uint64_t fd, uint64_t flags);
