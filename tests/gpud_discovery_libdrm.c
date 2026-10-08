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
#include <stdlib.h>
#include <xf86drm.h>

int stat(const char *path, struct stat *st)
{
    int rc = syscall(SYS_newfstatat, AT_FDCWD, path, st, 0);
    if (!rc && (!strcmp(path, "/dev/dri/renderD128") || !strcmp(path, "/dev/dri/card0"))) {
        st->st_mode = S_IFCHR | 0660;
        st->st_rdev = makedev(226, !strcmp(path, "/dev/dri/card0") ? 0 : 128);
    }
    return rc;
}
int fstat(int fd, struct stat *st)
{
    int rc = syscall(SYS_fstat, fd, st);
    char proc[64], path[128];
    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
    ssize_t size = readlink(proc, path, sizeof(path) - 1);
    if (!rc && size >= 0) {
        path[size] = 0;
        if (!strcmp(path, "/dev/dri/renderD128") || !strcmp(path, "/dev/dri/card0")) {
            st->st_mode = S_IFCHR | 0660;
            st->st_rdev = makedev(226, !strcmp(path, "/dev/dri/card0") ? 0 : 128);
        }
    }
    return rc;
}
int main(int argc, char **argv)
{
    (void)argv;
    const int primary = argc > 1;
    dev_t node=makedev(226,128);
    int type=drmGetNodeTypeFromDevId(node);
    if(type!=DRM_NODE_RENDER) { fprintf(stderr,"node type=%d\n",type); return 1; }
    drmDevicePtr d=NULL;
    int rc=drmGetDeviceFromDevId(node,DRM_DEVICE_GET_PCI_REVISION,&d);
    if(rc || !d) { fprintf(stderr,"discovery status=%d\n",rc); return 1; }
    int ok=d->bustype==DRM_BUS_PCI && d->businfo.pci->bus==0x2d &&
        d->deviceinfo.pci->vendor_id==0x5678 && d->deviceinfo.pci->device_id==0x1234 &&
        d->deviceinfo.pci->subvendor_id==0x6789 && d->deviceinfo.pci->subdevice_id==0x2345 &&
        d->deviceinfo.pci->revision_id==0x91 &&
        d->available_nodes==((1<<DRM_NODE_RENDER) | (primary ? 1<<DRM_NODE_PRIMARY : 0));
    drmFreeDevice(&d);
    const char *nodes[] = {"/dev/dri/renderD128", "/dev/dri/card0"};
    for (int i = 0; i < 1 + primary; ++i) {
        int fd = open(nodes[i], O_RDONLY | O_CLOEXEC);
        if (fd < 0) return 1;
        char *name = drmGetDeviceNameFromFd2(fd);
        if (!name || strcmp(name, nodes[i])) {
            fprintf(stderr, "DRI3 fd pathname missing: %s -> %s\n", nodes[i], name ? name : "NULL");
            ok = 0;
        }
        free(name);
        close(fd);
    }
    puts(ok ? "unmodified libdrm: PCI discovery and DRI3 fd path resolution PASS" : "DRM discovery mismatch");
    return !ok;
}
