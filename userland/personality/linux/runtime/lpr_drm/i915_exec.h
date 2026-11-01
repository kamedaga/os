/* SPDX-License-Identifier: MIT */
#pragma once
#include <kobox2/i915_exec_shape.h>

static __attribute__((noinline)) int64_t lpr_i915_exec_ioctl(uint64_t handle,
    uint32_t command,uint64_t argument)
{
    if(command!=GPUD_DRM_IOCTL_I915_EXECBUFFER && command!=GPUD_DRM_IOCTL_I915_EXECBUFFER_WR)
        return -LPR_LINUX_EINVAL;
    gpud_i915_execbuffer_t input;
    lpr_memcpy(&input,(const void *)(uintptr_t)argument,sizeof(input));
    if(input.flags & (1u<<9)) return -LPR_LINUX_EACCES;
    if(input.flags & ((1u<<16)|(1u<<17)|(1u<<20)|(1u<<21))) return -LPR_LINUX_EOPNOTSUPP;
    if(input.dr1 || input.dr4 || input.sync_file_fds || input.context_id>UINT32_MAX ||
        (!(input.flags & (1u<<19)) && (input.fence_count || input.fences_pointer))) return -LPR_LINUX_EINVAL;
    const uint32_t header[8]={input.object_count,input.fence_count,input.batch_start,input.batch_length,
        (uint32_t)input.flags,(uint32_t)(input.flags>>32),(uint32_t)input.context_id,0};
    size_t bytes;
    if(!kb2_i915_exec_header(header,sizeof(header),&bytes)) return -LPR_LINUX_EINVAL;
    if(!input.objects_pointer || (input.fence_count && !input.fences_pointer)) return -LPR_LINUX_EFAULT;
    void *page=0,*aux=0;pacha_fd_t aux_fd=-1;uint64_t aux_size=0;
    const pacha_fd_t page_fd=lpr_create_tty_wire_page(&page);
    if(page_fd<0) return page_fd;
    int64_t status=lpr_drm_aux_create(bytes,0,&aux_fd,&aux,&aux_size);
    if(status) goto done;
    for(uint32_t i=0;i<input.object_count;++i) {
        gpud_i915_exec_object_t source;
        lpr_memcpy(&source,(const unsigned char *)(uintptr_t)input.objects_pointer+(uint64_t)i*sizeof(source),sizeof(source));
        if(source.relocation_count) {status=-LPR_LINUX_EOPNOTSUPP;goto done;}
        if(source.reserved) {status=-LPR_LINUX_EINVAL;goto done;}
        const gpud_i915_exec_object_wire_t wire={.handle=source.handle,
            .alignment=source.alignment,.offset=source.offset,.flags=source.flags,.pad_to_size=source.pad_to_size};
        lpr_memcpy((unsigned char *)aux+(uint64_t)i*sizeof(wire),&wire,sizeof(wire));
    }
    const uint64_t prefix=(uint64_t)input.object_count*40;
    if(input.fence_count) lpr_memcpy((unsigned char *)aux+prefix,
        (const void *)(uintptr_t)input.fences_pointer,(uint64_t)input.fence_count*8);
    if(!kb2_i915_exec_payload(aux,bytes,input.object_count,input.fence_count)) {status=-LPR_LINUX_EINVAL;goto done;}
    gpud_drm_ioctl_request_t *request=lpr_gpud_drm_payload(page);
    lpr_memset(request,0,sizeof(*request));
    request->handle=handle;request->request=command;request->arg_size=64;
    request->data_size=sizeof(header);request->aux_size=bytes;
    lpr_memcpy(request->data,header,sizeof(header));
    status=lpr_gpud_drm_call_transfer(GPUD_DRM_OP_HANDLE_IOCTL,
        page_fd,page,sizeof(*request),0,0,aux_fd,1);
    /* A request-sized failure is a transport/admission error, not a driver
     * reply. Only the separately validated result frame permits copyout. */
    if(request->data_size==sizeof(header) && status) goto done;
    int32_t result;uint32_t reserved;
    lpr_memcpy(&result,request->data,4);lpr_memcpy(&reserved,request->data+4,4);
    if(request->data_size!=8 || reserved || result>0 || result< -4095 || result!=status ||
        !kb2_i915_exec_payload(aux,bytes,input.object_count,input.fence_count)) {
        status=-LPR_DRM_ERR_PROTOCOL;goto done;
    }
    for(uint32_t i=0;i<input.object_count;++i) {
        /* Linux writes object offsets even when submission later fails.
         * Pointer-bearing caller records never leave LPR. */
        lpr_memcpy((unsigned char *)(uintptr_t)input.objects_pointer+
            (uint64_t)i*sizeof(gpud_i915_exec_object_t)+offsetof(gpud_i915_exec_object_t,offset),
            (const unsigned char *)aux+(uint64_t)i*40+16,8);
    }
done:
    lpr_drm_aux_destroy(aux_fd,aux,aux_size);
    lpr_destroy_tty_wire_page(page_fd,page);
    return status;
}
