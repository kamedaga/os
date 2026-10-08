# Consume upstream unchanged; replace only the PachaOS port objects and
# include precedence outside the upstream source tree.
include $(srcdir)/Makefile
port_root := $(abspath $(srcdir)/../pachaos)
repo_root := $(abspath $(srcdir)/../..)
CFLAGS_ALL := -I$(port_root)/syscall -I$(repo_root)/userland/filed/include -I$(repo_root)/userland/libipc/include -I$(repo_root)/kobox2/protocol/include -I$(repo_root)/kobox2/protocol/generated/include $(CFLAGS_ALL)
port_objs := obj/port/native.o obj/port/filesystem.o obj/port/exec_payload.o
LIBC_OBJS := $(filter-out obj/src/thread/pachaos/filed_runtime.o,$(LIBC_OBJS)) $(port_objs)
AOBJS := $(LIBC_OBJS)
LOBJS := $(LIBC_OBJS:.o=.lo)
lib/libc.a: $(port_objs)
lib/libc.so: $(port_objs:.o=.lo)
$(port_objs) $(port_objs:.o=.lo): | obj/port
obj/port:
	mkdir -p $@
obj/port/%.lo: CFLAGS_ALL += -fPIC
obj/port/native.o obj/port/native.lo: $(port_root)/syscall/native.c $(GENH) $(IMPH)
	$(CC_CMD)
obj/port/filesystem.o obj/port/filesystem.lo: $(repo_root)/kobox2/protocol/src/filesystem.c $(GENH) $(IMPH)
	$(CC_CMD)
obj/port/exec_payload.o obj/port/exec_payload.lo: $(repo_root)/userland/filed/src/exec_payload.c $(GENH) $(IMPH)
	$(CC_CMD)
