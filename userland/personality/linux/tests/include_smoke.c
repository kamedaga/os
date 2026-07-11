#include <personality/personality_abi.h>
#include <personality/runtime_page.h>
#include <personality/lpr_image_abi.h>
#include <personality/zpoline.h>
#include <personality/coordinator_protocol.h>
#include <personality/linux_lpr.h>

int main(void) {
    struct lpr_runtime_page runtime;
    struct personality_trap_frame trap;
    struct lpr_patch_mapping_request patch;
    struct lpr_coordinator_request request;
    runtime.magic = LPR_RUNTIME_MAGIC;
    trap.magic = PERSONALITY_TRAP_FRAME_MAGIC;
    patch.start_va = LPR_ZPOLINE_PAGE_VA;
    request.magic = PACHA_SERVICE_REQUEST_MAGIC;
    (void)LPR_LINUX_SYS_WRITE;
    return runtime.magic == 0 || trap.magic == 0 || request.magic == 0;
}
