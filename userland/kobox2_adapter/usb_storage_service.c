/* SPDX-License-Identifier: MIT */
#include "usb_storage_service.h"
#include "boot/module_launch.h"

#include <errno.h>
#include <string.h>

static struct ph_lifecycle_service *selected(struct ph_usb_storage_service *state) {
    return state->active == 1 ? &state->input_ops :
        state->active == 2 ? &state->block_ops : NULL;
}

static int prepare(void *context, struct ph_ipc_packet *packet) {
    struct ph_usb_storage_service *state = context;
    struct ph_lifecycle_service *ops;
    if (state->active) return -EPROTO;
    if (packet->operation >= PH_USB_INPUT_ATTACH &&
        packet->operation <= PH_USB_INPUT_READ) state->active = 1;
    else if (packet->operation >= PH_BLOCK_ATTACH &&
             packet->operation <= PH_BLOCK_FLUSH) state->active = 2;
    else return -EPROTO;
    ops = selected(state);
    int result = ops->prepare(ops->context, packet);
    if (result) state->active = 0;
    return result;
}

static int dispatch(void *context, void *linux_service) {
    struct ph_usb_storage_service *state = context;
    struct kobox_linux_io_service *io = linux_service;
    struct ph_lifecycle_service *ops = selected(state);
    if (!ops || !io) return -EPROTO;
    return ops->dispatch(ops->context, state->active == 1 ? (void *)io->input : io);
}

static int complete(void *context, struct ph_ipc *ipc) {
    struct ph_lifecycle_service *ops = selected(context);
    return ops ? ops->complete(ops->context, ipc) : -EPROTO;
}

static int release(void *context) {
    struct ph_usb_storage_service *state = context;
    struct ph_lifecycle_service *ops = selected(state);
    if (!ops) return -EPROTO;
    int result = ops->release(ops->context);
    state->active = 0;
    return result;
}

static int stop(void *context) {
    struct ph_usb_storage_service *state = context;
    int input = state->input_ops.stop(state->input_ops.context);
    int block = state->block_ops.stop(state->block_ops.context);
    return input ? input : block;
}

int ph_usb_storage_service_init(struct ph_usb_storage_service *state,
    void *core, uint64_t generation, struct ph_lifecycle_service *service) {
    if (!state || !service) return -EINVAL;
    memset(state, 0, sizeof(*state));
    int result = ph_usb_input_service_init(&state->input, core,
        generation, &state->input_ops);
    if (!result)
        result = ph_block_service_init(&state->block, core,
            generation, &state->block_ops);
    if (result) {
        if (state->input_ops.stop)
            (void)state->input_ops.stop(state->input_ops.context);
        return result;
    }
    *service = (struct ph_lifecycle_service){.context = state,
        .prepare = prepare, .dispatch = dispatch, .complete = complete,
        .release = release, .stop = stop};
    return 0;
}
