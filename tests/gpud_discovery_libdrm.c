/* Exercise the shipped, unmodified libdrm against gpud's generated tree.
 * Only device-node stat is simulated: the host needs no GPU or mknod rights. */
#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <xf86drm.h>

int stat(const char *path, struct stat *st)
{
    int rc = syscall(SYS_newfstatat, AT_FDCWD, path, st, 0);
    if (!rc && !strcmp(path, "/dev/dri/renderD128")) {
        st->st_mode = S_IFCHR | 0660;
        st->st_rdev = makedev(226,128);
    }
    return rc;
}
int main(void)
{
    dev_t node=makedev(226,128);
    int type=drmGetNodeTypeFromDevId(node);
    if(type!=DRM_NODE_RENDER) { fprintf(stderr,"node type=%d\n",type); return 1; }
    drmDevicePtr d=NULL;
    int rc=drmGetDeviceFromDevId(node,DRM_DEVICE_GET_PCI_REVISION,&d);
    if(rc || !d) { fprintf(stderr,"discovery status=%d\n",rc); return 1; }
    int ok=d->bustype==DRM_BUS_PCI && d->businfo.pci->bus==0x2d &&
        d->deviceinfo.pci->vendor_id==0x5678 && d->deviceinfo.pci->device_id==0x1234 &&
        d->deviceinfo.pci->subvendor_id==0x6789 && d->deviceinfo.pci->subdevice_id==0x2345 &&
        d->deviceinfo.pci->revision_id==0x91 && d->available_nodes==(1<<DRM_NODE_RENDER);
    drmFreeDevice(&d);
    puts(ok ? "unmodified libdrm: render node and PCI discovery PASS" : "PCI identity mismatch");
    return !ok;
}
