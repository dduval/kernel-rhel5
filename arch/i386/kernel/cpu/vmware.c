/*
 * VMware Detection code.
 *
 * Copyright (C) 2008, VMware, Inc.
 * Author : Alok N Kataria <akataria@vmware.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, GOOD TITLE or
 * NON INFRINGEMENT.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <linux/dmi.h>
#include <linux/init.h>
#include <asm/div64.h>
#include <asm/vmware.h>

extern int __initdata nosoftlockup;

#define CPUID_VMWARE_INFO_LEAF	0x40000000
#define VMWARE_HYPERVISOR_MAGIC	0x564D5868
#define VMWARE_HYPERVISOR_PORT	0x5658

#define VMWARE_PORT_CMD_GETVERSION	10
#define VMWARE_PORT_CMD_GETHZ		45
#define VMWARE_PORT_CMD_LAZYTIMEREMULATION 49

#define VMWARE_PORT(cmd, eax, ebx, ecx, edx)				\
	__asm__("inl (%%dx)" :						\
			"=a"(eax), "=c"(ecx), "=d"(edx), "=b"(ebx) :	\
			"0"(VMWARE_HYPERVISOR_MAGIC),			\
			"1"(VMWARE_PORT_CMD_##cmd),			\
			"2"(VMWARE_HYPERVISOR_PORT), "3"(UINT_MAX) :	\
			"memory");

static inline int vmware_enable_lazy_timer_emulation(void)
{
	uint32_t eax, ebx, ecx, edx;
	VMWARE_PORT(LAZYTIMEREMULATION, eax, ebx, ecx, edx);
	return ebx == VMWARE_HYPERVISOR_MAGIC;
}

static inline int __vmware_platform(void)
{
	uint32_t eax, ebx, ecx, edx;
	VMWARE_PORT(GETVERSION, eax, ebx, ecx, edx);
	return eax != (uint32_t)-1 && ebx == VMWARE_HYPERVISOR_MAGIC;
}

static unsigned long __vmware_get_tsc_khz(void)
{
        uint64_t tsc_hz;
        uint32_t eax, ebx, ecx, edx;

        VMWARE_PORT(GETHZ, eax, ebx, ecx, edx);

        if (ebx == UINT_MAX)
                return 0;
        tsc_hz = eax | (((uint64_t)ebx) << 32);
        do_div(tsc_hz, 1000);
        BUG_ON(tsc_hz >> 32);
        return tsc_hz;
}

/*
 * While checking the dmi string infomation, just checking the product
 * serial key should be enough, as this will always have a VMware
 * specific string when running under VMware hypervisor.
 */
int vmware_platform(void)
{
	if (cpu_has_hypervisor) {
		unsigned int eax, ebx, ecx, edx;
		char hyper_vendor_id[13];

		cpuid(CPUID_VMWARE_INFO_LEAF, &eax, &ebx, &ecx, &edx);
		memcpy(hyper_vendor_id + 0, &ebx, 4);
		memcpy(hyper_vendor_id + 4, &ecx, 4);
		memcpy(hyper_vendor_id + 8, &edx, 4);
		hyper_vendor_id[12] = '\0';
		if (!strcmp(hyper_vendor_id, "VMwareVMware"))
			return 1;
	} else if (dmi_name_in_serial("VMware") &&
		   __vmware_platform())
		return 1;

	return 0;
}

unsigned long vmware_get_tsc_khz(void)
{
	unsigned long vm_tsc_khz;

	BUG_ON(!vmware_platform());

	/* Enable "lazy" timer emulation.  Rather than holding
	 * back virtual time when timer interrupt delivery falls
	 * behind and attempting to "catch up", in lazy mode,
	 * missed periodic interrupts are skipped and virtual
	 * time always reflects real time.
	 */
	vm_tsc_khz = __vmware_get_tsc_khz();

#ifdef CONFIG_X86_64
	{
	extern int timekeeping_use_tsc;

	if (vm_tsc_khz && timekeeping_use_tsc >= 0) {
		if (vmware_enable_lazy_timer_emulation()) {
			timekeeping_use_tsc = 1;
			/*
			 * Disable softlockups if using TSC based
			 * timekeeping, as this may have false
			 * positives when running under hypervisors.
			 */
			nosoftlockup = 1;
		}
		else {
			printk(KERN_WARNING
			       "time.c: failed to enable lazy timer "
			       "emulation. Disabling tsc based "
			       "timekeeping\n");
			timekeeping_use_tsc = 0;
		}
	}
	}
#else
	if (vm_tsc_khz) {
		if (!vmware_enable_lazy_timer_emulation())
			printk(KERN_WARNING
			       "time.c: failed to enable lazy timer "
			       "emulation. Disabling tsc based "
			       "timekeeping\n");
	}
#endif

	return vm_tsc_khz;
}

/*
 * VMware hypervisor takes care of exporting a reliable TSC to the guest.
 * Still, due to timing difference when running on virtual cpus, the TSC can
 * be marked as unstable in some cases. For example, the TSC sync check at
 * bootup can fail due to a marginal offset between vcpus' TSCs (though the
 * TSCs do not drift from each other).  Also, the ACPI PM timer clocksource
 * is not suitable as a watchdog when running on a hypervisor because the
 * kernel may miss a wrap of the counter if the vcpu is descheduled for a
 * long time. To skip these checks at runtime we set these capability bits,
 * so that the kernel could just trust the hypervisor with providing a
 * reliable virtual TSC that is suitable for timekeeping.
 */
void __cpuinit vmware_set_feature_bits(struct cpuinfo_x86 *c)
{
	set_bit(X86_FEATURE_CONSTANT_TSC, c->x86_capability);
	set_bit(X86_FEATURE_TSC_RELIABLE, c->x86_capability);
}
