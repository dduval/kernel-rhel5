/*
 *   (c) 2003-2006 Advanced Micro Devices, Inc.
 *  Your use of this code is subject to the terms and conditions of the
 *  GNU general public license version 2. See "COPYING" or
 *  http://www.gnu.org/licenses/gpl.html
 *
 *  Support : mark.langsdorf@amd.com
 *
 *  Based on the powernow-k7.c module written by Dave Jones.
 *  (C) 2003 Dave Jones <davej@codemonkey.org.uk> on behalf of SuSE Labs
 *  (C) 2004 Dominik Brodowski <linux@brodo.de>
 *  (C) 2004 Pavel Machek <pavel@suse.cz>
 *  Licensed under the terms of the GNU GPL License version 2.
 *  Based upon datasheets & sample CPUs kindly provided by AMD.
 *
 *  Valuable input gratefully received from Dave Jones, Pavel Machek,
 *  Dominik Brodowski, Jacob Shin, and others.
 *  Originally developed by Paul Devriendt.
 *  Processor information obtained from Chapter 9 (Power and Thermal Management)
 *  of the "BIOS and Kernel Developer's Guide for the AMD Athlon 64 and AMD
 *  Opteron Processors" available for download from www.amd.com
 *
 *  Tables for specific CPUs can be inferred from
 *     http://www.amd.com/us-en/assets/content_type/white_papers_and_tech_docs/30430.pdf
 */

#include <linux/kernel.h>
#include <linux/smp.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/cpumask.h>
#include <linux/sched.h>	/* for current / set_cpus_allowed() */
#include <linux/dmi.h>

#include <asm/msr.h>
#include <asm/io.h>
#include <asm/delay.h>
#include <asm/pci-direct.h>

#ifdef CONFIG_X86_POWERNOW_K8_ACPI
#include <linux/acpi.h>
#include <linux/mutex.h>
#include <acpi/processor.h>
#endif

#define PFX "powernow-k8: "
#define BFX PFX "BIOS error: "
#define VERSION "version 2.20.00"
#include "powernow-k8.h"

/* serialize freq changes  */
static DEFINE_MUTEX(fidvid_mutex);

static DEFINE_PER_CPU(struct powernow_k8_data *, powernow_data);
static int *req_state = NULL;
static int tscsync = 0;

/* preregister_acpi_perf can have 3 values:
   0 = NEVER call preregister_acpi_preregister_performance
   1 = call unless overridden by code check in powernowk8_init
   2 = ALWAYS call preregister_acpi_preregister_performance
 */
static int preregister_acpi_perf = 1;

static int cpu_family = CPU_OPTERON;

#ifndef CONFIG_SMP
static cpumask_t cpu_core_map[1];
#endif

/* Return a frequency in MHz, given an input fid */
static u32 find_freq_from_fid(u32 fid)
{
	return 800 + (fid * 100);
}


/* Return a frequency in KHz, given an input fid */
static u32 find_khz_freq_from_fid(u32 fid)
{
	return 1000 * find_freq_from_fid(fid);
}

static u32 find_khz_freq_from_pstate(struct cpufreq_frequency_table *data, u32 pstate)
{
	return data[pstate].frequency;
 }

/* Return the vco fid for an input fid
 *
 * Each "low" fid has corresponding "high" fid, and you can get to "low" fids
 * only from corresponding high fids. This returns "high" fid corresponding to
 * "low" one.
 */
static u32 convert_fid_to_vco_fid(u32 fid)
{
	if (fid < HI_FID_TABLE_BOTTOM)
		return 8 + (2 * fid);
	else
		return fid;
}

/*
 * Return 1 if the pending bit is set. Unless we just instructed the processor
 * to transition to a new state, seeing this bit set is really bad news.
 */
static int pending_bit_stuck(void)
{
	u32 lo, hi;

	if (cpu_family == CPU_HW_PSTATE)
		return 0;

	rdmsr(MSR_FIDVID_STATUS, lo, hi);
	return lo & MSR_S_LO_CHANGE_PENDING ? 1 : 0;
}

/*
 * Update the global current fid / vid values from the status msr.
 * Returns 1 on error.
 */
static int query_current_values_with_pending_wait(struct powernow_k8_data *data)
{
	u32 lo, hi;
	u32 i = 0;

	if (cpu_family == CPU_HW_PSTATE) {
		rdmsr(MSR_PSTATE_STATUS, lo, hi);
		i = lo & HW_PSTATE_MASK;
		data->currpstate = i;

		/*
		 * a workaround for family 11h erratum 311 might cause
		 * an "out-of-range Pstate if the core is in Pstate-0
		 */
		if ((boot_cpu_data.x86 == 0x11) && (i >= data->numps))
 			data->currpstate = HW_PSTATE_0;

		return 0;
	}
	do {
		if (i++ > 10000) {
			dprintk("detected change pending stuck\n");
			return 1;
		}
		rdmsr(MSR_FIDVID_STATUS, lo, hi);
	} while (lo & MSR_S_LO_CHANGE_PENDING);

	data->currvid = hi & MSR_S_HI_CURRENT_VID;
	data->currfid = lo & MSR_S_LO_CURRENT_FID;

	return 0;
}

/* the isochronous relief time */
static void count_off_irt(struct powernow_k8_data *data)
{
	udelay((1 << data->irt) * 10);
	return;
}

/* the voltage stabalization time */
static void count_off_vst(struct powernow_k8_data *data)
{
	udelay(data->vstable * VST_UNITS_20US);
	return;
}

/* need to init the control msr to a safe value (for each cpu) */
static void fidvid_msr_init(void)
{
	u32 lo, hi;
	u8 fid, vid;

	rdmsr(MSR_FIDVID_STATUS, lo, hi);
	vid = hi & MSR_S_HI_CURRENT_VID;
	fid = lo & MSR_S_LO_CURRENT_FID;
	lo = fid | (vid << MSR_C_LO_VID_SHIFT);
	hi = MSR_C_HI_STP_GNT_BENIGN;
	dprintk("cpu%d, init lo 0x%x, hi 0x%x\n", smp_processor_id(), lo, hi);
	wrmsr(MSR_FIDVID_CTL, lo, hi);
}


/* write the new fid value along with the other control fields to the msr */
static int write_new_fid(struct powernow_k8_data *data, u32 fid)
{
	u32 lo;
	u32 savevid = data->currvid;
	u32 i = 0;

	if ((fid & INVALID_FID_MASK) || (data->currvid & INVALID_VID_MASK)) {
		printk(KERN_ERR PFX "internal error - overflow on fid write\n");
		return 1;
	}

	lo = fid | (data->currvid << MSR_C_LO_VID_SHIFT) | MSR_C_LO_INIT_FID_VID;

	dprintk("writing fid 0x%x, lo 0x%x, hi 0x%x\n",
		fid, lo, data->plllock * PLL_LOCK_CONVERSION);

	if (tscsync) {
		int i;
		cpumask_t oldmask = current->cpus_allowed;
		for_each_online_cpu(i) {
			set_cpus_allowed(current, cpumask_of_cpu(i));
			schedule();
			wrmsr(MSR_FIDVID_CTL, lo & ~MSR_C_LO_INIT_FID_VID, data->plllock * PLL_LOCK_CONVERSION);
		}
		set_cpus_allowed(current, oldmask);
		schedule();
	}
	do {
		wrmsr(MSR_FIDVID_CTL, lo, data->plllock * PLL_LOCK_CONVERSION);
		if (i++ > 100) {
			printk(KERN_ERR PFX "Hardware error - pending bit very stuck - no further pstate changes possible\n");
			return 1;
		}
	} while (query_current_values_with_pending_wait(data));

	count_off_irt(data);

	if (savevid != data->currvid) {
		printk(KERN_ERR PFX "vid change on fid trans, old 0x%x, new 0x%x\n",
		       savevid, data->currvid);
		return 1;
	}

	if (fid != data->currfid) {
		printk(KERN_ERR PFX "fid trans failed, fid 0x%x, curr 0x%x\n", fid,
		        data->currfid);
		return 1;
	}

	return 0;
}

/* Write a new vid to the hardware */
static int write_new_vid(struct powernow_k8_data *data, u32 vid)
{
	u32 lo;
	u32 savefid = data->currfid;
	int i = 0;

	if ((data->currfid & INVALID_FID_MASK) || (vid & INVALID_VID_MASK)) {
		printk(KERN_ERR PFX "internal error - overflow on vid write\n");
		return 1;
	}

	lo = data->currfid | (vid << MSR_C_LO_VID_SHIFT) | MSR_C_LO_INIT_FID_VID;

	dprintk("writing vid 0x%x, lo 0x%x, hi 0x%x\n",
		vid, lo, STOP_GRANT_5NS);

	if (tscsync) {
		int i;
		cpumask_t oldmask = current->cpus_allowed;
		for_each_online_cpu(i) {
			set_cpus_allowed(current, cpumask_of_cpu(i));
			schedule();
			wrmsr(MSR_FIDVID_CTL, lo & ~MSR_C_LO_INIT_FID_VID, STOP_GRANT_5NS);
		}
		set_cpus_allowed(current, oldmask);
		schedule();
	}
	do {
		wrmsr(MSR_FIDVID_CTL, lo, STOP_GRANT_5NS);
		if (i++ > 100) {
			printk(KERN_ERR PFX "internal error - pending bit very stuck - no further pstate changes possible\n");
			return 1;
		}
	} while (query_current_values_with_pending_wait(data));

	if (savefid != data->currfid) {
		printk(KERN_ERR PFX "fid changed on vid trans, old 0x%x new 0x%x\n",
		       savefid, data->currfid);
		return 1;
	}

	if (vid != data->currvid) {
		printk(KERN_ERR PFX "vid trans failed, vid 0x%x, curr 0x%x\n", vid,
				data->currvid);
		return 1;
	}

	return 0;
}

/*
 * Reduce the vid by the max of step or reqvid.
 * Decreasing vid codes represent increasing voltages:
 * vid of 0 is 1.550V, vid of 0x1e is 0.800V, vid of VID_OFF is off.
 */
static int decrease_vid_code_by_step(struct powernow_k8_data *data, u32 reqvid, u32 step)
{
	if ((data->currvid - reqvid) > step)
		reqvid = data->currvid - step;

	if (write_new_vid(data, reqvid))
		return 1;

	count_off_vst(data);

	return 0;
}

/* Change hardware pstate by single MSR write */
static int transition_pstate(struct powernow_k8_data *data, u32 pstate)
{
	wrmsr(MSR_PSTATE_CTRL, pstate, 0);
	data->currpstate = pstate;
	return 0;
}

/* Change Opteron/Athlon64 fid and vid, by the 3 phases. */
static int transition_fid_vid(struct powernow_k8_data *data, u32 reqfid, u32 reqvid)
{
	if (core_voltage_pre_transition(data, reqvid))
		return 1;

	if (core_frequency_transition(data, reqfid))
		return 1;

	if (core_voltage_post_transition(data, reqvid))
		return 1;

	if (query_current_values_with_pending_wait(data))
		return 1;

	if ((reqfid != data->currfid) || (reqvid != data->currvid)) {
		printk(KERN_ERR PFX "failed (cpu%d): req 0x%x 0x%x, curr 0x%x 0x%x\n",
				smp_processor_id(),
				reqfid, reqvid, data->currfid, data->currvid);
		return 1;
	}

	dprintk("transitioned (cpu%d): new fid 0x%x, vid 0x%x\n",
		smp_processor_id(), data->currfid, data->currvid);

	return 0;
}

/* Phase 1 - core voltage transition ... setup voltage */
static int core_voltage_pre_transition(struct powernow_k8_data *data, u32 reqvid)
{
	u32 rvosteps = data->rvo;
	u32 savefid = data->currfid;
	u32 maxvid, lo;

	dprintk("ph1 (cpu%d): start, currfid 0x%x, currvid 0x%x, reqvid 0x%x, rvo 0x%x\n",
		smp_processor_id(),
		data->currfid, data->currvid, reqvid, data->rvo);

	rdmsr(MSR_FIDVID_STATUS, lo, maxvid);
	maxvid = 0x1f & (maxvid >> 16);
	dprintk("ph1 maxvid=0x%x\n", maxvid);
	if (reqvid < maxvid) /* lower numbers are higher voltages */
		reqvid = maxvid;

	while (data->currvid > reqvid) {
		dprintk("ph1: curr 0x%x, req vid 0x%x\n",
			data->currvid, reqvid);
		if (decrease_vid_code_by_step(data, reqvid, data->vidmvs))
			return 1;
	}

	while ((rvosteps > 0) && ((data->rvo + data->currvid) > reqvid)) {
		if (data->currvid == maxvid) {
			rvosteps = 0;
		} else {
			dprintk("ph1: changing vid for rvo, req 0x%x\n",
				data->currvid - 1);
			if (decrease_vid_code_by_step(data, data->currvid - 1, 1))
				return 1;
			rvosteps--;
		}
	}

	if (query_current_values_with_pending_wait(data))
		return 1;

	if (savefid != data->currfid) {
		printk(KERN_ERR PFX "ph1 err, currfid changed 0x%x\n", data->currfid);
		return 1;
	}

	dprintk("ph1 complete, currfid 0x%x, currvid 0x%x\n",
		data->currfid, data->currvid);

	return 0;
}

/* Phase 2 - core frequency transition */
static int core_frequency_transition(struct powernow_k8_data *data, u32 reqfid)
{
	u32 vcoreqfid, vcocurrfid, vcofiddiff, fid_interval, savevid = data->currvid;

	if ((reqfid < HI_FID_TABLE_BOTTOM) && (data->currfid < HI_FID_TABLE_BOTTOM)) {
		printk(KERN_ERR PFX "ph2: illegal lo-lo transition 0x%x 0x%x\n",
			reqfid, data->currfid);
		return 1;
	}

	if (data->currfid == reqfid) {
		if (!tscsync)
			printk(KERN_ERR PFX "ph2 null fid transition 0x%x\n", data->currfid);
		return 0;
	}

	dprintk("ph2 (cpu%d): starting, currfid 0x%x, currvid 0x%x, reqfid 0x%x\n",
		smp_processor_id(),
		data->currfid, data->currvid, reqfid);

	vcoreqfid = convert_fid_to_vco_fid(reqfid);
	vcocurrfid = convert_fid_to_vco_fid(data->currfid);
	vcofiddiff = vcocurrfid > vcoreqfid ? vcocurrfid - vcoreqfid
	    : vcoreqfid - vcocurrfid;

	while (vcofiddiff > 2) {
		(data->currfid & 1) ? (fid_interval = 1) : (fid_interval = 2);

		if (reqfid > data->currfid) {
			if (data->currfid > LO_FID_TABLE_TOP) {
				if (write_new_fid(data, data->currfid + fid_interval)) {
					return 1;
				}
			} else {
				if (write_new_fid
				    (data, 2 + convert_fid_to_vco_fid(data->currfid))) {
					return 1;
				}
			}
		} else {
			if (write_new_fid(data, data->currfid - fid_interval))
				return 1;
		}

		vcocurrfid = convert_fid_to_vco_fid(data->currfid);
		vcofiddiff = vcocurrfid > vcoreqfid ? vcocurrfid - vcoreqfid
		    : vcoreqfid - vcocurrfid;
	}

	if (write_new_fid(data, reqfid))
		return 1;

	if (query_current_values_with_pending_wait(data))
		return 1;

	if (data->currfid != reqfid) {
		printk(KERN_ERR PFX
			"ph2: mismatch, failed fid transition, curr 0x%x, req 0x%x\n",
			data->currfid, reqfid);
		return 1;
	}

	if (savevid != data->currvid) {
		printk(KERN_ERR PFX "ph2: vid changed, save 0x%x, curr 0x%x\n",
			savevid, data->currvid);
		return 1;
	}

	dprintk("ph2 complete, currfid 0x%x, currvid 0x%x\n",
		data->currfid, data->currvid);

	return 0;
}

/* Phase 3 - core voltage transition flow ... jump to the final vid. */
static int core_voltage_post_transition(struct powernow_k8_data *data, u32 reqvid)
{
	u32 savefid = data->currfid;
	u32 savereqvid = reqvid;

	dprintk("ph3 (cpu%d): starting, currfid 0x%x, currvid 0x%x\n",
		smp_processor_id(),
		data->currfid, data->currvid);

	if (reqvid != data->currvid) {
		if (write_new_vid(data, reqvid))
			return 1;

		if (savefid != data->currfid) {
			printk(KERN_ERR PFX
			       "ph3: bad fid change, save 0x%x, curr 0x%x\n",
			       savefid, data->currfid);
			return 1;
		}

		if (data->currvid != reqvid) {
			printk(KERN_ERR PFX
			       "ph3: failed vid transition\n, req 0x%x, curr 0x%x",
			       reqvid, data->currvid);
			return 1;
		}
	}

	if (query_current_values_with_pending_wait(data))
		return 1;

	if (savereqvid != data->currvid) {
		dprintk("ph3 failed, currvid 0x%x\n", data->currvid);
		return 1;
	}

	if (savefid != data->currfid) {
		dprintk("ph3 failed, currfid changed 0x%x\n",
			data->currfid);
		return 1;
	}

	dprintk("ph3 complete, currfid 0x%x, currvid 0x%x\n",
		data->currfid, data->currvid);

	return 0;
}

static void check_supported_cpu(void *_rc)
{
	u32 eax, ebx, ecx, edx;
	int *rc = _rc;

	*rc = -ENODEV;

	if (current_cpu_data.x86_vendor != X86_VENDOR_AMD)
		return;

	eax = cpuid_eax(CPUID_PROCESSOR_SIGNATURE);
	if (((eax & CPUID_XFAM) != CPUID_XFAM_K8) &&
	    ((eax & CPUID_XFAM) < CPUID_XFAM_10H))
		return;

	if ((eax & CPUID_XFAM) == CPUID_XFAM_K8) {
		if (((eax & CPUID_USE_XFAM_XMOD) != CPUID_USE_XFAM_XMOD) ||
		    ((eax & CPUID_XMOD) > CPUID_XMOD_REV_G)) {
			printk(KERN_INFO PFX "Processor cpuid %x not supported\n", eax);
			return;
		}

		eax = cpuid_eax(CPUID_GET_MAX_CAPABILITIES);
		if (eax < CPUID_FREQ_VOLT_CAPABILITIES) {
			printk(KERN_INFO PFX
			       "No frequency change capabilities detected\n");
			return;
		}

		cpuid(CPUID_FREQ_VOLT_CAPABILITIES, &eax, &ebx, &ecx, &edx);
		if ((edx & P_STATE_TRANSITION_CAPABLE) != P_STATE_TRANSITION_CAPABLE) {
			printk(KERN_INFO PFX "Power state transitions not supported\n");
			return;
		}
	} else { /* must be a HW Pstate capable processor */
		cpuid(CPUID_FREQ_VOLT_CAPABILITIES, &eax, &ebx, &ecx, &edx);
		if ((edx & USE_HW_PSTATE) == USE_HW_PSTATE)
			cpu_family = CPU_HW_PSTATE;
		else
			return;
	}

	*rc = 0;
}

static int check_pst_table(struct powernow_k8_data *data, struct pst_s *pst, u8 maxvid)
{
	unsigned int j;
	u8 lastfid = 0xff;

	for (j = 0; j < data->numps; j++) {
		if (pst[j].vid > LEAST_VID) {
			printk(KERN_ERR PFX "vid %d invalid : 0x%x\n", j, pst[j].vid);
			return -EINVAL;
		}
		if (pst[j].vid < data->rvo) {	/* vid + rvo >= 0 */
			printk(KERN_ERR BFX "0 vid exceeded with pstate %d\n", j);
			return -ENODEV;
		}
		if (pst[j].vid < maxvid + data->rvo) {	/* vid + rvo >= maxvid */
			printk(KERN_ERR BFX "maxvid exceeded with pstate %d\n", j);
			return -ENODEV;
		}
		if (pst[j].fid > MAX_FID) {
			printk(KERN_ERR BFX "maxfid exceeded with pstate %d\n", j);
			return -ENODEV;
		}
		if (j && (pst[j].fid < HI_FID_TABLE_BOTTOM)) {
			/* Only first fid is allowed to be in "low" range */
			printk(KERN_ERR BFX "two low fids - %d : 0x%x\n", j, pst[j].fid);
			return -EINVAL;
		}
		if (pst[j].fid < lastfid)
			lastfid = pst[j].fid;
	}
	if (lastfid & 1) {
		printk(KERN_ERR BFX "lastfid invalid\n");
		return -EINVAL;
	}
	if (lastfid > LO_FID_TABLE_TOP)
		printk(KERN_INFO BFX  "first fid not from lo freq table\n");

	return 0;
}

static void print_basics(struct powernow_k8_data *data)
{
	int j;
	for (j = 0; j < data->numps; j++) {
		if (data->powernow_table[j].frequency != CPUFREQ_ENTRY_INVALID) {
			if (cpu_family == CPU_HW_PSTATE) {
			printk(KERN_INFO PFX "   %d : fid 0x%x gid 0x%x (%d MHz)\n", j, (data->powernow_table[j].index & 0xff00) >> 8,
				(data->powernow_table[j].index & 0xff0000) >> 16,
				data->powernow_table[j].frequency/1000);
			} else {
			printk(KERN_INFO PFX "   %d : fid 0x%x (%d MHz), vid 0x%x\n", j,
				data->powernow_table[j].index & 0xff,
				data->powernow_table[j].frequency/1000,
				data->powernow_table[j].index >> 8);
			}
		}
	}
	if (data->batps)
		printk(KERN_INFO PFX "Only %d pstates on battery\n", data->batps);
}

static int fill_powernow_table(struct powernow_k8_data *data, struct pst_s *pst, u8 maxvid)
{
	struct cpufreq_frequency_table *powernow_table;
	unsigned int j;

	if (data->batps) {    /* use ACPI support to get full speed on mains power */
		printk(KERN_WARNING PFX "Only %d pstates usable (use ACPI driver for full range\n", data->batps);
		data->numps = data->batps;
	}

	for ( j=1; j<data->numps; j++ ) {
		if (pst[j-1].fid >= pst[j].fid) {
			printk(KERN_ERR PFX "PST out of sequence\n");
			return -EINVAL;
		}
	}

	if (data->numps < 2) {
		printk(KERN_ERR PFX "no p states to transition\n");
		return -ENODEV;
	}

	if (check_pst_table(data, pst, maxvid))
		return -EINVAL;

	powernow_table = kmalloc((sizeof(struct cpufreq_frequency_table)
		* (data->numps + 1)), GFP_KERNEL);
	if (!powernow_table) {
		printk(KERN_ERR PFX "powernow_table memory alloc failure\n");
		return -ENOMEM;
	}

	for (j = 0; j < data->numps; j++) {
		powernow_table[j].index = pst[j].fid; /* lower 8 bits */
		powernow_table[j].index |= (pst[j].vid << 8); /* upper 8 bits */
		powernow_table[j].frequency = find_khz_freq_from_fid(pst[j].fid);
	}
	powernow_table[data->numps].frequency = CPUFREQ_TABLE_END;
	powernow_table[data->numps].index = 0;

	if (query_current_values_with_pending_wait(data)) {
		kfree(powernow_table);
		return -EIO;
	}

	dprintk("cfid 0x%x, cvid 0x%x\n", data->currfid, data->currvid);
	data->powernow_table = powernow_table;
	print_basics(data);

	for (j = 0; j < data->numps; j++)
		if ((pst[j].fid==data->currfid) && (pst[j].vid==data->currvid))
			return 0;

	dprintk("currfid/vid do not match PST, ignoring\n");
	return 0;
}

/* Find and validate the PSB/PST table in BIOS. */
static int find_psb_table(struct powernow_k8_data *data)
{
	struct psb_s *psb;
	unsigned int i;
	u32 mvs;
	u8 maxvid;
	u32 cpst = 0;
	u32 thiscpuid;

	for (i = 0xc0000; i < 0xffff0; i += 0x10) {
		/* Scan BIOS looking for the signature. */
		/* It can not be at ffff0 - it is too big. */

		psb = phys_to_virt(i);
		if (memcmp(psb, PSB_ID_STRING, PSB_ID_STRING_LEN) != 0)
			continue;

		dprintk("found PSB header at 0x%p\n", psb);

		dprintk("table vers: 0x%x\n", psb->tableversion);
		if (psb->tableversion != PSB_VERSION_1_4) {
			printk(KERN_ERR BFX "PSB table is not v1.4\n");
			return -ENODEV;
		}

		dprintk("flags: 0x%x\n", psb->flags1);
		if (psb->flags1) {
			printk(KERN_ERR BFX "unknown flags\n");
			return -ENODEV;
		}

		data->vstable = psb->vstable;
		dprintk("voltage stabilization time: %d(*20us)\n", data->vstable);

		dprintk("flags2: 0x%x\n", psb->flags2);
		data->rvo = psb->flags2 & 3;
		data->irt = ((psb->flags2) >> 2) & 3;
		mvs = ((psb->flags2) >> 4) & 3;
		data->vidmvs = 1 << mvs;
		data->batps = ((psb->flags2) >> 6) & 3;

		dprintk("ramp voltage offset: %d\n", data->rvo);
		dprintk("isochronous relief time: %d\n", data->irt);
		dprintk("maximum voltage step: %d - 0x%x\n", mvs, data->vidmvs);

		dprintk("numpst: 0x%x\n", psb->num_tables);
		cpst = psb->num_tables;
		if ((psb->cpuid == 0x00000fc0) || (psb->cpuid == 0x00000fe0) ){
			thiscpuid = cpuid_eax(CPUID_PROCESSOR_SIGNATURE);
			if ((thiscpuid == 0x00000fc0) || (thiscpuid == 0x00000fe0) ) {
				cpst = 1;
			}
		}
		if (cpst != 1) {
			printk(KERN_ERR BFX "numpst must be 1\n");
			return -ENODEV;
		}

		data->plllock = psb->plllocktime;
		dprintk("plllocktime: 0x%x (units 1us)\n", psb->plllocktime);
		dprintk("maxfid: 0x%x\n", psb->maxfid);
		dprintk("maxvid: 0x%x\n", psb->maxvid);
		maxvid = psb->maxvid;

		data->numps = psb->numps;
		dprintk("numpstates: 0x%x\n", data->numps);
		data->starting_core_affinity = cpumask_of_cpu(0);
		return fill_powernow_table(data, (struct pst_s *)(psb+1), maxvid);
	}
	/*
	 * If you see this message, complain to BIOS manufacturer. If
	 * he tells you "we do not support Linux" or some similar
	 * nonsense, remember that Windows 2000 uses the same legacy
	 * mechanism that the old Linux PSB driver uses. Tell them it
	 * is broken with Windows 2000.
	 *
	 * The reference to the AMD documentation is chapter 9 in the
	 * BIOS and Kernel Developer's Guide, which is available on
	 * www.amd.com
	 */
	printk(KERN_ERR PFX "BIOS error - no PSB or ACPI _PSS objects\n");
	return -ENODEV;
}

#ifdef CONFIG_X86_POWERNOW_K8_ACPI
static void powernow_k8_acpi_pst_values(struct powernow_k8_data *data, unsigned int index)
{
	if (!data->acpi_data->state_count || (cpu_family == CPU_HW_PSTATE))
		return;

	data->irt = (data->acpi_data->states[index].control >> IRT_SHIFT) & IRT_MASK;
	data->rvo = (data->acpi_data->states[index].control >> RVO_SHIFT) & RVO_MASK;
	data->exttype = (data->acpi_data->states[index].control >> EXT_TYPE_SHIFT) & EXT_TYPE_MASK;
	data->plllock = (data->acpi_data->states[index].control >> PLL_L_SHIFT) & PLL_L_MASK;
	data->vidmvs = 1 << ((data->acpi_data->states[index].control >> MVS_SHIFT) & MVS_MASK);
	data->vstable = (data->acpi_data->states[index].control >> VST_SHIFT) & VST_MASK;
}

static struct acpi_processor_performance *acpi_perf_data[NR_CPUS];
static int preregister_valid = 0;

static int powernow_k8_cpu_preinit_acpi(void)
{
	int i;
	struct acpi_processor_performance *data;
	for_each_possible_cpu(i) {
		data = kzalloc(sizeof(struct acpi_processor_performance),
				GFP_KERNEL);
		if (!data) {
			int j;
			for_each_possible_cpu(j) {
				kfree(acpi_perf_data[j]);
				acpi_perf_data[j] = NULL;
			}
			return -ENOMEM;
		}
		acpi_perf_data[i] = data;
	}

	if (preregister_acpi_perf != 0) {
		if (acpi_processor_preregister_performance(acpi_perf_data))
			return -ENODEV;
		else
			preregister_valid = 1;
	} else {
		printk(KERN_INFO "powernow-k8: Disabling ACPI "
		       "pre-initialization.\n");
	}
	return 0;
}

static int powernow_k8_cpu_init_acpi(struct powernow_k8_data *data)
{
	struct cpufreq_frequency_table *powernow_table;
	int ret_val;

	data->acpi_data = acpi_perf_data[data->cpu];
	if (acpi_processor_register_performance(data->acpi_data, data->cpu)) {
		dprintk("register performance failed: bad ACPI data\n");
		return -EIO;
	}

	/* verify the data contained in the ACPI structures */
	if (data->acpi_data->state_count <= 1) {
		dprintk("No ACPI P-States\n");
		goto err_out;
	}

	if ((data->acpi_data->control_register.space_id != ACPI_ADR_SPACE_FIXED_HARDWARE) ||
		(data->acpi_data->status_register.space_id != ACPI_ADR_SPACE_FIXED_HARDWARE)) {
		dprintk("Invalid control/status registers (%x - %x)\n",
			data->acpi_data->control_register.space_id,
			data->acpi_data->status_register.space_id);
		goto err_out;
	}

	/* fill in data->powernow_table */
	powernow_table = kmalloc((sizeof(struct cpufreq_frequency_table)
		* (data->acpi_data->state_count + 1)), GFP_KERNEL);
	if (!powernow_table) {
		dprintk("powernow_table memory alloc failure\n");
		goto err_out;
	}

	if (cpu_family == CPU_HW_PSTATE)
		ret_val = fill_powernow_table_pstate(data, powernow_table);
	else
		ret_val = fill_powernow_table_fidvid(data, powernow_table);
	if (ret_val)
		goto err_out_mem;

	powernow_table[data->acpi_data->state_count].frequency = CPUFREQ_TABLE_END;
	powernow_table[data->acpi_data->state_count].index = 0;
	data->powernow_table = powernow_table;

	/* fill in data */
	data->numps = data->acpi_data->state_count;
	if (first_cpu(cpu_core_map[data->cpu]) == data->cpu)
		print_basics(data);
	powernow_k8_acpi_pst_values(data, 0);

	/* notify BIOS that we exist */
	acpi_processor_notify_smm(THIS_MODULE);

	/* determine affinity, from ACPI if available */
	if (preregister_valid) {
		if ((data->acpi_data->shared_type == CPUFREQ_SHARED_TYPE_ALL) ||
		    (data->acpi_data->shared_type == CPUFREQ_SHARED_TYPE_ANY))
			data->starting_core_affinity = data->acpi_data->shared_cpu_map;
		else
			data->starting_core_affinity = cpumask_of_cpu(data->cpu);
	} else {
		/* best guess from family if not */
		if (cpu_family == CPU_HW_PSTATE)
			data->starting_core_affinity = cpumask_of_cpu(data->cpu);
		else
			data->starting_core_affinity = cpu_core_map[data->cpu];
	}

	return 0;

err_out_mem:
	kfree(powernow_table);

err_out:
	acpi_processor_unregister_performance(data->acpi_data, data->cpu);

	/* data->acpi_data->state_count informs us at ->exit() whether ACPI was used */
	data->acpi_data->state_count = 0;

	return -ENODEV;
}

static int fill_powernow_table_pstate(struct powernow_k8_data *data, struct cpufreq_frequency_table *powernow_table)
{
	int i;
	u32 hi = 0, lo = 0;
	rdmsr(MSR_PSTATE_CUR_LIMIT, hi, lo);
	data->max_hw_pstate = (hi & HW_PSTATE_MAX_MASK) >> HW_PSTATE_MAX_SHIFT;

	for (i = 0; i < data->acpi_data->state_count; i++) {
		u32 index;

		index = data->acpi_data->states[i].control & HW_PSTATE_MASK;
		if (index > data->max_hw_pstate) {
			printk(KERN_ERR PFX "invalid pstate %d - bad value %d.\n", i, index);
			printk(KERN_ERR PFX "Please report to BIOS manufacturer\n");
			continue;
		}
		powernow_table[i].index = index;
		powernow_table[i].frequency = data->acpi_data->states[i].core_frequency * 1000;

	}
	return 0;
}

static int fill_powernow_table_fidvid(struct powernow_k8_data *data, struct cpufreq_frequency_table *powernow_table)
{
	int i;
	int cntlofreq = 0;
	for (i = 0; i < data->acpi_data->state_count; i++) {
		u32 fid;
		u32 vid;

		if (data->exttype) {
			fid = data->acpi_data->states[i].status & EXT_FID_MASK;
			vid = (data->acpi_data->states[i].status >> VID_SHIFT) & EXT_VID_MASK;
		} else {
			fid = data->acpi_data->states[i].control & FID_MASK;
			vid = (data->acpi_data->states[i].control >> VID_SHIFT) & VID_MASK;
		}

		dprintk("   %d : fid 0x%x, vid 0x%x\n", i, fid, vid);

		powernow_table[i].index = fid; /* lower 8 bits */
		powernow_table[i].index |= (vid << 8); /* upper 8 bits */
		powernow_table[i].frequency = find_khz_freq_from_fid(fid);

		/* verify frequency is OK */
		if ((powernow_table[i].frequency > (MAX_FREQ * 1000)) ||
			(powernow_table[i].frequency < (MIN_FREQ * 1000))) {
			dprintk("invalid freq %u kHz, ignoring\n", powernow_table[i].frequency);
			powernow_table[i].frequency = CPUFREQ_ENTRY_INVALID;
			continue;
		}

		/* verify voltage is OK - BIOSs are using "off" to indicate invalid */
		if (vid == VID_OFF) {
			dprintk("invalid vid %u, ignoring\n", vid);
			powernow_table[i].frequency = CPUFREQ_ENTRY_INVALID;
			continue;
		}

		/* verify only 1 entry from the lo frequency table */
		if (fid < HI_FID_TABLE_BOTTOM) {
			if (cntlofreq) {
				/* if both entries are the same, ignore this one ... */
				if ((powernow_table[i].frequency != powernow_table[cntlofreq].frequency) ||
				    (powernow_table[i].index != powernow_table[cntlofreq].index)) {
					printk(KERN_ERR PFX "Too many lo freq table entries\n");
					return 1;
				}

				dprintk("double low frequency table entry, ignoring it.\n");
				powernow_table[i].frequency = CPUFREQ_ENTRY_INVALID;
				continue;
			} else
				cntlofreq = i;
		}

		if (powernow_table[i].frequency != (data->acpi_data->states[i].core_frequency * 1000)) {
			printk(KERN_INFO PFX "invalid freq entries %u kHz vs. %u kHz\n",
				powernow_table[i].frequency,
				(unsigned int) (data->acpi_data->states[i].core_frequency * 1000));
			powernow_table[i].frequency = CPUFREQ_ENTRY_INVALID;
			continue;
		}
	}
	return 0;
}

static void powernow_k8_cpu_exit_acpi(struct powernow_k8_data *data)
{
	if (data->acpi_data->state_count)
		acpi_processor_unregister_performance(data->acpi_data, data->cpu);
}

#else
static int powernow_k8_cpu_init_acpi(struct powernow_k8_data *data) { return -ENODEV; }
static void powernow_k8_cpu_exit_acpi(struct powernow_k8_data *data) { return; }
static void powernow_k8_acpi_pst_values(struct powernow_k8_data *data, unsigned int index) { return; }
static int powernow_k8_cpu_preinit_acpi() { return -ENODEV; }
#endif /* CONFIG_X86_POWERNOW_K8_ACPI */

/* Take a frequency, and issue the fid/vid transition command */
static int transition_frequency_fidvid(struct powernow_k8_data *data, unsigned int index)
{
	u32 fid = 0;
	u32 vid = 0;
	int res, i;
	struct cpufreq_freqs freqs;
	cpumask_t changing_cores;

	dprintk("cpu %d transition to index %u\n", smp_processor_id(), index);

	/* if all processors are transitioning in step, find the highest
	 * current state and go to that
         */

	if (tscsync && req_state) {
		req_state[smp_processor_id()] = index;
		for_each_online_cpu(i) 
			if (req_state[i] < index)
				index = req_state[i];
	}

	/* fid/vid correctness check for k8 */
	/* fid are the lower 8 bits of the index we stored into
	 * the cpufreq frequency table in find_psb_table, vid
	 * are the upper 8 bits.
	 */
	fid = data->powernow_table[index].index & 0xFF;
	vid = (data->powernow_table[index].index & 0xFF00) >> 8;

	dprintk("table matched fid 0x%x, giving vid 0x%x\n", fid, vid);

	if (query_current_values_with_pending_wait(data))
		return 1;

	if ((data->currvid == vid) && (data->currfid == fid)) {
		dprintk("target matches current values (fid 0x%x, vid 0x%x)\n",
			fid, vid);
		return 0;
	}

	if ((fid < HI_FID_TABLE_BOTTOM) && (data->currfid < HI_FID_TABLE_BOTTOM)) {
		if (tscsync && (data->currfid == fid))
			return 0;
		printk(KERN_ERR PFX
		       "ignoring illegal change in lo freq table-%x to 0x%x\n",
		       data->currfid, fid);
		return 1;
	}

	dprintk("cpu %d, changing to fid 0x%x, vid 0x%x\n",
		smp_processor_id(), fid, vid);
	freqs.old = find_khz_freq_from_fid(data->currfid);
	freqs.new = find_khz_freq_from_fid(fid);

	if (tscsync)
		changing_cores = cpu_online_map;
	else
		changing_cores = *(data->available_cores);
	for_each_cpu_mask(i, changing_cores) {
		freqs.cpu = i;
		cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);
	}

	res = transition_fid_vid(data, fid, vid);
	freqs.new = find_khz_freq_from_fid(data->currfid);

	for_each_cpu_mask(i, changing_cores) {
		freqs.cpu = i;
		cpufreq_notify_transition(&freqs, CPUFREQ_POSTCHANGE);
	}
	if (tscsync) 
		for_each_online_cpu(i)
			if (per_cpu(powernow_data, i)) {
				per_cpu(powernow_data, i)->currfid = data->currfid;
				per_cpu(powernow_data, i)->currvid = data->currvid;
			}
	return res;
}

/* Take a frequency, and issue the hardware pstate transition command */
static int transition_frequency_pstate(struct powernow_k8_data *data, unsigned int index)
{
	u32 pstate = 0;
	int res, i;
	struct cpufreq_freqs freqs;

	dprintk("cpu %d transition to index %u\n", smp_processor_id(), index);

	/* get MSR index for hardware pstate transition */
	pstate = index & HW_PSTATE_MASK;
	if (pstate > data->max_hw_pstate)
		return 0;
	freqs.old = find_khz_freq_from_pstate(data->powernow_table, data->currpstate);
	freqs.new = find_khz_freq_from_pstate(data->powernow_table, pstate);

	for_each_cpu_mask(i, *(data->available_cores)) {
		freqs.cpu = i;
		cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);
	}

	res = transition_pstate(data, pstate);
	freqs.new = find_khz_freq_from_pstate(data->powernow_table, pstate);

	for_each_cpu_mask(i, *(data->available_cores)) {
		freqs.cpu = i;
		cpufreq_notify_transition(&freqs, CPUFREQ_POSTCHANGE);
	}
	return res;
}

/* Driver entry point to switch to the target frequency */
static int powernowk8_target(struct cpufreq_policy *pol, unsigned targfreq, unsigned relation)
{
	cpumask_t oldmask = CPU_MASK_ALL;
	struct powernow_k8_data *data = per_cpu(powernow_data, pol->cpu);
	u32 checkfid;
	u32 checkvid;
	unsigned int newstate;
	int ret = -EIO;

	if (!data)
		return -EINVAL;

	checkfid = data->currfid;
	checkvid = data->currvid;

	/* only run on specific CPU from here on */
	oldmask = current->cpus_allowed;
	set_cpus_allowed(current, cpumask_of_cpu(pol->cpu));

	if (smp_processor_id() != pol->cpu) {
		printk(KERN_ERR PFX "limiting to cpu %u failed\n", pol->cpu);
		goto err_out;
	}

	if (pending_bit_stuck()) {
		printk(KERN_ERR PFX "failing targ, change pending bit set\n");
		goto err_out;
	}

	dprintk("targ: cpu %d, %d kHz, min %d, max %d, relation %d\n",
		pol->cpu, targfreq, pol->min, pol->max, relation);

	if (query_current_values_with_pending_wait(data))
		goto err_out;

	if (cpu_family != CPU_HW_PSTATE) {
		dprintk("targ: curr fid 0x%x, vid 0x%x\n",
		data->currfid, data->currvid);

		if (!tscsync && ((checkvid != data->currvid) || (checkfid != data->currfid))) {
			printk(KERN_INFO PFX
				"error - out of sync, fix 0x%x 0x%x, vid 0x%x 0x%x\n",
				checkfid, data->currfid, checkvid, data->currvid);
		}
	}

	if (cpufreq_frequency_table_target(pol, data->powernow_table, targfreq, relation, &newstate))
		goto err_out;

	mutex_lock(&fidvid_mutex);
	powernow_k8_acpi_pst_values(data, newstate);

	if (cpu_family == CPU_HW_PSTATE)
		ret = transition_frequency_pstate(data, newstate);
	else
		ret = transition_frequency_fidvid(data, newstate);
	if (ret) {
		printk(KERN_ERR PFX "transition frequency failed\n");
		ret = 1;
		mutex_unlock(&fidvid_mutex);
		goto err_out;
	}
	mutex_unlock(&fidvid_mutex);

	if (cpu_family == CPU_HW_PSTATE)
		pol->cur = find_khz_freq_from_pstate(data->powernow_table, newstate);
	else
		pol->cur = find_khz_freq_from_fid(data->currfid);
	ret = 0;

err_out:
	set_cpus_allowed(current, oldmask);
	return ret;
}

/* Driver entry point to verify the policy and range of frequencies */
static int powernowk8_verify(struct cpufreq_policy *pol)
{
	struct powernow_k8_data *data = per_cpu(powernow_data, pol->cpu);

	if (!data)
		return -EINVAL;

	return cpufreq_frequency_table_verify(pol, data->powernow_table);
}

#ifdef CONFIG_SMP
/* On an MP system that is transitioning all cores in sync, adjust the
 * vids for each frequency to the highest.  Otherwise, systems made up
 * of different steppings may fail.
 */
static void sync_tables(int curcpu)
{
	int j;
	for (j = 0; j < per_cpu(powernow_data, curcpu)->numps; j++) {
		int i;
		int maxvid = 0;
		for_each_online_cpu(i) {
			int testvid;
			if (!per_cpu(powernow_data, i) || !per_cpu(powernow_data, i)->powernow_table)
				continue;
			testvid = per_cpu(powernow_data, i)->powernow_table[j].index & 0xff00;
			if (testvid > maxvid)
				maxvid = testvid;
		}	
		for_each_online_cpu(i) {
			if (!per_cpu(powernow_data, i) || !per_cpu(powernow_data, i)->powernow_table)
				continue;
			per_cpu(powernow_data, i)->powernow_table[j].index &= 0xff;
			per_cpu(powernow_data, i)->powernow_table[j].index |= maxvid;
		}
	}
}
#endif

struct init_on_cpu {
	struct powernow_k8_data *data;
	int rc;
};

static void __cpuinit powernowk8_cpu_init_on_cpu(void *_init_on_cpu)
{
	struct init_on_cpu *init_on_cpu = _init_on_cpu;

	if (pending_bit_stuck()) {
		printk(KERN_ERR PFX "failing init, change pending bit set\n");
		init_on_cpu->rc = -ENODEV;
		return;
	}

	if (query_current_values_with_pending_wait(init_on_cpu->data)) {
		init_on_cpu->rc = -ENODEV;
		return;
	}

	if (cpu_family == CPU_OPTERON)
		fidvid_msr_init();

	init_on_cpu->rc = 0;
}

/* per CPU init entry point to the driver */
static int __cpuinit powernowk8_cpu_init(struct cpufreq_policy *pol)
{
	struct powernow_k8_data *data;
	struct init_on_cpu init_on_cpu;
	int rc;

	if (!cpu_online(pol->cpu))
		return -ENODEV;

	smp_call_function_single(pol->cpu, check_supported_cpu, &rc, 1, 1);
	if (rc)
		return -ENODEV;

	data = kzalloc(sizeof(struct powernow_k8_data), GFP_KERNEL);
	if (!data) {
		printk(KERN_ERR PFX "unable to alloc powernow_k8_data");
		return -ENOMEM;
	}

	data->cpu = pol->cpu;
	data->currpstate = HW_PSTATE_INVALID;

	if (powernow_k8_cpu_init_acpi(data)) {
		/*
		 * Use the PSB BIOS structure. This is only availabe on
		 * an UP version, and is deprecated by AMD.
		 */
		if (num_online_cpus() != 1) {
			printk(KERN_ERR PFX "Your BIOS does not provide _PSS objects.  PowerNow! does not work on SMP systems without _PSS objects.  Complain to your BIOS vendor.\n");
			kfree(data);
			return -ENODEV;
		}
		if (pol->cpu != 0) {
			printk(KERN_ERR PFX "No _PSS objects for CPU other than CPU0\n");
			kfree(data);
			return -ENODEV;
		}
		rc = find_psb_table(data);
		if (rc) {
			kfree(data);
			return -ENODEV;
		}
	}

	/* only run on specific CPU from here on */
	init_on_cpu.data = data;
	smp_call_function_single(data->cpu, powernowk8_cpu_init_on_cpu,
				 &init_on_cpu, 1, 1);
	rc = init_on_cpu.rc;
	if (rc != 0)
		goto err_out_exit_acpi;

	pol->governor = CPUFREQ_DEFAULT_GOVERNOR;
	pol->cpus = data->starting_core_affinity;
	data->available_cores = &(pol->cpus);

	/* Take a crude guess here.
	 * That guess was in microseconds, so multiply with 1000 */
	pol->cpuinfo.transition_latency = (((data->rvo + 8) * data->vstable * VST_UNITS_20US)
	    + (3 * (1 << data->irt) * 10)) * 1000;

	if (cpu_family == CPU_HW_PSTATE)
		pol->cur = find_khz_freq_from_pstate(data->powernow_table, data->currpstate);
	else
		pol->cur = find_khz_freq_from_fid(data->currfid);
	dprintk("policy current frequency %d kHz\n", pol->cur);

	/* min/max the cpu is capable of */
	if (cpufreq_frequency_table_cpuinfo(pol, data->powernow_table)) {
		printk(KERN_ERR PFX "invalid powernow_table\n");
		powernow_k8_cpu_exit_acpi(data);
		kfree(data->powernow_table);
		kfree(data);
		return -EINVAL;
	}

	cpufreq_frequency_table_get_attr(data->powernow_table, pol->cpu);

	if (cpu_family == CPU_HW_PSTATE)
		dprintk("cpu_init done, current pstate 0x%x\n", data->currpstate);
	else
		dprintk("cpu_init done, current fid 0x%x, vid 0x%x\n",
			data->currfid, data->currvid);

	per_cpu(powernow_data, pol->cpu) = data;

#ifdef CONFIG_SMP
	if (tscsync && (cpu_family == CPU_OPTERON)) {
		u32 reg;
		struct cpuinfo_x86 *c = &cpu_data[pol->cpu];
		sync_tables(pol->cpu);
		reg = read_pci_config(0, NB_PCI_ADDR + c->cpu_core_id,
				      NB_PM_DEV, NB_C1_REG);
		/* turn off C1 clock ramping */
		write_pci_config(0, NB_PCI_ADDR + c->cpu_core_id,
				 NB_PM_DEV, NB_C1_REG, reg & NB_C1_MASK);
	}
#endif
	return 0;

err_out_exit_acpi:
	powernow_k8_cpu_exit_acpi(data);

	kfree(data);
	return -ENODEV;
}

static int __devexit powernowk8_cpu_exit (struct cpufreq_policy *pol)
{
	struct powernow_k8_data *data = per_cpu(powernow_data, pol->cpu);

	if (!data)
		return -EINVAL;

	powernow_k8_cpu_exit_acpi(data);

	cpufreq_frequency_table_put_attr(pol->cpu);

	kfree(data->powernow_table);
	kfree(data);

	return 0;
}

static void query_values_on_cpu(void *_err)
{
	int *err = _err;
	struct powernow_k8_data *data = __get_cpu_var(powernow_data);

	*err = query_current_values_with_pending_wait(data);
}

static unsigned int powernowk8_get (unsigned int cpu)
{
	struct powernow_k8_data *data = per_cpu(powernow_data, cpu);
	unsigned int khz = 0;
	int err;

	if (!data)
		return -EINVAL;

	smp_call_function_single(cpu, query_values_on_cpu, &err, 1, true);
	if (err)
		goto out;

	if (cpu_family == CPU_HW_PSTATE)
		khz = find_khz_freq_from_pstate(data->powernow_table, data->currpstate);
	else
		khz = find_khz_freq_from_fid(data->currfid);

out:
	return khz;
}

static struct freq_attr* powernow_k8_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
	NULL,
};

static struct cpufreq_driver cpufreq_amd64_driver = {
	.verify = powernowk8_verify,
	.target = powernowk8_target,
	.init = powernowk8_cpu_init,
	.exit = __devexit_p(powernowk8_cpu_exit),
	.get = powernowk8_get,
	.name = "powernow-k8",
	.owner = THIS_MODULE,
	.attr = powernow_k8_attr,
};

/* driver entry point for init */
static int __cpuinit powernowk8_init(void)
{
	unsigned int i, supported_cpus = 0;

#ifdef CONFIG_XEN
	if (!is_initial_xendomain()) {
		/* Xen PV domU's can't possibly do powersaving; bail */
		return -EPERM;
	}
#endif

	for_each_online_cpu(i) {
		int rc;
		smp_call_function_single(i, check_supported_cpu, &rc, 1, 1);
		if (rc == 0)
			supported_cpus++;
	}

	if (supported_cpus != num_online_cpus())
		return -ENODEV;

	/* AMD provides AGESA library modules for use in their BIOS. The
	   default AGESA code creates the _PSD with the assumption the APICs
	   are numbered per the BKDG HOWEVER, there is a callback
	   (ibvPSDApicIDtoNode) to set the APIC ID to node translation for _PSD
	   dependency domains if the system numbers the APICs differently.

	   It looks like HP did not follow spec on both fronts (it numbered
	   differently from the BKDG as	well as did not implement the callback
	   to set the domains properly).

	   AMD reports that HP is the only vendor to implement CPU enumeration
	   this way. */
	if (preregister_acpi_perf == 1 && cpu_family == CPU_OPTERON) {
		char * dmi_data = dmi_get_system_info(DMI_BIOS_VENDOR);
		if (dmi_data && !strncmp(dmi_data, "Hewlett-Packard", 15)) {
			/* Disable preregistering ACPI data for HP AMD Opteron
			   systems */
			preregister_acpi_perf = 0;
		}
	}


#ifndef CONFIG_SMP
	tscsync = 0;
#endif

	if (tscsync) {
		req_state = kzalloc(sizeof(int)*NR_CPUS, GFP_KERNEL);
		if (!req_state) {
			printk(KERN_ERR PFX "Unable to allocate memory!\n");
			return -ENOMEM;
		}
		/* necessary for dual-cores (99=just a large number) */
		for(i=0; i < num_possible_cpus(); i++)
			req_state[i] = 99;
	}
	if (powernow_k8_cpu_preinit_acpi())
		printk(KERN_ERR PFX "Pre-initialization of ACPI failed\n");
#ifndef CONFIG_XEN
#ifdef CONFIG_SMP
	printk(KERN_INFO PFX "Found %d %s "
	       "processors (%d cpu cores) (" VERSION ")\n",
	       supported_cpus/cpu_data[0].booted_cores,
	       boot_cpu_data.x86_model_id, supported_cpus);
#else
	printk(KERN_INFO PFX "Found 1 %s "
	       "processors (%d cpu cores) (" VERSION ")\n",
	       boot_cpu_data.x86_model_id, supported_cpus);
#endif
#else
	printk(KERN_INFO PFX "Found %d virtualized processors\n",
		supported_cpus);
#endif
	return cpufreq_register_driver(&cpufreq_amd64_driver);
}

/* driver entry point for term */
static void __exit powernowk8_exit(void)
{
	dprintk("exit\n");

	if (tscsync)
		kfree(req_state);

	cpufreq_unregister_driver(&cpufreq_amd64_driver);
}

MODULE_AUTHOR("Paul Devriendt <paul.devriendt@amd.com> and Mark Langsdorf <mark.langsdorf@amd.com>");
MODULE_DESCRIPTION("AMD Athlon 64 and Opteron processor frequency driver.");
MODULE_LICENSE("GPL");

late_initcall(powernowk8_init);
module_exit(powernowk8_exit);

module_param(tscsync, int, 0444);
MODULE_PARM_DESC(tscsync, "enable tsc by synchronizing powernow-k8 changes");
module_param(preregister_acpi_perf, int, 0444);
MODULE_PARM_DESC(preregister_acpi_perf, "allow preregistering of performance"
		 " related ACPI data");
