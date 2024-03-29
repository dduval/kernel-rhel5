#
# x86_64/Makefile
#
# This file is included by the global makefile so that you can add your own
# architecture-specific flags and dependencies. Remember to do have actions
# for "archclean" and "archdep" for cleaning up and making dependencies for
# this architecture
#
# This file is subject to the terms and conditions of the GNU General Public
# License.  See the file "COPYING" in the main directory of this archive
# for more details.
#
# Copyright (C) 1994 by Linus Torvalds
#
# 19990713  Artur Skawina <skawina@geocities.com>
#           Added '-march' and '-mpreferred-stack-boundary' support
# 20000913  Pavel Machek <pavel@suse.cz>
#	    Converted for x86_64 architecture
# 20010105  Andi Kleen, add IA32 compiler.
#           ....and later removed it again....
#
# $Id: Makefile,v 1.31 2002/03/22 15:56:07 ak Exp $

LDFLAGS		:= -m elf_x86_64
OBJCOPYFLAGS	:= -O binary -R .note -R .comment -S
LDFLAGS_vmlinux :=
CHECKFLAGS      += -D__x86_64__ -m64

cflags-y	:=
cflags-kernel-y	:=
cflags-$(CONFIG_MK8) += $(call cc-option,-march=k8)
cflags-$(CONFIG_MPSC) += $(call cc-option,-march=nocona)
cflags-$(CONFIG_GENERIC_CPU) += $(call cc-option,-mtune=generic)

cppflags-$(CONFIG_XEN) += \
	-D__XEN_INTERFACE_VERSION__=$(CONFIG_XEN_INTERFACE_VERSION)
CPPFLAGS += $(cppflags-y)

cflags-y += -m64
cflags-y += -mno-red-zone
cflags-y += -mcmodel=kernel
cflags-y += -pipe
cflags-kernel-$(CONFIG_REORDER) += -ffunction-sections
# this makes reading assembly source easier, but produces worse code
# actually it makes the kernel smaller too.
cflags-y += -fno-reorder-blocks
cflags-y += -Wno-sign-compare
cflags-y += -fno-asynchronous-unwind-tables
ifneq ($(CONFIG_DEBUG_INFO),y)
# -fweb shrinks the kernel a bit, but the difference is very small
# it also messes up debugging, so don't use it for now.
#cflags-y += $(call cc-option,-fweb)
endif
# -funit-at-a-time shrinks the kernel .text considerably
# unfortunately it makes reading oopses harder.
cflags-y += $(call cc-option,-funit-at-a-time)
# prevent gcc from generating any FP code by mistake
cflags-y += $(call cc-option,-mno-sse -mno-mmx -mno-sse2 -mno-3dnow,)

CFLAGS += $(cflags-y)
CFLAGS_KERNEL += $(cflags-kernel-y)
AFLAGS += -m64

head-y := arch/x86_64/kernel/head.o arch/x86_64/kernel/head64.o arch/x86_64/kernel/init_task.o

libs-y 					+= arch/x86_64/lib/
core-y					+= arch/x86_64/kernel/ \
					   arch/x86_64/mm/ \
					   arch/x86_64/crypto/
core-$(CONFIG_IA32_EMULATION)		+= arch/x86_64/ia32/
drivers-$(CONFIG_PCI)			+= arch/x86_64/pci/
drivers-$(CONFIG_OPROFILE)		+= arch/x86_64/oprofile/

boot := arch/x86_64/boot

PHONY += bzImage bzlilo install archmrproper \
	 fdimage fdimage144 fdimage288 isoimage archclean

ifdef CONFIG_XEN
CPPFLAGS := -Iinclude$(if $(KBUILD_SRC),2)/asm/mach-xen $(CPPFLAGS)
head-y := arch/x86_64/kernel/head-xen.o arch/x86_64/kernel/head64-xen.o arch/x86_64/kernel/init_task.o
LDFLAGS_vmlinux := -e _start
boot := arch/i386/boot-xen
.PHONY: vmlinuz
#Default target when executing "make"
all: vmlinuz

vmlinuz: vmlinux
	$(Q)$(MAKE) $(build)=$(boot) $@

install:
	$(Q)$(MAKE) $(build)=$(boot) XENGUEST=$(XENGUEST) $@
else
#Default target when executing "make"
all: bzImage

BOOTIMAGE                     := arch/x86_64/boot/bzImage
KBUILD_IMAGE                  := $(BOOTIMAGE)

bzImage: vmlinux
	$(Q)$(MAKE) $(build)=$(boot) $(BOOTIMAGE)

bzlilo: vmlinux
	$(Q)$(MAKE) $(build)=$(boot) BOOTIMAGE=$(BOOTIMAGE) zlilo

bzdisk: vmlinux
	$(Q)$(MAKE) $(build)=$(boot) BOOTIMAGE=$(BOOTIMAGE) zdisk

fdimage fdimage144 fdimage288 isoimage: vmlinux
	$(Q)$(MAKE) $(build)=$(boot) BOOTIMAGE=$(BOOTIMAGE) $@

install:
	$(Q)$(MAKE) $(build)=$(boot) BOOTIMAGE=$(BOOTIMAGE) $@ 
endif

archclean:
	$(Q)$(MAKE) $(clean)=$(boot)

define archhelp
  echo  '* bzImage	- Compressed kernel image (arch/$(ARCH)/boot/bzImage)'
  echo  '  install	- Install kernel using'
  echo  '		   (your) ~/bin/installkernel or'
  echo  '		   (distribution) /sbin/installkernel or'
  echo  '		   install to $$(INSTALL_PATH) and run lilo'
  echo  '  bzdisk       - Create a boot floppy in /dev/fd0'
  echo  '  fdimage      - Create a boot floppy image'
  echo  '  isoimage     - Create a boot CD-ROM image'
endef

CLEAN_FILES += arch/$(ARCH)/boot/fdimage \
	       arch/$(ARCH)/boot/image.iso \
	       arch/$(ARCH)/boot/mtools.conf


