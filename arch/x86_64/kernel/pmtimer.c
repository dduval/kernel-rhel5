/* Ported over from i386 by AK, original copyright was:
 *
 * (C) Dominik Brodowski <linux@brodo.de> 2003
 *
 * Driver to use the Power Management Timer (PMTMR) available in some
 * southbridges as primary timing source for the Linux kernel.
 *
 * Based on parts of linux/drivers/acpi/hardware/hwtimer.c, timer_pit.c,
 * timer_hpet.c, and on Arjan van de Ven's implementation for 2.4.
 *
 * This file is licensed under the GPL v2.
 *
 * Dropped all the hardware bug workarounds for now. Hopefully they
 * are not needed on 64bit chipsets.
 */

#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/cpumask.h>
#include <asm/io.h>
#include <asm/proto.h>
#include <asm/msr.h>
#include <asm/vsyscall.h>
#include <asm/apicdef.h>
#include <asm/apic.h>

/* The I/O port the PMTMR resides at.
 * The location is detected during setup_arch(),
 * in arch/i386/kernel/acpi/boot.c */
u32 pmtmr_ioport __read_mostly;

/* value of the Power timer at last timer interrupt */
static u32 offset_delay;
static u32 last_pmtmr_tick;

#define ACPI_PM_MASK 0xFFFFFF /* limit it to 24 bits */

static inline u32 cyc2us(u32 cycles)
{
	/* The Power Management Timer ticks at 3.579545 ticks per microsecond.
	 * 1 / PM_TIMER_FREQUENCY == 0.27936511 =~ 286/1024 [error: 0.024%]
	 *
	 * Even with HZ = 100, delta is at maximum 35796 ticks, so it can
	 * easily be multiplied with 286 (=0x11E) without having to fear
	 * u32 overflows.
	 */
	cycles *= 286;
	return (cycles >> 10);
}

int pmtimer_mark_offset(void)
{
	static int first_run = 1;
	unsigned long tsc;
	u32 lost;

	u32 tick = inl(pmtmr_ioport);
	u32 delta;

	delta = cyc2us((tick - last_pmtmr_tick) & ACPI_PM_MASK);

	last_pmtmr_tick = tick;
	monotonic_base += delta * NSEC_PER_USEC;

	delta += offset_delay;

	lost = delta / (USEC_PER_SEC / REAL_HZ);
	offset_delay = delta % (USEC_PER_SEC / REAL_HZ);

	rdtscll(tsc);
	vxtime.last_tsc = tsc - offset_delay * (u64)cpu_khz / 1000;

	/* don't calculate delay for first run,
	   or if we've got less then a tick */
	if (first_run || (lost < 1)) {
		first_run = 0;
		offset_delay = 0;
	}

	return lost - 1;
}

static unsigned pmtimer_wait_tick(void)
{
	u32 a, b;
	for (a = b = inl(pmtmr_ioport) & ACPI_PM_MASK;
	     a == b;
	     b = inl(pmtmr_ioport) & ACPI_PM_MASK)
		cpu_relax();
	return b;
}

/* note: wait time is rounded up to one tick */
void pmtimer_wait(unsigned us)
{
	u32 a, b;
	a = pmtimer_wait_tick();
	do {
		b = inl(pmtmr_ioport);
		cpu_relax();
	} while (cyc2us(b - a) < us);
}

int pmtimer_calibrate_apic(unsigned us, int *tries)
{
	u32 a, b;
	unsigned int apic = 0, apic_start = 0;

	while(*tries) {
		apic_start = apic_read(APIC_TMCCT);
		a = pmtimer_wait_tick();
		do {
			b = inl(pmtmr_ioport);
			cpu_relax();
		} while (cyc2us(b - a) < us);
		apic = apic_read(APIC_TMCCT);
		b = inl(pmtmr_ioport);

		/* if wait is longer that ~10% of expected time, try again */
		if ((cyc2us(b - a)) < (us + (us >> 3)))
			break;
		(*tries)--;
	}

	return (apic_start - apic);
}

void pmtimer_resume(void)
{
	last_pmtmr_tick = inl(pmtmr_ioport);
}

long do_gettimeoffset_pm(void)
{
	u32 now, offset, delta = 0;

	offset = last_pmtmr_tick;
	now = inl(pmtmr_ioport);
	delta = (now - offset) & ACPI_PM_MASK;

	/* seems crazy to do with PM timer resolution but we need nsec
	   resolution in arch/x86_64/kernel/time.c code */
	return ((offset_delay + cyc2us(delta)) * NSEC_PER_USEC);
}


static int __init nopmtimer_setup(char *s)
{
	pmtmr_ioport = 0;
	return 1;
}

__setup("nopmtimer", nopmtimer_setup);
