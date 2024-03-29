#
# Makefile for the PCI bus specific drivers.
#

obj-y		+= access.o bus.o probe.o remove.o pci.o quirks.o \
			pci-driver.o search.o pci-sysfs.o rom.o setup-res.o
obj-$(CONFIG_PROC_FS) += proc.o

# Build PCI Express stuff if needed
obj-$(CONFIG_PCIEPORTBUS) += pcie/

obj-$(CONFIG_HOTPLUG) += hotplug.o

# Build the PCI Hotplug drivers if we were asked to
obj-$(CONFIG_HOTPLUG_PCI) += hotplug/

# Build Intel IOMMU support
obj-$(CONFIG_DMAR) += dmar.o iova.o intel-iommu.o

#
# Some architectures use the generic PCI setup functions
#
obj-$(CONFIG_X86) += setup-bus.o
obj-$(CONFIG_ALPHA) += setup-bus.o setup-irq.o
obj-$(CONFIG_ARM) += setup-bus.o setup-irq.o
obj-$(CONFIG_PARISC) += setup-bus.o
obj-$(CONFIG_SUPERH) += setup-bus.o setup-irq.o
obj-$(CONFIG_PPC32) += setup-irq.o
obj-$(CONFIG_PPC64) += setup-bus.o
obj-$(CONFIG_MIPS) += setup-bus.o setup-irq.o
obj-$(CONFIG_X86_VISWS) += setup-irq.o

ifndef CONFIG_PPC_MERGE
msiobj-y := msi.o msi-apic.o
msiobj-$(CONFIG_IA64_GENERIC) += msi-altix.o
msiobj-$(CONFIG_IA64_SGI_SN2) += msi-altix.o
obj-$(CONFIG_PCI_MSI) += $(msiobj-y)
endif

#
# ACPI Related PCI FW Functions
#
obj-$(CONFIG_ACPI)    += pci-acpi.o

# Cardbus & CompactPCI use setup-bus
obj-$(CONFIG_HOTPLUG) += setup-bus.o

ifndef CONFIG_X86
obj-y += syscall.o
endif

ifndef CONFIG_XEN
obj-$(CONFIG_PCI_STUB) += pci-stub.o
endif

ifeq ($(CONFIG_PCI_DEBUG),y)
EXTRA_CFLAGS += -DDEBUG
endif

ifdef CONFIG_XEN
include $(srctree)/scripts/Makefile.xen
obj-y := $(call cherrypickxen, $(obj-y))
endif

obj-$(CONFIG_PCI_IOV) += iov.o
