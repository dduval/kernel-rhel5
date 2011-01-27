/*
 *  arch/s390/kernel/topology.c
 *
 *    Copyright IBM Corp. 2007
 *    Author(s): Heiko Carstens <heiko.carstens@de.ibm.com>
 */

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/bootmem.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/smp.h>
#include <asm/delay.h>
#include <asm/s390_ext.h>
#include <asm/sysinfo.h>

static DEFINE_MUTEX(smp_cpu_state_mutex);

#define CPU_BITS 64

struct tl_cpu {
	unsigned char reserved[6];
	unsigned short origin;
	unsigned long mask[CPU_BITS / BITS_PER_LONG];
};

struct tl_container {
	unsigned char reserved[8];
};

union tl_entry {
	unsigned char nl;
	struct tl_cpu cpu;
	struct tl_container container;
};

#define NR_MAG 6

struct tl_info {
	unsigned char reserved0[2];
	unsigned short length;
	unsigned char mag[NR_MAG];
	unsigned char reserved1;
	unsigned char mnest;
	unsigned char reserved2[4];
	union tl_entry tle[0];
};

struct core_info {
	struct core_info *next;
	cpumask_t mask;
};

static int topology_enabled;
static void topology_work_fn(void *data);
static struct tl_info *tl_info;
static struct core_info core_info;
static int machine_has_topology;
static int machine_has_topology_irq;
static struct timer_list topology_timer;
static void set_topology_timer(void);
static struct work_struct topology_work;

cpumask_t cpu_coregroup_map(unsigned int cpu)
{
	struct core_info *core = &core_info;
	cpumask_t mask;

	cpus_clear(mask);
	if (!machine_has_topology)
		return cpu_present_map;
	mutex_lock(&smp_cpu_state_mutex);
	while (core) {
		if (cpu_isset(cpu, core->mask)) {
			mask = core->mask;
			break;
		}
		core = core->next;
	}
	mutex_unlock(&smp_cpu_state_mutex);
	if (cpus_empty(mask))
		mask = cpumask_of_cpu(cpu);
	return mask;
}

static void add_cpus_to_core(struct tl_cpu *tl_cpu, struct core_info *core)
{
	unsigned int cpu;

	for (cpu = find_first_bit(&tl_cpu->mask[0], CPU_BITS);
	     cpu < CPU_BITS;
	     cpu = find_next_bit(&tl_cpu->mask[0], CPU_BITS, cpu + 1))
	{
		unsigned int rcpu, lcpu;

		rcpu = CPU_BITS - 1 - cpu + tl_cpu->origin;
		for_each_present_cpu(lcpu) {
			if (__cpu_logical_map[lcpu] == rcpu)
				cpu_set(lcpu, core->mask);
		}
	}
}

static void clear_cores(void)
{
	struct core_info *core = &core_info;

	while (core) {
		cpus_clear(core->mask);
		core = core->next;
	}
}

static union tl_entry *next_tle(union tl_entry *tle)
{
	if (tle->nl)
		return (union tl_entry *)((struct tl_container *)tle + 1);
	else
		return (union tl_entry *)((struct tl_cpu *)tle + 1);
}

static void tl_to_cores(struct tl_info *info)
{
	union tl_entry *tle, *end;
	struct core_info *core = &core_info;

	mutex_lock(&smp_cpu_state_mutex);
	clear_cores();
	tle = (union tl_entry *)&info->tle;
	end = (union tl_entry *)((unsigned long)info + info->length);
	while (tle < end) {
		switch (tle->nl) {
		case 5:
		case 4:
		case 3:
		case 2:
			break;
		case 1:
			core = core->next;
			break;
		case 0:
			add_cpus_to_core(&tle->cpu, core);
			break;
		default:
			clear_cores();
			machine_has_topology = 0;
			return;
		}
		tle = next_tle(tle);
	}
	mutex_unlock(&smp_cpu_state_mutex);
}

static int ptf(void)
{
	int rc;

	asm volatile(
		"	.insn	rre,0xb9a20000,%1,%1\n"
		"	ipm	%0\n"
		"	srl	%0,28\n"
		: "=d" (rc)
		: "d" (2UL)  : "cc");
	return rc;
}

void arch_update_cpu_topology(void)
{
	struct tl_info *info = tl_info;
	struct sys_device *sysdev;
	int cpu;

	if (!machine_has_topology)
		return;
	ptf();
	stsi(info, 15, 1, 2);
	tl_to_cores(info);
	for_each_online_cpu(cpu) {
		sysdev = get_cpu_sysdev(cpu);
		kobject_uevent(&sysdev->kobj, KOBJ_CHANGE);
	}
}

static int topology_kthread(void *data)
{
	arch_reinit_sched_domains();
	return 0;
}

static void topology_work_fn(void *data)
{
	/* We can't call arch_reinit_sched_domains() from a multi-threaded
	 * workqueue context since it may deadlock in case of cpu hotplug.
	 * So we have to create a kernel thread in order to call
	 * arch_reinit_sched_domains().
	 */
	kthread_run(topology_kthread, NULL, "topology_update");
}

static void topology_timer_fn(unsigned long ignored)
{
	if (ptf())
		schedule_work(&topology_work);
	set_topology_timer();
}

static void set_topology_timer(void)
{
	topology_timer.function = topology_timer_fn;
	topology_timer.data = 0;
	topology_timer.expires = jiffies + 60 * HZ;
	add_timer(&topology_timer);
}

static void topology_interrupt(struct pt_regs *regs, __u16 code)
{
	schedule_work(&topology_work);
}

static int __init early_parse_topology(char *p)
{
	if (strncmp(p, "on", 2))
		return 0;
	topology_enabled = 1;
	return 0;
}
early_param("topology", early_parse_topology);

static int __init init_topology_update(void)
{
	int rc;

	if (!machine_has_topology)
		return 0;
	init_timer(&topology_timer);
	if (machine_has_topology_irq) {
		rc = register_external_interrupt(0x2005, topology_interrupt);
		if (rc)
			return rc;
		ctl_set_bit(0, 8);
	}
	else
		set_topology_timer();
	return 0;
}
__initcall(init_topology_update);

void __init s390_init_cpu_topology(void)
{
	unsigned long long facility_bits;
	struct tl_info *info;
	struct core_info *core;
	int nr_cores;
	int i;

	INIT_WORK(&topology_work, topology_work_fn, NULL);
	if (!topology_enabled)
		return;
	if (stfle(&facility_bits, 1) <= 0)
		return;
	if (!(facility_bits & (1ULL << 52)))
		return;
	machine_has_topology = 1;

	if (facility_bits & (1ULL << 51))
		machine_has_topology_irq = 1;

	tl_info = alloc_bootmem_pages(PAGE_SIZE);
	if (!tl_info)
		goto error;
	info = tl_info;
	stsi(info, 15, 1, 2);

	nr_cores = info->mag[NR_MAG - 2];
	for (i = 0; i < info->mnest - 2; i++)
		nr_cores *= info->mag[NR_MAG - 3 - i];

	printk(KERN_INFO "CPU topology:");
	for (i = 0; i < NR_MAG; i++)
		printk(" %d", info->mag[i]);
	printk(" / %d\n", info->mnest);

	core = &core_info;
	for (i = 0; i < nr_cores; i++) {
		core->next = alloc_bootmem(sizeof(struct core_info));
		core = core->next;
		if (!core)
			goto error;
	}
	return;
error:
	machine_has_topology = 0;
	machine_has_topology_irq = 0;
}
