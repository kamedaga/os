/* SPDX-License-Identifier: MIT */
#pragma once
#include <kobox2/i915_context_shape.h>

static int64_t lpr_i915_context_param_snapshot(const gpud_i915_context_param_t *source,
    gpud_i915_context_param_wire_t *wire, uint64_t *engine_pointer, uint32_t *engines)
{
    *wire = (gpud_i915_context_param_wire_t){.parameter=source->parameter,
        .value=source->value, .size=source->size};
    *engine_pointer = 0;
    if (kb2_i915_context_scalar(source->parameter))
        return source->size ? -LPR_LINUX_EINVAL : 0;
    if (source->parameter != 10) return -LPR_LINUX_EOPNOTSUPP;
    wire->value = 0;
    if (!source->size) return 0;
    if (source->size < 8 || (source->size-8)%4 ||
        (source->size-8)/4 > KB2_GPU_DRM_I915_MAX_CONTEXT_ENGINES_PER_PARAM)
        return -LPR_LINUX_EINVAL;
    if (!source->value) return -LPR_LINUX_EFAULT;
    uint64_t extensions;
    lpr_memcpy(&extensions,(const void *)(uintptr_t)source->value,8);
    if (extensions) return -LPR_LINUX_EOPNOTSUPP;
    const uint32_t count = (source->size-8)/4;
    if (count > KB2_GPU_DRM_I915_MAX_CONTEXT_ENGINES-*engines)
        return -LPR_LINUX_EINVAL;
    wire->engine_first = *engines;
    wire->engine_count = count;
    *engine_pointer = source->value;
    *engines += count;
    return 0;
}

static __attribute__((noinline)) int64_t lpr_i915_context_ioctl(
    uint64_t handle, uint32_t command, uint64_t argument)
{
    if (command!=GPUD_DRM_IOCTL_I915_CONTEXT_CREATE &&
        command!=GPUD_DRM_IOCTL_I915_CONTEXT_CREATE_EXT &&
        command!=GPUD_DRM_IOCTL_I915_CONTEXT_SETPARAM) return -LPR_LINUX_EINVAL;
    gpud_i915_context_param_wire_t params[KB2_GPU_DRM_I915_MAX_CONTEXT_PARAMS];
    uint64_t engine_pointers[KB2_GPU_DRM_I915_MAX_CONTEXT_PARAMS];
    uint64_t seen[KB2_GPU_DRM_I915_MAX_CONTEXT_PARAMS];
    uint32_t count=0, engines=0, first=0;
    uint64_t next=0;
    if (command==GPUD_DRM_IOCTL_I915_CONTEXT_SETPARAM) {
        gpud_i915_context_param_t input;
        lpr_memcpy(&input,(const void *)(uintptr_t)argument,sizeof(input));
        first=input.context_id;
        int64_t error=lpr_i915_context_param_snapshot(&input,&params[0],&engine_pointers[0],&engines);
        if(error) return error;
        count=1;
    } else {
        gpud_i915_context_create_t input={0};
        lpr_memcpy(&input,(const void *)(uintptr_t)argument,
            command==GPUD_DRM_IOCTL_I915_CONTEXT_CREATE ? 8 : sizeof(input));
        if(command==GPUD_DRM_IOCTL_I915_CONTEXT_CREATE && input.flags)
            return -LPR_LINUX_EINVAL;
        if(input.flags & ~3u) return -LPR_LINUX_EINVAL;
        first=input.flags;
        if(input.flags & 1u) next=input.extensions;
    }
    while(next) {
        if(count==KB2_GPU_DRM_I915_MAX_CONTEXT_PARAMS) return -LPR_LINUX_EINVAL;
        for(uint32_t i=0;i<count;++i) if(seen[i]==next) return -LPR_LINUX_EINVAL;
        seen[count]=next;
        gpud_i915_context_setparam_ext_t extension;
        lpr_memcpy(&extension.base,(const void *)(uintptr_t)next,sizeof(extension.base));
        if(extension.base.name || extension.base.flags) return -LPR_LINUX_EINVAL;
        for(unsigned i=0;i<4;++i) if(extension.base.reserved[i]) return -LPR_LINUX_EINVAL;
        lpr_memcpy(&extension.param,(const unsigned char *)(uintptr_t)next+sizeof(extension.base),
            sizeof(extension.param));
        if(extension.param.context_id) return -LPR_LINUX_EINVAL;
        int64_t error=lpr_i915_context_param_snapshot(&extension.param,
            &params[count],&engine_pointers[count],&engines);
        if(error) return error;
        ++count;
        next=extension.base.next;
    }
    const uint64_t prefix=(uint64_t)count*sizeof(params[0]);
    const uint64_t bytes=prefix+(uint64_t)engines*4;
    void *page=0,*aux=0;
    pacha_fd_t aux_fd=-1;
    uint64_t aux_size=0;
    const pacha_fd_t page_fd=lpr_create_tty_wire_page(&page);
    if(page_fd<0) return page_fd;
    int64_t status=0;
    if(bytes) {
        status=lpr_drm_aux_create(bytes,0,&aux_fd,&aux,&aux_size);
        if(status) goto done;
        lpr_memcpy(aux,params,prefix);
        for(uint32_t i=0;i<count;++i) if(engine_pointers[i] && params[i].engine_count)
            lpr_memcpy((unsigned char *)aux+prefix+(uint64_t)params[i].engine_first*4,
                (const unsigned char *)(uintptr_t)engine_pointers[i]+8,
                (uint64_t)params[i].engine_count*4);
    }
    gpud_drm_ioctl_request_t *request=lpr_gpud_drm_payload(page);
    lpr_memset(request,0,sizeof(*request));
    request->handle=handle;
    request->request=command;
    request->arg_size=command==GPUD_DRM_IOCTL_I915_CONTEXT_SETPARAM ? 24 :
        command==GPUD_DRM_IOCTL_I915_CONTEXT_CREATE ? 8 : 16;
    request->data_size=16;
    request->aux_size=bytes;
    const uint32_t header[4]={first,count,engines,0};
    lpr_memcpy(request->data,header,sizeof(header));
    status=lpr_gpud_drm_call_transfer(GPUD_DRM_OP_HANDLE_IOCTL,
        page_fd,page,sizeof(*request),0,0,aux_fd,1);
    if(!status && command!=GPUD_DRM_IOCTL_I915_CONTEXT_SETPARAM) {
        uint32_t context_id, reserved;
        lpr_memcpy(&context_id,request->data,4);
        lpr_memcpy(&reserved,request->data+4,4);
        if(request->data_size!=8 || !context_id || reserved) status=-LPR_DRM_ERR_PROTOCOL;
        else lpr_memcpy((void *)(uintptr_t)argument,&context_id,4);
    }
done:
    lpr_drm_aux_destroy(aux_fd,aux,aux_size);
    lpr_destroy_tty_wire_page(page_fd,page);
    return status;
}
