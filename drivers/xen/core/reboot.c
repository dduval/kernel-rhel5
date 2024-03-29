#define __KERNEL_SYSCALLS__
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/unistd.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/sysrq.h>
#include <linux/stringify.h>
#include <asm/irq.h>
#include <asm/mmu_context.h>
#include <xen/evtchn.h>
#include <asm/hypervisor.h>
#include <xen/interface/dom0_ops.h>
#include <xen/xenbus.h>
#include <linux/cpu.h>
#include <linux/kthread.h>
#include <xen/gnttab.h>
#include <xen/xencons.h>
#include <xen/cpu_hotplug.h>

#ifdef HAVE_XEN_PLATFORM_COMPAT_H
#include <xen/platform-compat.h>
#endif

#define SHUTDOWN_INVALID  -1
#define SHUTDOWN_POWEROFF  0
#define SHUTDOWN_SUSPEND   2
/* Code 3 is SHUTDOWN_CRASH, which we don't use because the domain can only
 * report a crash, not be instructed to crash!
 * HALT is the same as POWEROFF, as far as we're concerned.  The tools use
 * the distinction when we return the reason code to them.
 */
#define SHUTDOWN_HALT      4

#ifdef CONFIG_XEN /* non-pv-on-hvm */
#if defined(__i386__) || defined(__x86_64__)

/*
 * Power off function, if any
 */
void (*pm_power_off)(void);
EXPORT_SYMBOL(pm_power_off);

void machine_emergency_restart(void)
{
	/* We really want to get pending console data out before we die. */
	xencons_force_flush();
	HYPERVISOR_shutdown(SHUTDOWN_reboot);
}

void machine_restart(char * __unused)
{
	machine_emergency_restart();
}

void machine_halt(void)
{
	machine_power_off();
}

void machine_power_off(void)
{
	/* We really want to get pending console data out before we die. */
	xencons_force_flush();
	if (pm_power_off)
		pm_power_off();
	HYPERVISOR_shutdown(SHUTDOWN_poweroff);
}

int reboot_thru_bios = 0;	/* for dmi_scan.c */
EXPORT_SYMBOL(machine_restart);
EXPORT_SYMBOL(machine_halt);
EXPORT_SYMBOL(machine_power_off);

#endif /* defined(__i386__) || defined(__x86_64__) */
#endif /* CONFIG_XEN */

/******************************************************************************
 * Stop/pickle callback handling.
 */

/* Ignore multiple shutdown requests. */
static int shutting_down = SHUTDOWN_INVALID;

/* Can we leave APs online when we suspend? */
static int fast_suspend;

static void __shutdown_handler(void *unused);
static DECLARE_WORK(shutdown_work, __shutdown_handler, NULL);

#ifdef CONFIG_XEN
#if defined(__i386__) || defined(__x86_64__)

/* Ensure we run on the idle task page tables so that we will
   switch page tables before running user space. This is needed
   on architectures with separate kernel and user page tables
   because the user page table pointer is not saved/restored. */
static void switch_idle_mm(void)
{
	struct mm_struct *mm = current->active_mm;

	if (mm == &init_mm)
		return;

	atomic_inc(&init_mm.mm_count);
	switch_mm(mm, &init_mm, current);
	current->active_mm = &init_mm;
	mmdrop(mm);
}

static void pre_suspend(void)
{
	HYPERVISOR_shared_info = (shared_info_t *)empty_zero_page;
	clear_fixmap(FIX_SHARED_INFO);

	xen_start_info->store_mfn = mfn_to_pfn(xen_start_info->store_mfn);
	xen_start_info->console.domU.mfn =
		mfn_to_pfn(xen_start_info->console.domU.mfn);
}

static void post_suspend(void)
{
	int i, j, k, fpp;
	extern unsigned long max_pfn;
	extern unsigned long *pfn_to_mfn_frame_list_list;
	extern unsigned long *pfn_to_mfn_frame_list[];

#ifdef CONFIG_SMP
	cpu_initialized_map = cpu_online_map;
#endif

	set_fixmap(FIX_SHARED_INFO, xen_start_info->shared_info);

	HYPERVISOR_shared_info = (shared_info_t *)fix_to_virt(FIX_SHARED_INFO);

	memset(empty_zero_page, 0, PAGE_SIZE);

	HYPERVISOR_shared_info->arch.pfn_to_mfn_frame_list_list =
		virt_to_mfn(pfn_to_mfn_frame_list_list);

	fpp = PAGE_SIZE/sizeof(unsigned long);
	for (i = 0, j = 0, k = -1; i < max_pfn; i += fpp, j++) {
		if ((j % fpp) == 0) {
			k++;
			pfn_to_mfn_frame_list_list[k] =
				virt_to_mfn(pfn_to_mfn_frame_list[k]);
			j = 0;
		}
		pfn_to_mfn_frame_list[k][j] =
			virt_to_mfn(&phys_to_machine_mapping[i]);
	}
	HYPERVISOR_shared_info->arch.max_pfn = max_pfn;
}

#else /* !(defined(__i386__) || defined(__x86_64__)) */

#define switch_idle_mm()	((void)0)
#define mm_pin_all()		((void)0)
#define pre_suspend()		((void)0)
#define post_suspend()		((void)0)

#endif

static int __do_suspend(void *ignore)
{
	int err;

	extern void time_resume(void);

	BUG_ON(smp_processor_id() != 0);
	BUG_ON(in_interrupt());

#if defined(__i386__) || defined(__x86_64__)
	if (xen_feature(XENFEAT_auto_translated_physmap)) {
		printk(KERN_WARNING "Cannot suspend in "
		       "auto_translated_physmap mode.\n");
		return -EOPNOTSUPP;
	}
#endif

	err = smp_suspend();
	if (err)
		return err;

	xenbus_suspend();

	preempt_disable();

	mm_pin_all();
	local_irq_disable();
	preempt_enable();

	gnttab_suspend();

	pre_suspend();

	/*
	 * We'll stop somewhere inside this hypercall. When it returns,
	 * we'll start resuming after the restore.
	 */
	HYPERVISOR_suspend(virt_to_mfn(xen_start_info));

	shutting_down = SHUTDOWN_INVALID;

	post_suspend();

	gnttab_resume();

	irq_resume();

	time_resume();

	switch_idle_mm();

	local_irq_enable();

	xencons_resume();

	xenbus_resume();

	smp_resume();

	return err;
}
#endif /* CONFIG_XEN */

extern int __xen_suspend(int fast_suspend);

static int shutdown_process(void *__unused)
{
	static char *envp[] = { "HOME=/", "TERM=linux",
				"PATH=/sbin:/usr/sbin:/bin:/usr/bin", NULL };
	static char *poweroff_argv[] = { "/sbin/poweroff", NULL };

#ifdef CONFIG_XEN
	extern asmlinkage long sys_reboot(int magic1, int magic2,
					  unsigned int cmd, void *arg);
#endif

	if ((shutting_down == SHUTDOWN_POWEROFF) ||
	    (shutting_down == SHUTDOWN_HALT)) {
		if (call_usermodehelper("/sbin/poweroff", poweroff_argv,
					envp, 0) < 0) {
#ifdef CONFIG_XEN
			sys_reboot(LINUX_REBOOT_MAGIC1,
				   LINUX_REBOOT_MAGIC2,
				   LINUX_REBOOT_CMD_POWER_OFF,
				   NULL);
#endif /* CONFIG_XEN */
		}
	}

	shutting_down = SHUTDOWN_INVALID; /* could try again */

	return 0;
}

#ifndef CONFIG_XEN /* pv-on-hvm */
static int xen_suspend(void *__unused)
{
	int err = __xen_suspend(fast_suspend);
	if (err)
		printk(KERN_ERR "Xen suspend failed (%d)\n", err);
	shutting_down = SHUTDOWN_INVALID;
	return 0;
}
#endif

static int kthread_create_on_cpu(int (*f)(void *arg),
				 void *arg,
				 const char *name,
				 int cpu)
{
	struct task_struct *p;
	p = kthread_create(f, arg, name);
	if (IS_ERR(p))
		return PTR_ERR(p);
	kthread_bind(p, cpu);
	wake_up_process(p);
	return 0;
}

static void __shutdown_handler(void *unused)
{
	int err;

	if (shutting_down != SHUTDOWN_SUSPEND)
		err = kernel_thread(shutdown_process, NULL,
				    CLONE_FS | CLONE_FILES);
	else
#ifndef CONFIG_XEN /* pv-on-hvm */
		err = kthread_create_on_cpu(xen_suspend, NULL, "suspend", 0);
#else /* domU */
		err = kthread_create_on_cpu(__do_suspend, NULL, "suspend", 0);
#endif

	if (err < 0) {
		printk(KERN_WARNING "Error creating shutdown process (%d): "
		       "retrying...\n", -err);
		schedule_delayed_work(&shutdown_work, HZ/2);
	}
}

static void shutdown_handler(struct xenbus_watch *watch,
			     const char **vec, unsigned int len)
{
	extern void ctrl_alt_del(void);
	char *str;
	struct xenbus_transaction xbt;
	int err;

	if (shutting_down != SHUTDOWN_INVALID)
		return;

 again:
	err = xenbus_transaction_start(&xbt);
	if (err)
		return;
	str = (char *)xenbus_read(xbt, "control", "shutdown", NULL);
	/* Ignore read errors and empty reads. */
	if (XENBUS_IS_ERR_READ(str)) {
		xenbus_transaction_end(xbt, 1);
		return;
	}

	xenbus_write(xbt, "control", "shutdown", "");

	err = xenbus_transaction_end(xbt, 0);
	if (err == -EAGAIN) {
		kfree(str);
		goto again;
	}

	if (strcmp(str, "poweroff") == 0)
		shutting_down = SHUTDOWN_POWEROFF;
	else if (strcmp(str, "reboot") == 0)
		ctrl_alt_del();
	else if (strcmp(str, "suspend") == 0)
		shutting_down = SHUTDOWN_SUSPEND;
	else if (strcmp(str, "halt") == 0)
		shutting_down = SHUTDOWN_HALT;
	else {
		printk("Ignoring shutdown request: %s\n", str);
		shutting_down = SHUTDOWN_INVALID;
	}

	if (shutting_down != SHUTDOWN_INVALID)
		schedule_work(&shutdown_work);

	kfree(str);
}

static void sysrq_handler(struct xenbus_watch *watch, const char **vec,
			  unsigned int len)
{
	char sysrq_key = '\0';
	struct xenbus_transaction xbt;
	int err;

 again:
	err = xenbus_transaction_start(&xbt);
	if (err)
		return;
	if (!xenbus_scanf(xbt, "control", "sysrq", "%c", &sysrq_key)) {
		printk(KERN_ERR "Unable to read sysrq code in "
		       "control/sysrq\n");
		xenbus_transaction_end(xbt, 1);
		return;
	}

	if (sysrq_key != '\0')
		xenbus_printf(xbt, "control", "sysrq", "%c", '\0');

	err = xenbus_transaction_end(xbt, 0);
	if (err == -EAGAIN)
		goto again;

#ifdef CONFIG_MAGIC_SYSRQ
	if (sysrq_key != '\0')
		handle_sysrq(sysrq_key, NULL, NULL);
#endif
}

static struct xenbus_watch shutdown_watch = {
	.node = "control/shutdown",
	.callback = shutdown_handler
};

static struct xenbus_watch sysrq_watch = {
	.node ="control/sysrq",
	.callback = sysrq_handler
};


static int setup_shutdown_watcher(void)


{
	int err;

	xenbus_scanf(XBT_NIL, "control",
		     "platform-feature-multiprocessor-suspend",
		     "%d", &fast_suspend);

	err = register_xenbus_watch(&shutdown_watch);
	if (err) {
		printk(KERN_ERR "Failed to set shutdown watcher\n");
		return err;
	}

	err = register_xenbus_watch(&sysrq_watch);
	if (err) {
		printk(KERN_ERR "Failed to set sysrq watcher\n");
		return err;
	}

	return 0;
}

#ifdef CONFIG_XEN

static int shutdown_event(struct notifier_block *notifier,
			  unsigned long event,
			  void *data)
{
	setup_shutdown_watcher();
	return NOTIFY_DONE;
}

static int __init setup_shutdown_event(void)
{
	static struct notifier_block xenstore_notifier = {
		.notifier_call = shutdown_event
	};
	register_xenstore_notifier(&xenstore_notifier);

	return 0;
}

subsys_initcall(setup_shutdown_event);

#else /* !defined(CONFIG_XEN) */

int xen_reboot_init(void)
{
	return setup_shutdown_watcher();
}

#endif /* !defined(CONFIG_XEN) */
