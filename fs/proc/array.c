/*
 *  linux/fs/proc/array.c
 *
 *  Copyright (C) 1992  by Linus Torvalds
 *  based on ideas by Darren Senn
 *
 * Fixes:
 * Michael. K. Johnson: stat,statm extensions.
 *                      <johnsonm@stolaf.edu>
 *
 * Pauline Middelink :  Made cmdline,envline only break at '\0's, to
 *                      make sure SET_PROCTITLE works. Also removed
 *                      bad '!' which forced address recalculation for
 *                      EVERY character on the current page.
 *                      <middelin@polyware.iaf.nl>
 *
 * Danny ter Haar    :	added cpuinfo
 *			<dth@cistron.nl>
 *
 * Alessandro Rubini :  profile extension.
 *                      <rubini@ipvvis.unipv.it>
 *
 * Jeff Tranter      :  added BogoMips field to cpuinfo
 *                      <Jeff_Tranter@Mitel.COM>
 *
 * Bruno Haible      :  remove 4K limit for the maps file
 *			<haible@ma2s2.mathematik.uni-karlsruhe.de>
 *
 * Yves Arrouye      :  remove removal of trailing spaces in get_array.
 *			<Yves.Arrouye@marin.fdn.fr>
 *
 * Jerome Forissier  :  added per-CPU time information to /proc/stat
 *                      and /proc/<pid>/cpu extension
 *                      <forissier@isia.cma.fr>
 *			- Incorporation and non-SMP safe operation
 *			of forissier patch in 2.1.78 by
 *			Hans Marcus <crowbar@concepts.nl>
 *
 * aeb@cwi.nl        :  /proc/partitions
 *
 *
 * Alan Cox	     :  security fixes.
 *			<Alan.Cox@linux.org>
 *
 * Al Viro           :  safe handling of mm_struct
 *
 * Gerhard Wichert   :  added BIGMEM support
 * Siemens AG           <Gerhard.Wichert@pdb.siemens.de>
 *
 * Al Viro & Jeff Garzik :  moved most of the thing into base.c and
 *			 :  proc_misc.c. The rest may eventually go into
 *			 :  base.c too.
 */

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/time.h>
#include <linux/kernel.h>
#include <linux/kernel_stat.h>
#include <linux/tty.h>
#include <linux/string.h>
#include <linux/mman.h>
#include <linux/proc_fs.h>
#include <linux/ioport.h>
#include <linux/mm.h>
#include <linux/hugetlb.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/signal.h>
#include <linux/highmem.h>
#include <linux/file.h>
#include <linux/times.h>
#include <linux/cpuset.h>
#include <linux/tracehook.h>
#include <linux/rcupdate.h>
#include <linux/delayacct.h>
#include <linux/resource.h>
#include <linux/ptrace.h>

#include <asm/uaccess.h>
#include <asm/pgtable.h>
#include <asm/io.h>
#include <asm/processor.h>
#include "internal.h"

/* Gcc optimizes away "strlen(x)" for constant x */
#define ADDBUF(buffer, string) \
do { memcpy(buffer, string, strlen(string)); \
     buffer += strlen(string); } while (0)

static inline char * task_name(struct task_struct *p, char * buf)
{
	int i;
	char * name;
	char tcomm[sizeof(p->comm)];

	get_task_comm(tcomm, p);

	ADDBUF(buf, "Name:\t");
	name = tcomm;
	i = sizeof(tcomm);
	do {
		unsigned char c = *name;
		name++;
		i--;
		*buf = c;
		if (!c)
			break;
		if (c == '\\') {
			buf[1] = c;
			buf += 2;
			continue;
		}
		if (c == '\n') {
			buf[0] = '\\';
			buf[1] = 'n';
			buf += 2;
			continue;
		}
		buf++;
	} while (i);
	*buf = '\n';
	return buf+1;
}

/*
 * The task state array is a strange "bitmap" of
 * reasons to sleep. Thus "running" is zero, and
 * you can test for combinations of others with
 * simple bit tests.
 */
static const char *task_state_array[] = {
	"R (running)",		/*  0 */
	"S (sleeping)",		/*  1 */
	"D (disk sleep)",	/*  2 */
	"T (stopped)",		/*  4 */
	"T (tracing stop)",	/*  8 */
	"Z (zombie)",		/* 16 */
	"X (dead)"		/* 32 */
};

static inline const char * get_task_state(struct task_struct *tsk)
{
	unsigned int state = (tsk->state & (TASK_RUNNING |
					    TASK_INTERRUPTIBLE |
					    TASK_UNINTERRUPTIBLE |
					    TASK_STOPPED |
					    TASK_TRACED)) |
			(tsk->exit_state & (EXIT_ZOMBIE |
					    EXIT_DEAD));
	const char **p = &task_state_array[0];

	while (state) {
		p++;
		state >>= 1;
	}
	return *p;
}

static inline char * task_state(struct task_struct *p, char *buffer)
{
	struct task_struct *tracer;
	pid_t tracer_pid;
	struct group_info *group_info;
	int g;
	struct fdtable *fdt = NULL;

	read_lock(&tasklist_lock);
	tracer = tracehook_tracer_task(p);
	tracer_pid = tracer == NULL ? 0 : tracer->pid;

	buffer += sprintf(buffer,
		"State:\t%s\n"
		"SleepAVG:\t%lu%%\n"
		"Tgid:\t%d\n"
		"Pid:\t%d\n"
		"PPid:\t%d\n"
		"TracerPid:\t%d\n"
		"Uid:\t%d\t%d\t%d\t%d\n"
		"Gid:\t%d\t%d\t%d\t%d\n",
		get_task_state(p),
		(p->sleep_avg/1024)*100/(1020000000/1024),
	       	p->tgid,
		p->pid, pid_alive(p) ? p->group_leader->parent->tgid : 0,
		tracer_pid,
		p->uid, p->euid, p->suid, p->fsuid,
		p->gid, p->egid, p->sgid, p->fsgid);
	read_unlock(&tasklist_lock);
	task_lock(p);
	rcu_read_lock();
	if (p->files)
		fdt = files_fdtable(p->files);
	buffer += sprintf(buffer,
		"FDSize:\t%d\n"
		"Groups:\t",
		fdt ? fdt->max_fds : 0);
	rcu_read_unlock();

	group_info = p->group_info;
	get_group_info(group_info);
	task_unlock(p);

	for (g = 0; g < min(group_info->ngroups,NGROUPS_SMALL); g++)
		buffer += sprintf(buffer, "%d ", GROUP_AT(group_info,g));
	put_group_info(group_info);

	buffer += sprintf(buffer, "\n");
	return buffer;
}

static char * render_sigset_t(const char *header, sigset_t *set, char *buffer)
{
	int i, len;

	len = strlen(header);
	memcpy(buffer, header, len);
	buffer += len;

	i = _NSIG;
	do {
		int x = 0;

		i -= 4;
		if (sigismember(set, i+1)) x |= 1;
		if (sigismember(set, i+2)) x |= 2;
		if (sigismember(set, i+3)) x |= 4;
		if (sigismember(set, i+4)) x |= 8;
		*buffer++ = (x < 10 ? '0' : 'a' - 10) + x;
	} while (i >= 4);

	*buffer++ = '\n';
	*buffer = 0;
	return buffer;
}

static void collect_sigign_sigcatch(struct task_struct *p, sigset_t *ign,
				    sigset_t *catch)
{
	struct k_sigaction *k;
	int i;

	k = p->sighand->action;
	for (i = 1; i <= _NSIG; ++i, ++k) {
		if (k->sa.sa_handler == SIG_IGN)
			sigaddset(ign, i);
		else if (k->sa.sa_handler != SIG_DFL)
			sigaddset(catch, i);
	}
}

static inline char * task_sig(struct task_struct *p, char *buffer)
{
	sigset_t pending, shpending, blocked, ignored, caught;
	int num_threads = 0;
	unsigned long qsize = 0;
	unsigned long qlim = 0;

	sigemptyset(&pending);
	sigemptyset(&shpending);
	sigemptyset(&blocked);
	sigemptyset(&ignored);
	sigemptyset(&caught);

	/* Gather all the data with the appropriate locks held */
	read_lock(&tasklist_lock);
	if (p->sighand) {
		spin_lock_irq(&p->sighand->siglock);
		pending = p->pending.signal;
		shpending = p->signal->shared_pending.signal;
		blocked = p->blocked;
		collect_sigign_sigcatch(p, &ignored, &caught);
		num_threads = atomic_read(&p->signal->count);
		qsize = atomic_read(&p->user->sigpending);
		qlim = p->signal->rlim[RLIMIT_SIGPENDING].rlim_cur;
		spin_unlock_irq(&p->sighand->siglock);
	}
	read_unlock(&tasklist_lock);

	buffer += sprintf(buffer, "Threads:\t%d\n", num_threads);
	buffer += sprintf(buffer, "SigQ:\t%lu/%lu\n", qsize, qlim);

	/* render them all */
	buffer = render_sigset_t("SigPnd:\t", &pending, buffer);
	buffer = render_sigset_t("ShdPnd:\t", &shpending, buffer);
	buffer = render_sigset_t("SigBlk:\t", &blocked, buffer);
	buffer = render_sigset_t("SigIgn:\t", &ignored, buffer);
	buffer = render_sigset_t("SigCgt:\t", &caught, buffer);

	return buffer;
}

static inline char *task_cap(struct task_struct *p, char *buffer)
{
    return buffer + sprintf(buffer, "CapInh:\t%016x\n"
			    "CapPrm:\t%016x\n"
			    "CapEff:\t%016x\n",
			    cap_t(p->cap_inheritable),
			    cap_t(p->cap_permitted),
			    cap_t(p->cap_effective));
}

int proc_pid_status(struct task_struct *task, char * buffer)
{
	char * orig = buffer;
	struct mm_struct *mm = get_task_mm(task);

	buffer = task_name(task, buffer);
	buffer = task_state(task, buffer);
 
	if (mm) {
		buffer = task_mem(mm, buffer);
		mmput(mm);
	}
	buffer = task_sig(task, buffer);
	buffer = task_cap(task, buffer);
	buffer = cpuset_task_status_allowed(task, buffer);
#if defined(CONFIG_S390)
	buffer = task_show_regs(task, buffer);
#endif
	return buffer - orig;
}

static int do_task_stat(struct task_struct *task, char * buffer, int whole)
{
	unsigned long vsize, eip, esp, wchan = ~0UL;
	long priority, nice;
	int tty_pgrp = -1, tty_nr = 0;
	sigset_t sigign, sigcatch;
	char state;
	int res;
 	pid_t ppid, pgid = -1, sid = -1;
	int num_threads = 0;
	int permitted;
	struct mm_struct *mm;
	unsigned long long start_time;
	unsigned long cmin_flt = 0, cmaj_flt = 0;
	unsigned long  min_flt = 0,  maj_flt = 0;
	cputime_t cutime, cstime, utime, stime;
	unsigned long rsslim = 0;
	struct task_struct *t;
	char tcomm[sizeof(task->comm)];

	state = *get_task_state(task);
	vsize = eip = esp = 0;
	permitted = ptrace_may_attach(task);
	mm = get_task_mm(task);
	if (mm) {
		vsize = task_vsize(mm);
		if (permitted) {
			eip = KSTK_EIP(task);
			esp = KSTK_ESP(task);
		}
	}

	get_task_comm(tcomm, task);

	sigemptyset(&sigign);
	sigemptyset(&sigcatch);
	cutime = cstime = utime = stime = cputime_zero;

	mutex_lock(&tty_mutex);
	read_lock(&tasklist_lock);
	if (task->sighand) {
		spin_lock_irq(&task->sighand->siglock);
		num_threads = atomic_read(&task->signal->count);
		collect_sigign_sigcatch(task, &sigign, &sigcatch);

		/* add up live thread stats at the group level */
		if (whole) {
			t = task;
			do {
				min_flt += t->min_flt;
				maj_flt += t->maj_flt;
				utime = cputime_add(utime, t->utime);
				stime = cputime_add(stime, t->stime);
				t = next_thread(t);
			} while (t != task);
		}

		spin_unlock_irq(&task->sighand->siglock);
	}
	if (task->signal) {
		if (task->signal->tty) {
			tty_pgrp = task->signal->tty->pgrp;
			tty_nr = new_encode_dev(tty_devnum(task->signal->tty));
		}
		pgid = process_group(task);
		sid = task->signal->session;
		cmin_flt = task->signal->cmin_flt;
		cmaj_flt = task->signal->cmaj_flt;
		cutime = task->signal->cutime;
		cstime = task->signal->cstime;
		rsslim = task->signal->rlim[RLIMIT_RSS].rlim_cur;
		if (whole) {
			min_flt += task->signal->min_flt;
			maj_flt += task->signal->maj_flt;
			utime = cputime_add(utime, task->signal->utime);
			stime = cputime_add(stime, task->signal->stime);
		}
	}
	ppid = pid_alive(task) ? task->group_leader->parent->tgid : 0;
	read_unlock(&tasklist_lock);
	mutex_unlock(&tty_mutex);

	if (permitted && (!whole || num_threads<2)) {
		wchan = 0;
		if (current->uid == task->uid || current->euid == task->uid ||
				capable(CAP_SYS_NICE))
			wchan = get_wchan(task);
	}
	if (!whole) {
		min_flt = task->min_flt;
		maj_flt = task->maj_flt;
		utime = task->utime;
		stime = task->stime;
	}

	/* scale priority and nice values from timeslices to -20..20 */
	/* to make it look like a "normal" Unix priority/nice value  */
	priority = task_prio(task);
	nice = task_nice(task);

	/* Temporary variable needed for gcc-2.96 */
	/* convert timespec -> nsec*/
	start_time = (unsigned long long)task->start_time.tv_sec * NSEC_PER_SEC
				+ task->start_time.tv_nsec;
	/* convert nsec -> ticks */
	start_time = nsec_to_clock_t(start_time);

	res = sprintf(buffer,"%d (%s) %c %d %d %d %d %d %lu %lu \
%lu %lu %lu %lu %lu %ld %ld %ld %ld %d 0 %llu %lu %ld %lu %lu %lu %lu %lu \
%lu %lu %lu %lu %lu %lu %lu %lu %d %d %lu %lu %llu\n",
		task->pid,
		tcomm,
		state,
		ppid,
		pgid,
		sid,
		tty_nr,
		tty_pgrp,
		task->flags,
		min_flt,
		cmin_flt,
		maj_flt,
		cmaj_flt,
		cputime_to_clock_t(utime),
		cputime_to_clock_t(stime),
		cputime_to_clock_t(cutime),
		cputime_to_clock_t(cstime),
		priority,
		nice,
		num_threads,
		start_time,
		vsize,
		mm ? get_mm_rss(mm) : 0,
	        rsslim,
		mm ? mm->start_code : 0,
		mm ? mm->end_code : 0,
		(permitted && mm) ? mm->start_stack : 0,
		esp,
		eip,
		/* The signal information here is obsolete.
		 * It must be decimal for Linux 2.0 compatibility.
		 * Use /proc/#/status for real-time signals.
		 */
		task->pending.signal.sig[0] & 0x7fffffffUL,
		task->blocked.sig[0] & 0x7fffffffUL,
		sigign      .sig[0] & 0x7fffffffUL,
		sigcatch    .sig[0] & 0x7fffffffUL,
		wchan,
		0UL,
		0UL,
		task->exit_signal,
		task_cpu(task),
		task->rt_priority,
		task->policy,
		(unsigned long long)delayacct_blkio_ticks(task));
	if(mm)
		mmput(mm);
	return res;
}

int proc_tid_stat(struct task_struct *task, char * buffer)
{
	return do_task_stat(task, buffer, 0);
}

int proc_tgid_stat(struct task_struct *task, char * buffer)
{
	return do_task_stat(task, buffer, 1);
}

int proc_pid_statm(struct task_struct *task, char *buffer)
{
	int size = 0, resident = 0, shared = 0, text = 0, lib = 0, data = 0;
	struct mm_struct *mm = get_task_mm(task);
	
	if (mm) {
		size = task_statm(mm, &shared, &text, &data, &resident);
		mmput(mm);
	}

	return sprintf(buffer,"%d %d %d %d %d %d %d\n",
		       size, resident, shared, text, lib, data, 0);
}


struct limit_names {
	char *name;
	char *unit;
};

static const struct limit_names lnames[RLIM_NLIMITS] = {
	[RLIMIT_CPU] = {"Max cpu time", "seconds"},
	[RLIMIT_FSIZE] = {"Max file size", "bytes"},
	[RLIMIT_DATA] = {"Max data size", "bytes"},
	[RLIMIT_STACK] = {"Max stack size", "bytes"},
	[RLIMIT_CORE] = {"Max core file size", "bytes"},
	[RLIMIT_RSS] = {"Max resident set", "bytes"},
	[RLIMIT_NPROC] = {"Max processes", "processes"},
	[RLIMIT_NOFILE] = {"Max open files", "files"},
	[RLIMIT_MEMLOCK] = {"Max locked memory", "bytes"},
	[RLIMIT_AS] = {"Max address space", "bytes"},
	[RLIMIT_LOCKS] = {"Max file locks", "locks"},
	[RLIMIT_SIGPENDING] = {"Max pending signals", "signals"},
	[RLIMIT_MSGQUEUE] = {"Max msgqueue size", "bytes"},
	[RLIMIT_NICE] = {"Max nice priority", NULL},
	[RLIMIT_RTPRIO] = {"Max realtime priority", NULL},
};

/* Display limits for a process */

int proc_pid_limits(struct task_struct *task, char *buffer)
{
	unsigned int i;
	int count = 0;
	unsigned long flags;
	char *bufptr = buffer;

	struct rlimit rlim[RLIM_NLIMITS];

	rcu_read_lock();
	if (!lock_task_sighand(task,&flags)) {
		rcu_read_unlock();
		return 0;
	}
	memcpy(rlim, task->signal->rlim, sizeof(struct rlimit) * RLIM_NLIMITS);
	unlock_task_sighand(task, &flags);
	rcu_read_unlock();

	/*
	 * print the file header
	 */
	count += sprintf(&bufptr[count], "%-25s %-20s %-20s %-10s\n",
			"Limit", "Soft Limit", "Hard Limit", "Units");

	for (i = 0; i < RLIM_NLIMITS; i++) {
		if (rlim[i].rlim_cur == RLIM_INFINITY)
			count += sprintf(&bufptr[count], "%-25s %-20s ",
					 lnames[i].name, "unlimited");
		else
			count += sprintf(&bufptr[count], "%-25s %-20lu ",
					 lnames[i].name, rlim[i].rlim_cur);

		if (rlim[i].rlim_max == RLIM_INFINITY)
			count += sprintf(&bufptr[count], "%-20s ", "unlimited");
		else
			count += sprintf(&bufptr[count], "%-20lu ",
					 rlim[i].rlim_max);

		if (lnames[i].unit)
			count += sprintf(&bufptr[count], "%-10s\n",
					 lnames[i].unit);
		else
			count += sprintf(&bufptr[count], "\n");
	}

	return count;
}
