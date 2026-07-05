# SPDX-License-Identifier: GPL-2.0

obj-m += linux_tty_core.o
obj-m += linux_tty_n_null.o
obj-m += linux_virtio.o
obj-m += linux_virtio_ring.o
obj-m += linux_virtio_console.o

linux_tty_core-y := \
	kmodule/core_entry.o \
	drivers/tty/tty_io.o \
	drivers/tty/n_tty.o \
	drivers/tty/tty_ioctl.o \
	drivers/tty/tty_ldisc.o \
	drivers/tty/tty_buffer.o \
	drivers/tty/tty_port.o \
	drivers/tty/tty_mutex.o \
	drivers/tty/tty_ldsem.o \
	drivers/tty/tty_baudrate.o \
	drivers/tty/tty_jobctrl.o \
	drivers/tty/pty.o \
	drivers/tty/hvc/hvc_console.o \
	fs/devpts/inode.o

linux_tty_n_null-y := drivers/tty/n_null.o
linux_virtio-y := \
	drivers/virtio/virtio.o \
	drivers/virtio/virtio_anchor.o
linux_virtio_ring-y := drivers/virtio/virtio_ring.o
linux_virtio_console-y := drivers/char/virtio_console.o
ccflags-y += \
	-I$(src)/drivers/tty \
	-I$(src)/drivers/tty/hvc \
	-I$(src)/drivers/virtio \
	-DCONFIG_TTY=1 \
	-DCONFIG_UNIX98_PTYS=1 \
	-DCONFIG_HVC_DRIVER=1 \
	-DCONFIG_VIRTIO=1 \
	-DCONFIG_VIRTIO_PCI=1 \
	-DCONFIG_VIRTIO_CONSOLE=1 \
	-DCONFIG_KOBOX_TTY_ISLAND=1 \
	-UCONFIG_VT \
	-UCONFIG_LEGACY_PTYS \
	-UCONFIG_LDISC_AUTOLOAD \
	-UCONFIG_MAGIC_SYSRQ \
	-UCONFIG_AUDIT \
	-UCONFIG_COMPAT \
	-UCONFIG_VIRTIO_HARDEN_NOTIFICATION \
	-UCONFIG_VIRTIO_PCI_LEGACY \
	-UCONFIG_VIRTIO_PCI_ADMIN_LEGACY \
	-UCONFIG_REMOTEPROC

CFLAGS_REMOVE_drivers/tty/tty_io.o += -pg -mfentry -mrecord-mcount
CFLAGS_REMOVE_drivers/tty/n_tty.o += -pg -mfentry -mrecord-mcount
CFLAGS_REMOVE_drivers/tty/pty.o += -pg -mfentry -mrecord-mcount
CFLAGS_REMOVE_drivers/tty/hvc/hvc_console.o += -pg -mfentry -mrecord-mcount
CFLAGS_REMOVE_drivers/virtio/virtio.o += -pg -mfentry -mrecord-mcount
CFLAGS_REMOVE_drivers/virtio/virtio_anchor.o += -pg -mfentry -mrecord-mcount
CFLAGS_REMOVE_drivers/virtio/virtio_ring.o += -pg -mfentry -mrecord-mcount
CFLAGS_REMOVE_drivers/virtio/virtio_pci_modern_dev.o += -pg -mfentry -mrecord-mcount
CFLAGS_REMOVE_drivers/virtio/virtio_pci_legacy_dev.o += -pg -mfentry -mrecord-mcount
CFLAGS_REMOVE_drivers/virtio/virtio_pci_modern.o += -pg -mfentry -mrecord-mcount
CFLAGS_REMOVE_drivers/virtio/virtio_pci_legacy.o += -pg -mfentry -mrecord-mcount
CFLAGS_REMOVE_drivers/virtio/virtio_pci_common.o += -pg -mfentry -mrecord-mcount
CFLAGS_REMOVE_drivers/char/virtio_console.o += -pg -mfentry -mrecord-mcount

KDIR ?= /lib/modules/$(shell uname -r)/build
OUT ?= $(CURDIR)/../../.artifacts/kobox-linux-tty

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(OUT) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(OUT) clean
