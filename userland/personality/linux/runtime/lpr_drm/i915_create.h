/* SPDX-License-Identifier: MIT */
#pragma once
static __attribute__((noinline)) int64_t lpr_i915_create_ext_ioctl(uint64_t handle,uint64_t argument)
{
    gpud_i915_create_ext_t input;
    lpr_memcpy(&input,(const void *)(uintptr_t)argument,sizeof(input));
    if(!input.size || (input.flags & ~1u)) return -LPR_LINUX_EINVAL;
    uint32_t regions[64];uint32_t count=0;uint64_t seen[64],next=input.extensions;
    unsigned links=0;int found=0;
    while(next) {
        if(links==64) return -LPR_LINUX_EINVAL;
        for(unsigned i=0;i<links;++i) if(seen[i]==next) return -LPR_LINUX_EINVAL;
        seen[links++]=next;
        gpud_i915_user_extension_t base;
        lpr_memcpy(&base,(const void *)(uintptr_t)next,sizeof(base));
        if(base.flags) return -LPR_LINUX_EINVAL;
        for(unsigned i=0;i<4;++i) if(base.reserved[i]) return -LPR_LINUX_EINVAL;
        if(base.name) return -LPR_LINUX_EOPNOTSUPP;
        if(found++) return -LPR_LINUX_EINVAL;
        struct {uint32_t pad,count;uint64_t pointer;} payload;
        lpr_memcpy(&payload,(const unsigned char *)(uintptr_t)next+sizeof(base),sizeof(payload));
        if(payload.pad || !payload.count || payload.count>64) return -LPR_LINUX_EINVAL;
        if(!payload.pointer) return -LPR_LINUX_EFAULT;
        count=payload.count;lpr_memcpy(regions,(const void *)(uintptr_t)payload.pointer,count*4);
        for(uint32_t i=0;i<count;++i) for(uint32_t j=0;j<i;++j)
            if(regions[i]==regions[j]) return -LPR_LINUX_EINVAL;
        next=base.next;
    }
    void *page=0,*aux=0;uint64_t aux_size=0;pacha_fd_t aux_fd=-1;
    const pacha_fd_t page_fd=lpr_create_tty_wire_page(&page);
    if(page_fd<0) return page_fd;
    int64_t status=0;
    if(count) {
        status=lpr_drm_aux_create(count*4,0,&aux_fd,&aux,&aux_size);
        if(status) goto done;
        lpr_memcpy(aux,regions,count*4);
    }
    gpud_drm_ioctl_request_t *r=lpr_gpud_drm_payload(page);lpr_memset(r,0,sizeof(*r));
    r->handle=handle;r->request=GPUD_DRM_IOCTL_I915_GEM_CREATE_EXT;r->arg_size=24;r->data_size=24;r->aux_size=count*4;
    lpr_memcpy(r->data,&input.size,8);lpr_memcpy(r->data+8,&input.flags,4);lpr_memcpy(r->data+12,&count,4);
    status=lpr_gpud_drm_call_transfer(GPUD_DRM_OP_HANDLE_IOCTL,page_fd,page,sizeof(*r),0,0,aux_fd,1);
    if(!status) {
        uint32_t result_handle,reserved;lpr_memcpy(&result_handle,r->data+8,4);lpr_memcpy(&reserved,r->data+12,4);
        if(r->data_size!=16 || !result_handle || reserved) status=-LPR_DRM_ERR_PROTOCOL;
        else {lpr_memcpy((void *)(uintptr_t)argument,r->data,8);lpr_memcpy((unsigned char *)(uintptr_t)argument+8,&result_handle,4);}
    }
done:
    lpr_drm_aux_destroy(aux_fd,aux,aux_size);lpr_destroy_tty_wire_page(page_fd,page);return status;
}
