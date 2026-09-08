#ifndef LPR_GUI_DETAIL_H
#define LPR_GUI_DETAIL_H
#if defined(LPR_GUI_PROFILE) && LPR_GUI_PROFILE
#include <stdint.h>
int lpr_gui_profile_current_thread(void);
void lpr_gui_profile_span(unsigned kind, uint64_t start, uint64_t end);
void lpr_gui_profile_file(uint64_t nr, uint64_t argument, uint64_t flags,
    uint64_t offset_valid, uint64_t pread_active, uint64_t size, const char *path);
enum {
    LPR_GUI_POLL_SCAN = 11, LPR_GUI_POLL_GRAPH, LPR_GUI_POLL_BLOCK,
    LPR_GUI_WAIT_NATIVE, LPR_GUI_WAIT_DRAIN,
};
void lpr_gui_profile_peer(uint64_t fd, uint64_t handle, const char *path, unsigned length);
void lpr_gui_profile_wait(uint64_t start, uint64_t end, uint64_t fd,
    uint64_t events, uint64_t leaves, uint64_t ready, uint64_t timeout,
    int64_t status, uint64_t handle, uint64_t type);
#endif
#endif
