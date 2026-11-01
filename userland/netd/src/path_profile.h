#pragma once
#include <stddef.h>
#include <stdint.h>
#ifndef NETD_PATH_PROFILE
#define NETD_PATH_PROFILE 0
#endif
#if NETD_PATH_PROFILE
#include "boot/net_perf.h"
int netd_path_control(const char *operation, char *reply, size_t capacity);
void netd_path_record(unsigned stage, uint64_t key, uint64_t begin, uint64_t end, uint64_t detail);
#define NETD_PATH_NOW() kobox_net_perf_tsc()
#define NETD_PATH_RECORD(s,k,b,e,d) netd_path_record(s,k,b,e,d)
#else
#define NETD_PATH_NOW() UINT64_C(0)
#define NETD_PATH_RECORD(s,k,b,e,d) ((void)(b),(void)(e))
#endif
