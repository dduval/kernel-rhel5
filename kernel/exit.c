/*
 *  linux/kernel/exit.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/smp_lock.h>
#include <linux/module.h>
#include <linux/capability.h>
#include <linux/completion.h>
#include <linux/personality.h>
#include <linux/tty.h>
#include <linux/namespace.h>
#include <linux/key.h>
#include <linux/security.h>
#include <linux/cpu.h>
#include <linux/acct.h>
#include <linux/file.h>
#include <linux/binfmts.h>
#include <linux/tracehook.h>
#include <linux/ptrace.h>
#include <linux/profile.h>
#include <linux/mount.h>
#include <linux/proc_fs.h>
#include <linux/mempolicy.h>
#include <linux/taskstats_kern.h>
#include <linux/delayacct.h>
#include <linux/cpuset.h>
#include <linux/syscalls.h>
#include <linux/signal.h>
#include <linux/posix-timers.h>
#include <linux/cn_proc.h>
#include <linux/mutex.h>
#include <linux/futex.h>
#include <linux/compat.h>
#include <linux/pipe_fs_i.h>
#include <linux/audit.h> /* for audit_free() */
#include <linux/resource.h>
#include <linux/task_io_accounting_ops.h>
#include <trace/sched.h>

#include <asm/uaccess.h>
#include <asm/unistd.h>
#include <asm/pgtable.h>
#include <asm/mmu_context.h>

extern void sem_exit (void);
extern struct task_struct *child_reaper;

static void exit_mm(struct task_struct * tsk);

static void __unhash_process(struct task_struct *p)
{
	nr_threads--;
	detach_pid(p, PIDTYPE_PID);
	if (thread_group_leader(p)) {
		detach_pid(p, PIDTYPE_PGID);
		detach_pid(p, PIDTYPE_SID);

		list_del_rcu(&p->tasks);
		__get_cpu_var(process_counts)--;
	}
	list_del_rcu(&p->thread_group);
	remove_parent(p);
}

/*
 * This function expects the tasklist_lock write-locked.
 */
static void __exit_signal(struct task_struct *tsk)
{
	struct signal_struct *sig = tsk->signal;
	struct sighand_struct *sighand;

	BUG_ON(!sig);
	BUG_ON(!atomic_read(&sig->count));

	rcu_read_lock();
	sighand = rcu_dereference(tsk->sighand);
	spin_lock(&sighand->siglock);

	posix_cpu_timers_exit(tsk);
	if (atomic_dec_and_test(&sig->count))
		posix_cpu_timers_exit_group(tsk);
	else {
		/*
		 * If there is any task waiting for the group exit
		 * then notify it:
		 */
		if (sig->group_exit_task && atomic_read(&sig->count) == sig->notify_count) {
			wake_up_process(sig->group_exit_task);
			sig->group_exit_task = NULL;
		}
		if (tsk == sig->curr_target)
			sig->curr_target = next_thread(tsk);
		/*
		 * Accumulate here the counters for all threads but the
		 * group leader as they die, so they can be added into
		 * the process-wide totals when those are taken.
		 * The group leader stays around as a zombie as long
		 * as there are other threads.  When it gets reaped,
		 * the exit.c code will add its counts into these totals.
		 * We won't ever get here for the group leader, since it
		 * will have been the last reference on the signal_struct.
		 */
		sig->utime = cputime_add(sig->utime, tsk->utime);
		sig->stime = cputime_add(sig->stime, tsk->stime);
		sig->min_flt += tsk->min_flt;
		sig->maj_flt += tsk->maj_flt;
		sig->nvcsw += tsk->nvcsw;
		sig->nivcsw += tsk->nivcsw;
		{
			struct signal_struct_aux *aux;
			aux = signal_aux(sig);
			aux->rchar += tsk->rchar;
			aux->wchar += tsk->wchar;
			aux->syscr += tsk->syscr;
			aux->syscw += tsk->syscw;
#ifdef CONFIG_TASK_IO_ACCOUNTING
			aux->ioac.read_bytes += task_aux(tsk)->ioac.read_bytes;
			aux->ioac.write_bytes +=
				task_aux(tsk)->ioac.write_bytes;
			aux->ioac.cancelled_write_bytes +=
				task_aux(tsk)->ioac.cancelled_write_bytes;
#endif /* CONFIG_TASK_IO_ACCOUNTING */
		}
		sig->sched_time += tsk->sched_time;
		sig = NULL; /* Marker for below. */
	}

	__unhash_process(tsk);

	tsk->signal = NULL;
	tsk->sighand = NULL;
	spin_unlock(&sighand->siglock);
	rcu_read_unlock();

	__cleanup_sighand(sighand);
	clear_tsk_thread_flag(tsk,TIF_SIGPENDING);
	flush_sigqueue(&tsk->pending);
	if (sig) {
		flush_sigqueue(&sig->shared_pending);
		__cleanup_signal(sig);
	}
}

static void delayed_put_task_struct(struct rcu_head *rhp)
{
	trace_sched_process_free(container_of(rhp, struct task_struct, rcu));
	put_task_struct(container_of(rhp, struct task_struct, rcu));
}

void release_task(struct task_struct * p)
{
	struct task_struct *leader;
	int zap_leader;
	int inhibited_leader;
repeat:
	tracehook_release_task(p);
	atomic_dec(&p->user->processes);
	BUG_ON(tracehook_check_released(p));
	write_lock_irq(&tasklist_lock);
	__exit_signal(p);

	/*
	 * If we are the last non-leader member of the thread
	 * group, and the leader is zombie, then notify the
	 * group leader's parent process. (if it wants notification.)
	 */
	zap_leader = 0;
	inhibited_leader = 0;
	leader = p->group_leader;
	if (leader != p && thread_group_empty(leader) && leader->exit_state == EXIT_ZOMBIE) {
		BUG_ON(leader->exit_signal == -1);
		if (tracehook_inhibit_wait_zombie(leader))
			inhibited_leader = 1;
		else
			do_notify_parent(leader, leader->exit_signal);
		/*
		 * If we were the last child thread and the leader has
		 * exited already, and the leader's parent ignores SIGCHLD,
		 * then we are the one who should release the leader.
		 *
		 * do_notify_parent() will have marked it self-reaping in
		 * that case.
		 */
		zap_leader = (leader->exit_signal == -1);
		if (zap_leader) {
			/*
			 * Preserve the invariant that release_task()
			 * can only be called on a task in EXIT_DEAD.
			 */
			zap_leader = xchg(&leader->exit_state, EXIT_DEAD);
			BUG_ON(zap_leader != EXIT_ZOMBIE);
		}
	}

	sched_exit(p);
	write_unlock_irq(&tasklist_lock);
	proc_flush_task(p);
	release_thread(p);
	call_rcu(&p->rcu, delayed_put_task_struct);

	p = leader;
	if (unlikely(zap_leader))
		goto repeat;

	/*
	 * If tracing usurps normal reaping of the leader, tracing needs
	 * to be notified it would normally be reapable now.
	 */
	if (unlikely(inhibited_leader))
		tracehook_report_delayed_group_leader(leader);
}

/*
 * This checks not only the pgrp, but falls back on the pid if no
 * satisfactory pgrp is found. I dunno - gdb doesn't work correctly
 * without this...
 */
int session_of_pgrp(int pgrp)
{
	struct task_struct *p;
	int sid = -1;

	read_lock(&tasklist_lock);
	do_each_task_pid(pgrp, PIDTYPE_PGID, p) {
		if (p->signal->session > 0) {
			sid = p->signal->session;
			goto out;
		}
	} while_each_task_pid(pgrp, PIDTYPE_PGID, p);
	p = find_task_by_pid(pgrp);
	if (p)
		sid = p->signal->session;
out:
	read_unlock(&tasklist_lock);
	
	return sid;
}

/*
 * Determine if a process group is "orphaned", according to the POSIX
 * definition in 2.2.2.52.  Orphaned process groups are not to be affected
 * by terminal-generated stop signals.  Newly orphaned process groups are
 * to receive a SIGHUP and a SIGCONT.
 *
 * "I ask you, have you ever known what it is to be an orphan?"
 */
static int will_become_orphaned_pgrp(int pgrp, struct task_struct *ignored_task)
{
	struct task_struct *p;
	int ret = 1;

	do_each_task_pid(pgrp, PIDTYPE_PGID, p) {
		if (p == ignored_task
				|| p->exit_state
				|| p->parent->pid == 1)
			continue;
		if (process_group(p->parent) != pgrp
			    && p->parent->signal->session == p->signal->session) {
			ret = 0;
			break;
		}
	} while_each_task_pid(pgrp, PIDTYPE_PGID, p);
	return ret;	/* (sighing) "Often!" */
}

int is_orphaned_pgrp(int pgrp)
{
	int retval;

	read_lock(&tasklist_lock);
	retval = will_become_orphaned_pgrp(pgrp, NULL);
	read_unlock(&tasklist_lock);

	return retval;
}

static int has_stopped_jobs(int pgrp)
{
	int retval = 0;
	struct task_struct *p;

	do_each_task_pid(pgrp, PIDTYPE_PGID, p) {
		if (p->state != TASK_STOPPED)
			continue;

		retval = 1;
		break;
	} while_each_task_pid(pgrp, PIDTYPE_PGID, p);
	return retval;
}

/**
 * reparent_to_init - Reparent the calling kernel thread to the init task.
 *
 * If a kernel thread is launched as a result of a system call, or if
 * it ever exits, it should generally reparent itself to init so that
 * it is correctly cleaned up on exit.
 *
 * The various task state such as scheduling policy and priority may have
 * been inherited from a user process, so we reset them to sane values here.
 *
 * NOTE that reparent_to_init() gives the caller full capabilities.
 */
static void reparent_to_init(void)
{
	write_lock_irq(&tasklist_lock);

	/* Reparent to init */
	remove_parent(current);
	current->parent = child_reaper;
	add_parent(current);

	/* Set the exit signal to SIGCHLD so we signal init on exit */
	current->exit_signal = SIGCHLD;

	if ((current->policy == SCHED_NORMAL ||
			current->policy == SCHED_BATCH)
				&& (task_nice(current) < 0))
		set_user_nice(current, 0);
	/* cpus_allowed? */
	/* rt_priority? */
	/* signals? */
	security_task_reparent_to_init(current);
	memcpy(current->signal->rlim, init_task.signal->rlim,
	       sizeof(current->signal->rlim));
	atomic_inc(&(INIT_USER->__count));
	write_unlock_irq(&tasklist_lock);
	switch_uid(INIT_USER);
}

void __set_special_pids(pid_t session, pid_t pgrp)
{
	struct task_struct *curr = current->group_leader;

	if (curr->signal->session != session) {
		detach_pid(curr, PIDTYPE_SID);
		curr->signal->session = session;
		attach_pid(curr, PIDTYPE_SID, session);
	}
	if (process_group(curr) != pgrp) {
		detach_pid(curr, PIDTYPE_PGID);
		curr->signal->pgrp = pgrp;
		attach_pid(curr, PIDTYPE_PGID, pgrp);
	}
}

void set_special_pids(pid_t session, pid_t pgrp)
{
	write_lock_irq(&tasklist_lock);
	__set_special_pids(session, pgrp);
	write_unlock_irq(&tasklist_lock);
}

/*
 * Let kernel threads use this to say that they
 * allow a certain signal (since daemonize() will
 * have disabled all of them by default).
 */
int allow_signal(int sig)
{
	if (!valid_signal(sig) || sig < 1)
		return -EINVAL;

	spin_lock_irq(&current->sighand->siglock);
	sigdelset(&current->blocked, sig);
	if (!current->mm) {
		/* Kernel threads handle their own signals.
		   Let the signal code know it'll be handled, so
		   that they don't get converted to SIGKILL or
		   just silently dropped */
		current->sighand->action[(sig)-1].sa.sa_handler = (void __user *)2;
	}
	recalc_sigpending();
	spin_unlock_irq(&current->sighand->siglock);
	return 0;
}

EXPORT_SYMBOL(allow_signal);

int disallow_signal(int sig)
{
	if (!valid_signal(sig) || sig < 1)
		return -EINVAL;

	spin_lock_irq(&current->sighand->siglock);
	sigaddset(&current->blocked, sig);
	recalc_sigpending();
	spin_unlock_irq(&current->sighand->siglock);
	return 0;
}

EXPORT_SYMBOL(disallow_signal);

/*
 *	Put all the gunge required to become a kernel thread without
 *	attached user resources in one place where it belongs.
 */

void daemonize(const char *name, ...)
{
	va_list args;
	struct fs_struct *fs;
	sigset_t blocked;

	va_start(args, name);
	vsnprintf(current->comm, sizeof(current->comm), name, args);
	va_end(args);

	/*
	 * If we were started as result of loading a module, close all of the
	 * user space pages.  We don't need them, and if we didn't close them
	 * they would be locked into memory.
	 */
	exit_mm(current);

	set_special_pids(1, 1);
	mutex_lock(&tty_mutex);
	current->signal->tty = NULL;
	mutex_unlock(&tty_mutex);

	/* Block and flush all signals */
	sigfillset(&blocked);
	sigprocmask(SIG_BLOCK, &blocked, NULL);
	flush_signals(current);

	/* Become as one with the init task */

	exit_fs(current);	/* current->fs->count--; */
	fs = init_task.fs;
	current->fs = fs;
	atomic_inc(&fs->count);
	exit_namespace(current);
	current->namespace = init_task.namespace;
	get_namespace(current->namespace);
 	exit_files(current);
	current->files = init_task.files;
	atomic_inc(&current->files->count);

	reparent_to_init();
}

EXPORT_SYMBOL(daemonize);

static void close_files(struct files_struct * files)
{
	int i, j;
	struct fdtable *fdt;

	j = 0;

	/*
	 * It is safe to dereference the fd table without RCU or
	 * ->file_lock because this is the last reference to the
	 * files structure.
	 */
	fdt = files_fdtable(files);
	for (;;) {
		unsigned long set;
		i = j * __NFDBITS;
		if (i >= fdt->max_fdset || i >= fdt->max_fds)
			break;
		set = fdt->open_fds->fds_bits[j++];
		while (set) {
			if (set & 1) {
				struct file * file = xchg(&fdt->fd[i], NULL);
				if (file)
					filp_close(file, files);
			}
			i++;
			set >>= 1;
		}
	}
}

struct files_struct *get_files_struct(struct task_struct *task)
{
	struct files_struct *files;

	task_lock(task);
	files = task->files;
	if (files)
		atomic_inc(&files->count);
	task_unlock(task);

	return files;
}

void fastcall put_files_struct(struct files_struct *files)
{
	struct fdtable *fdt;

	if (atomic_dec_and_test(&files->count)) {
		close_files(files);
		/*
		 * Free the fd and fdset arrays if we expanded them.
		 * If the fdtable was embedded, pass files for freeing
		 * at the end of the RCU grace period. Otherwise,
		 * you can free files immediately.
		 */
		fdt = files_fdtable(files);
		if (fdt == &files->fdtab)
			fdt->free_files = files;
		else
			kmem_cache_free(files_cachep, files);
		free_fdtable(fdt);
	}
}

EXPORT_SYMBOL(put_files_struct);

void reset_files_struct(struct task_struct *tsk, struct files_struct *files)
{
	struct files_struct *old;

	old = tsk->files;
	task_lock(tsk);
	tsk->files = files;
	task_unlock(tsk);
	put_files_struct(old);
}

EXPORT_SYMBOL(reset_files_struct);

static inline void __exit_files(struct task_struct *tsk)
{
	struct files_struct * files = tsk->files;

	if (files) {
		task_lock(tsk);
		tsk->files = NULL;
		task_unlock(tsk);
		put_files_struct(files);
	}
}

void exit_files(struct task_struct *tsk)
{
	__exit_files(tsk);
}

static inline void __put_fs_struct(struct fs_struct *fs)
{
	/* No need to hold fs->lock if we are killing it */
	if (atomic_dec_and_test(&fs->count)) {
		dput(fs->root);
		mntput(fs->rootmnt);
		dput(fs->pwd);
		mntput(fs->pwdmnt);
		if (fs->altroot) {
			dput(fs->altroot);
			mntput(fs->altrootmnt);
		}
		kmem_cache_free(fs_cachep, fs);
	}
}

void put_fs_struct(struct fs_struct *fs)
{
	__put_fs_struct(fs);
}

static inline void __exit_fs(struct task_struct *tsk)
{
	struct fs_struct * fs = tsk->fs;

	if (fs) {
		task_lock(tsk);
		tsk->fs = NULL;
		task_unlock(tsk);
		__put_fs_struct(fs);
	}
}

void exit_fs(struct task_struct *tsk)
{
	__exit_fs(tsk);
}

EXPORT_SYMBOL_GPL(exit_fs);

/*
 * Turn us into a lazy TLB process if we
 * aren't already..
 */
static void exit_mm(struct task_struct * tsk)
{
	struct mm_struct *mm = tsk->mm;

	mm_release(tsk, mm);
	if (!mm)
		return;
	/*
	 * Serialize with any possible pending coredump.
	 * We must hold mmap_sem around checking core_waiters
	 * and clearing tsk->mm.  The core-inducing thread
	 * will increment core_waiters for each thread in the
	 * group with ->mm != NULL.
	 */
	down_read(&mm->mmap_sem);
	if (mm->core_waiters) {
		up_read(&mm->mmap_sem);
		down_write(&mm->mmap_sem);
		if (!--mm->core_waiters)
			complete(mm->core_startup_done);
		up_write(&mm->mmap_sem);

		wait_for_completion(&mm->core_done);
		down_read(&mm->mmap_sem);
	}
	atomic_inc(&mm->mm_count);
	BUG_ON(mm != tsk->active_mm);
	/* more a memory barrier than a real lock */
	task_lock(tsk);
	tsk->mm = NULL;
	up_read(&mm->mmap_sem);
	enter_lazy_tlb(mm, current);
	task_unlock(tsk);
	mmput(mm);
}

static inline void
choose_new_parent(struct task_struct *p, struct task_struct *reaper)
{
	/*
	 * Make sure we're not reparenting to ourselves and that
	 * the parent is not a zombie.
	 */
	BUG_ON(p == reaper || reaper->exit_state);
	p->parent = reaper;
}

static void
reparent_thread(struct task_struct *p, struct task_struct *father)
{
	if (p->pdeath_signal)
		/* We already hold the tasklist_lock here.  */
		group_send_sig_info(p->pdeath_signal, SEND_SIG_NOINFO, p);

	/* Move the child from its dying parent to the new one.  */
	list_move_tail(&p->sibling, &p->parent->children);

        /* If this is a threaded reparent there is no need to
         * notify anyone anything has happened.
         */
        if (p->parent->group_leader == father->group_leader)
                return;

	/* We don't want people slaying init.  */
	if (p->exit_signal != -1)
		p->exit_signal = SIGCHLD;

	/* If we'd notified the old parent about this child's death,
	 * also notify the new parent.
	 */
	if (!tracehook_inhibit_wait_zombie(p) &&
	    p->exit_state == EXIT_ZOMBIE && p->exit_signal != -1 &&
	    thread_group_empty(p))
		do_notify_parent(p, p->exit_signal);

	/*
	 * process group orphan check
	 * Case ii: Our child is in a different pgrp
	 * than we are, and it was the only connection
	 * outside, so the child pgrp is now orphaned.
	 */
	if ((process_group(p) != process_group(father)) &&
	    (p->signal->session == father->signal->session)) {
		int pgrp = process_group(p);

		if (will_become_orphaned_pgrp(pgrp, NULL) && has_stopped_jobs(pgrp)) {
			__kill_pg_info(SIGHUP, SEND_SIG_PRIV, pgrp);
			__kill_pg_info(SIGCONT, SEND_SIG_PRIV, pgrp);
		}
	}
}

/*
 * When we die, we re-parent all our children.
 * Try to give them to another thread in our thread
 * group, and if no such member exists, give it to
 * the global child reaper process (ie "init")
 */
static void
forget_original_parent(struct task_struct *father)
{
	struct task_struct *p, *reaper = father;
	struct list_head *_p, *_n;

	do {
		reaper = next_thread(reaper);
		if (reaper == father) {
			reaper = child_reaper;
			break;
		}
	} while (reaper->exit_state);

	list_for_each_safe(_p, _n, &father->children) {
		p = list_entry(_p, struct task_struct, sibling);

                /* Father is going to die, so it does not need available
                 * first time slices from childs anymore */     
		if (p->first_time_slice == father->pid)
		    p->first_time_slice = 0;
		    
		choose_new_parent(p, reaper);
		reparent_thread(p, father);
	}
}

/*
 * Send signals to all our closest relatives so that they know
 * to properly mourn us..
 */
static void exit_notify(struct task_struct *tsk)
{
	int state;
	struct task_struct *t;
	int noreap;
	void *cookie;

	if (signal_pending(tsk) && !(tsk->signal->flags & SIGNAL_GROUP_EXIT)
	    && !thread_group_empty(tsk)) {
		/*
		 * This occurs when there was a race between our exit
		 * syscall and a group signal choosing us as the one to
		 * wake up.  It could be that we are the only thread
		 * alerted to check for pending signals, but another thread
		 * should be woken now to take the signal since we will not.
		 * Now we'll wake all the threads in the group just to make
		 * sure someone gets all the pending signals.
		 */
		read_lock(&tasklist_lock);
		spin_lock_irq(&tsk->sighand->siglock);
		for (t = next_thread(tsk); t != tsk; t = next_thread(t))
			if (!signal_pending(t) && !(t->flags & PF_EXITING))
				recalc_sigpending_and_wake(t);
		spin_unlock_irq(&tsk->sighand->siglock);
		read_unlock(&tasklist_lock);
	}

	write_lock_irq(&tasklist_lock);

	/*
	 * This does two things:
	 *
  	 * A.  Make init inherit all the child processes
	 * B.  Check to see if any process groups have become orphaned
	 *	as a result of our exiting, and if they have any stopped
	 *	jobs, send them a SIGHUP and then a SIGCONT.  (POSIX 3.2.2.2)
	 */

	forget_original_parent(tsk);
	BUG_ON(!list_empty(&tsk->children));

	/*
	 * Check to see if any process groups have become orphaned
	 * as a result of our exiting, and if they have any stopped
	 * jobs, send them a SIGHUP and then a SIGCONT.  (POSIX 3.2.2.2)
	 *
	 * Case i: Our father is in a different pgrp than we are
	 * and we were the only connection outside, so our pgrp
	 * is about to become orphaned.
	 */
	 
	t = tsk->parent;
	
	if ((process_group(t) != process_group(tsk)) &&
	    (t->signal->session == tsk->signal->session) &&
	    will_become_orphaned_pgrp(process_group(tsk), tsk) &&
	    has_stopped_jobs(process_group(tsk))) {
		__kill_pg_info(SIGHUP, SEND_SIG_PRIV, process_group(tsk));
		__kill_pg_info(SIGCONT, SEND_SIG_PRIV, process_group(tsk));
	}

	/* Let father know we died 
	 *
	 * Thread signals are configurable, but you aren't going to use
	 * that to send signals to arbitary processes. 
	 * That stops right now.
	 *
	 * If the parent exec id doesn't match the exec id we saved
	 * when we started then we know the parent has changed security
	 * domain.
	 *
	 * If our self_exec id doesn't match our parent_exec_id then
	 * we have changed execution domain as these two values started
	 * the same after a fork.
	 *	
	 */
	
	if (tsk->exit_signal != SIGCHLD && tsk->exit_signal != -1 &&
	    (tsk->parent_exec_id != t->self_exec_id ||
	     tsk->self_exec_id != tsk->parent_exec_id))
		tsk->exit_signal = SIGCHLD;

	if (!tracehook_notify_death(tsk, &noreap, &cookie)
	    && tsk->exit_signal != -1 && thread_group_empty(tsk))
		do_notify_parent(tsk, tsk->exit_signal);

	state = EXIT_ZOMBIE;
	if (tsk->exit_signal == -1 && !noreap)
		state = EXIT_DEAD;
	tsk->exit_state = state;

	write_unlock_irq(&tasklist_lock);

	tracehook_report_death(tsk, state, cookie);

	/* If the process is dead, release it - nobody will wait for it */
	if (state == EXIT_DEAD)
		release_task(tsk);
}

fastcall NORET_TYPE void do_exit(long code)
{
	struct task_struct *tsk = current;
	struct taskstats *tidstats;
	int group_dead;
	unsigned int mycpu;

	profile_task_exit(tsk);

	WARN_ON(atomic_read(&tsk->fs_excl));

	if (unlikely(in_interrupt()))
		panic("Aiee, killing interrupt handler!");
	if (unlikely(!tsk->pid))
		panic("Attempted to kill the idle task!");
	if (unlikely(tsk == child_reaper))
		panic("Attempted to kill init!");

	tracehook_report_exit(&code);

	/*
	 * We're taking recursive faults here in do_exit. Safest is to just
	 * leave this task alone and wait for reboot.
	 */
	if (unlikely(tsk->flags & PF_EXITING)) {
		printk(KERN_ALERT
			"Fixing recursive fault but reboot is needed!\n");
		/*
		 * We can do this unlocked here. The futex code uses
		 * this flag just to verify whether the pi state
		 * cleanup has been done or not. In the worst case it
		 * loops once more. We pretend that the cleanup was
		 * done as there is no way to return. Either the
		 * OWNER_DIED bit is set by now or we push the blocked
		 * task into the wait for ever nirwana as well.
		 */
		tsk->flags |= PF_EXITPIDONE;
		if (tsk->io_context)
			exit_io_context();
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule();
	}

	/*
	 * tsk->flags are checked in the futex code to protect against
	 * an exiting task cleaning up the robust pi futexes.
	 */
	spin_lock_irq(&tsk->pi_lock);
	tsk->flags |= PF_EXITING;
	spin_unlock_irq(&tsk->pi_lock);

	ptrace_exit(tsk);

	if (unlikely(in_atomic()))
		printk(KERN_INFO "note: %s[%d] exited with preempt_count %d\n",
				current->comm, current->pid,
				preempt_count());

	taskstats_exit_alloc(&tidstats, &mycpu);

	acct_update_integrals(tsk);
	if (tsk->mm) {
		update_hiwater_rss(tsk->mm);
		update_hiwater_vm(tsk->mm);
	}
	group_dead = atomic_dec_and_test(&tsk->signal->live);
	if (group_dead) {
		hrtimer_cancel(&tsk->signal->real_timer);
		exit_itimers(tsk->signal);
	}

	if (current->tux_info) {
#ifdef CONFIG_TUX_DEBUG
		printk("Possibly unexpected TUX-thread exit(%ld) at %p?\n",
			code, __builtin_return_address(0));
#endif
		current->tux_exit();
	}

	acct_collect(code, group_dead);
	if (unlikely(tsk->robust_list))
		exit_robust_list(tsk);
#if defined(CONFIG_FUTEX) && defined(CONFIG_COMPAT)
	if (unlikely(tsk->compat_robust_list))
		compat_exit_robust_list(tsk);
#endif
	if (group_dead)
		tty_audit_exit();
	if (unlikely(tsk->audit_context))
		audit_free(tsk);
	taskstats_exit_send(tsk, tidstats, group_dead, mycpu);
	taskstats_exit_free(tidstats);

	exit_mm(tsk);

	if (group_dead)
		acct_process();
	trace_sched_process_exit(tsk);

	exit_sem(tsk);
	__exit_files(tsk);
	__exit_fs(tsk);
	exit_namespace(tsk);
	exit_thread();
	cpuset_exit(tsk);
	exit_keys(tsk);

	if (group_dead && tsk->signal->leader)
		disassociate_ctty(1);

	module_put(task_thread_info(tsk)->exec_domain->module);
	if (tsk->binfmt)
		module_put(tsk->binfmt->module);

	tsk->exit_code = code;
	proc_exit_connector(tsk);
	exit_notify(tsk);
#ifdef CONFIG_NUMA
	mpol_free(tsk->mempolicy);
	tsk->mempolicy = NULL;
#endif
	/*
	 * This must happen late, after the PID is not
	 * hashed anymore:
	 */
	if (unlikely(!list_empty(&tsk->pi_state_list)))
		exit_pi_state_list(tsk);
	if (unlikely(current->pi_state_cache))
		kfree(current->pi_state_cache);
	/*
	 * Make sure we are holding no locks:
	 */
	debug_check_no_locks_held(tsk);
	/*
	 * We can do this unlocked here. The futex code uses this flag
	 * just to verify whether the pi state cleanup has been done
	 * or not. In the worst case it loops once more.
	 */
	tsk->flags |= PF_EXITPIDONE;

	if (tsk->io_context)
		exit_io_context();

	if (tsk->splice_pipe)
		__free_pipe_info(tsk->splice_pipe);

	/* PF_DEAD causes final put_task_struct after we schedule. */
	preempt_disable();
	BUG_ON(tsk->flags & PF_DEAD);
	tsk->flags |= PF_DEAD;

	schedule();
	BUG();
	/* Avoid "noreturn function does return".  */
	for (;;) ;
}

EXPORT_SYMBOL_GPL(do_exit);

NORET_TYPE void complete_and_exit(struct completion *comp, long code)
{
	if (comp)
		complete(comp);
	
	do_exit(code);
}

EXPORT_SYMBOL(complete_and_exit);

asmlinkage long sys_exit(int error_code)
{
	do_exit((error_code&0xff)<<8);
}

/*
 * Take down every thread in the group.  This is called by fatal signals
 * as well as by sys_exit_group (below).
 */
NORET_TYPE void
do_group_exit(int exit_code)
{
	BUG_ON(exit_code & 0x80); /* core dumps don't get here */

	if (current->signal->flags & SIGNAL_GROUP_EXIT)
		exit_code = current->signal->group_exit_code;
	else if (!thread_group_empty(current)) {
		struct signal_struct *const sig = current->signal;
		struct sighand_struct *const sighand = current->sighand;
		spin_lock_irq(&sighand->siglock);
		if (sig->flags & SIGNAL_GROUP_EXIT)
			/* Another thread got here before we took the lock.  */
			exit_code = sig->group_exit_code;
		else {
			sig->group_exit_code = exit_code;
			zap_other_threads(current);
		}
		spin_unlock_irq(&sighand->siglock);
	}

	do_exit(exit_code);
	/* NOTREACHED */
}

/*
 * this kills every thread in the thread group. Note that any externally
 * wait4()-ing process will get the correct exit code - even if this
 * thread is not the thread group leader.
 */
asmlinkage void sys_exit_group(int error_code)
{
	do_group_exit((error_code & 0xff) << 8);
}

static int eligible_child(pid_t pid, int options, struct task_struct *p)
{
	if (pid > 0) {
		if (p->pid != pid)
			return 0;
	} else if (!pid) {
		if (process_group(p) != process_group(current))
			return 0;
	} else if (pid != -1) {
		if (process_group(p) != -pid)
			return 0;
	}

	/*
	 * Do not consider detached threads.
	 */
	if (p->exit_signal == -1)
		return 0;

	/* Wait for all children (clone and not) if __WALL is set;
	 * otherwise, wait for clone children *only* if __WCLONE is
	 * set; otherwise, wait for non-clone children *only*.  (Note:
	 * A "clone" child here is one that reports to its parent
	 * using a signal other than SIGCHLD.) */
	if (((p->exit_signal != SIGCHLD) ^ ((options & __WCLONE) != 0))
	    && !(options & __WALL))
		return 0;
	/*
	 * Do not consider thread group leaders that are
	 * in a non-empty thread group:
	 */
	if (delay_group_leader(p))
		return 2;

	if (security_task_wait(p))
		return 0;

	return 1;
}

static int wait_noreap_copyout(struct task_struct *p, pid_t pid, uid_t uid,
			       int why, int status,
			       struct siginfo __user *infop,
			       struct rusage __user *rusagep)
{
	int retval = rusagep ? getrusage(p, RUSAGE_BOTH, rusagep) : 0;

	put_task_struct(p);
	if (!retval)
		retval = put_user(SIGCHLD, &infop->si_signo);
	if (!retval)
		retval = put_user(0, &infop->si_errno);
	if (!retval)
		retval = put_user((short)why, &infop->si_code);
	if (!retval)
		retval = put_user(pid, &infop->si_pid);
	if (!retval)
		retval = put_user(uid, &infop->si_uid);
	if (!retval)
		retval = put_user(status, &infop->si_status);
	if (!retval)
		retval = pid;
	return retval;
}

/*
 * Handle sys_wait4 work for one task in state EXIT_ZOMBIE.  We hold
 * read_lock(&tasklist_lock) on entry.  If we return zero, we still hold
 * the lock and this task is uninteresting.  If we return nonzero, we have
 * released the lock and the system call should return.
 */
static int wait_task_zombie(struct task_struct *p, int noreap,
			    struct siginfo __user *infop,
			    int __user *stat_addr, struct rusage __user *ru)
{
	unsigned long state;
	int retval;
	int status;

	if (unlikely(noreap)) {
		pid_t pid = p->pid;
		uid_t uid = p->uid;
		int exit_code = p->exit_code;
		int why, status;

		if (unlikely(p->exit_state != EXIT_ZOMBIE))
			return 0;
		if (unlikely(p->exit_signal == -1))
			return 0;
		get_task_struct(p);
		read_unlock(&tasklist_lock);
		if ((exit_code & 0x7f) == 0) {
			why = CLD_EXITED;
			status = exit_code >> 8;
		} else {
			why = (exit_code & 0x80) ? CLD_DUMPED : CLD_KILLED;
			status = exit_code & 0x7f;
		}
		return wait_noreap_copyout(p, pid, uid, why,
					   status, infop, ru);
	}

	/*
	 * Try to move the task's state to DEAD
	 * only one thread is allowed to do this:
	 */
	state = xchg(&p->exit_state, EXIT_DEAD);
	if (state != EXIT_ZOMBIE) {
		BUG_ON(state != EXIT_DEAD);
		return 0;
	}
	BUG_ON(p->exit_signal == -1);

	if (likely(p->signal)) {
		struct signal_struct *psig;
		struct signal_struct *sig;

		/*
		 * The resource counters for the group leader are in its
		 * own task_struct.  Those for dead threads in the group
		 * are in its signal_struct, as are those for the child
		 * processes it has previously reaped.  All these
		 * accumulate in the parent's signal_struct c* fields.
		 *
		 * We don't bother to take a lock here to protect these
		 * p->signal fields, because they are only touched by
		 * __exit_signal, which runs with tasklist_lock
		 * write-locked anyway, and so is excluded here.  We do
		 * need to protect the access to p->parent->signal fields,
		 * as other threads in the parent group can be right
		 * here reaping other children at the same time.
		 */
		spin_lock_irq(&p->parent->sighand->siglock);
		psig = p->parent->signal;
		sig = p->signal;
		psig->cutime =
			cputime_add(psig->cutime,
			cputime_add(p->utime,
			cputime_add(sig->utime,
				    sig->cutime)));
		psig->cstime =
			cputime_add(psig->cstime,
			cputime_add(p->stime,
			cputime_add(sig->stime,
				    sig->cstime)));
		psig->cmin_flt +=
			p->min_flt + sig->min_flt + sig->cmin_flt;
		psig->cmaj_flt +=
			p->maj_flt + sig->maj_flt + sig->cmaj_flt;
		psig->cnvcsw +=
			p->nvcsw + sig->nvcsw + sig->cnvcsw;
		psig->cnivcsw +=
			p->nivcsw + sig->nivcsw + sig->cnivcsw;
		{
			struct signal_struct_aux *aux, *paux;
			aux = signal_aux(sig);
			paux = signal_aux(psig);
			paux->rchar += p->rchar + aux->rchar;
			paux->wchar += p->wchar + aux->wchar;
			paux->syscr += p->syscr + aux->syscr;
			paux->syscw += p->syscw + aux->syscw;
#ifdef CONFIG_TASK_IO_ACCOUNTING
			paux->ioac.read_bytes +=
				task_aux(p)->ioac.read_bytes +
				aux->ioac.read_bytes;
			paux->ioac.write_bytes +=
				task_aux(p)->ioac.write_bytes +
				aux->ioac.write_bytes;
			paux->ioac.cancelled_write_bytes +=
				task_aux(p)->ioac.cancelled_write_bytes +
				aux->ioac.cancelled_write_bytes;
#endif /* CONFIG_TASK_IO_ACCOUNTING */
		}
		spin_unlock_irq(&p->parent->sighand->siglock);
	}

	/*
	 * Now we are sure this task is interesting, and no other
	 * thread can reap it because we set its state to EXIT_DEAD.
	 */
	read_unlock(&tasklist_lock);

	retval = ru ? getrusage(p, RUSAGE_BOTH, ru) : 0;
	status = (p->signal->flags & SIGNAL_GROUP_EXIT)
		? p->signal->group_exit_code : p->exit_code;
	if (!retval && stat_addr)
		retval = put_user(status, stat_addr);
	if (!retval && infop)
		retval = put_user(SIGCHLD, &infop->si_signo);
	if (!retval && infop)
		retval = put_user(0, &infop->si_errno);
	if (!retval && infop) {
		int why;

		if ((status & 0x7f) == 0) {
			why = CLD_EXITED;
			status >>= 8;
		} else {
			why = (status & 0x80) ? CLD_DUMPED : CLD_KILLED;
			status &= 0x7f;
		}
		retval = put_user((short)why, &infop->si_code);
		if (!retval)
			retval = put_user(status, &infop->si_status);
	}
	if (!retval && infop)
		retval = put_user(p->pid, &infop->si_pid);
	if (!retval && infop)
		retval = put_user(p->uid, &infop->si_uid);
	if (retval) {
		// TODO: is this safe?
		p->exit_state = EXIT_ZOMBIE;
		return retval;
	}
	retval = p->pid;
	release_task(p);

	BUG_ON(!retval);
	return retval;
}

/*
 * Handle sys_wait4 work for one task in state TASK_STOPPED.  We hold
 * read_lock(&tasklist_lock) on entry.  If we return zero, we still hold
 * the lock and this task is uninteresting.  If we return nonzero, we have
 * released the lock and the system call should return.
 */
static int wait_task_stopped(struct task_struct *p, int delayed_group_leader,
			     int noreap, struct siginfo __user *infop,
			     int __user *stat_addr, struct rusage __user *ru)
{
	int retval, exit_code;

	if (!p->exit_code)
		return 0;
	if (delayed_group_leader &&
	    p->signal && p->signal->group_stop_count > 0)
		/*
		 * A group stop is in progress and this is the group leader.
		 * We won't report until all threads have stopped.
		 */
		return 0;

	/*
	 * Now we are pretty sure this task is interesting.
	 * Make sure it doesn't get reaped out from under us while we
	 * give up the lock and then examine it below.  We don't want to
	 * keep holding onto the tasklist_lock while we call getrusage and
	 * possibly take page faults for user memory.
	 */
	get_task_struct(p);
	read_unlock(&tasklist_lock);

	if (unlikely(noreap)) {
		pid_t pid = p->pid;
		uid_t uid = p->uid;

		exit_code = p->exit_code;
		if (unlikely(!exit_code) ||
		    unlikely(p->state & TASK_TRACED))
			goto bail_ref;
		return wait_noreap_copyout(p, pid, uid, CLD_STOPPED,
					   (exit_code << 8) | 0x7f,
					   infop, ru);
	}

	write_lock_irq(&tasklist_lock);

	/*
	 * This uses xchg to be atomic with the thread resuming and setting
	 * it.  It must also be done with the write lock held to prevent a
	 * race with the EXIT_ZOMBIE case.
	 */
	exit_code = xchg(&p->exit_code, 0);
	if (unlikely(p->exit_state)) {
		/*
		 * The task resumed and then died.  Let the next iteration
		 * catch it in EXIT_ZOMBIE.  Note that exit_code might
		 * already be zero here if it resumed and did _exit(0).
		 * The task itself is dead and won't touch exit_code again;
		 * other processors in this function are locked out.
		 */
		p->exit_code = exit_code;
		exit_code = 0;
	}
	if (unlikely(exit_code == 0)) {
		/*
		 * Another thread in this function got to it first, or it
		 * resumed, or it resumed and then died.
		 */
		write_unlock_irq(&tasklist_lock);
bail_ref:
		put_task_struct(p);
		/*
		 * We are returning to the wait loop without having successfully
		 * removed the process and having released the lock. We cannot
		 * continue, since the "p" task pointer is potentially stale.
		 *
		 * Return -EAGAIN, and do_wait() will restart the loop from the
		 * beginning. Do _not_ re-acquire the lock.
		 */
		return -EAGAIN;
	}

	/* move to end of parent's list to avoid starvation */
	remove_parent(p);
	add_parent(p);

	write_unlock_irq(&tasklist_lock);

	retval = ru ? getrusage(p, RUSAGE_BOTH, ru) : 0;
	if (!retval && stat_addr)
		retval = put_user((exit_code << 8) | 0x7f, stat_addr);
	if (!retval && infop)
		retval = put_user(SIGCHLD, &infop->si_signo);
	if (!retval && infop)
		retval = put_user(0, &infop->si_errno);
	if (!retval && infop)
		retval = put_user((short)CLD_STOPPED, &infop->si_code);
	if (!retval && infop)
		retval = put_user(exit_code, &infop->si_status);
	if (!retval && infop)
		retval = put_user(p->pid, &infop->si_pid);
	if (!retval && infop)
		retval = put_user(p->uid, &infop->si_uid);
	if (!retval)
		retval = p->pid;
	put_task_struct(p);

	BUG_ON(!retval);
	return retval;
}

/*
 * Handle do_wait work for one task in a live, non-stopped state.
 * read_lock(&tasklist_lock) on entry.  If we return zero, we still hold
 * the lock and this task is uninteresting.  If we return nonzero, we have
 * released the lock and the system call should return.
 */
static int wait_task_continued(struct task_struct *p, int noreap,
			       struct siginfo __user *infop,
			       int __user *stat_addr, struct rusage __user *ru)
{
	int retval;
	pid_t pid;
	uid_t uid;

	if (unlikely(!p->signal))
		return 0;

	if (!(p->signal->flags & SIGNAL_STOP_CONTINUED))
		return 0;

	spin_lock_irq(&p->sighand->siglock);
	/* Re-check with the lock held.  */
	if (!(p->signal->flags & SIGNAL_STOP_CONTINUED)) {
		spin_unlock_irq(&p->sighand->siglock);
		return 0;
	}
	if (!noreap)
		p->signal->flags &= ~SIGNAL_STOP_CONTINUED;
	spin_unlock_irq(&p->sighand->siglock);

	pid = p->pid;
	uid = p->uid;
	get_task_struct(p);
	read_unlock(&tasklist_lock);

	if (!infop) {
		retval = ru ? getrusage(p, RUSAGE_BOTH, ru) : 0;
		put_task_struct(p);
		if (!retval && stat_addr)
			retval = put_user(0xffff, stat_addr);
		if (!retval)
			retval = p->pid;
	} else {
		retval = wait_noreap_copyout(p, pid, uid,
					     CLD_CONTINUED, SIGCONT,
					     infop, ru);
		BUG_ON(retval == 0);
	}

	return retval;
}


static long do_wait(pid_t pid, int options, struct siginfo __user *infop,
		    int __user *stat_addr, struct rusage __user *ru)
{
	DECLARE_WAITQUEUE(wait, current);
	struct task_struct *tsk;
	int flag, retval;

	trace_sched_process_wait(pid);

	add_wait_queue(&current->signal->wait_chldexit,&wait);
repeat:
	/*
	 * We will set this flag if we see any child that might later
	 * match our criteria, even if we are not able to reap it yet.
	 */
	flag = 0;
	current->state = TASK_INTERRUPTIBLE;
	read_lock(&tasklist_lock);
	tsk = current;
	do {
		struct task_struct *p;
		struct list_head *_p;
		int ret;

		list_for_each(_p,&tsk->children) {
			p = list_entry(_p, struct task_struct, sibling);

			ret = eligible_child(pid, options, p);
			if (!ret)
				continue;

			switch (p->state) {
			case TASK_TRACED:
				flag = 1;
				continue;
			case TASK_STOPPED:
				/*
				 * It's stopped now, so it might later
				 * continue, exit, or stop again.
				 */
				flag = 1;
				if (!(options & WUNTRACED))
					continue;
				if (tracehook_inhibit_wait_stopped(p))
					continue;
				retval = wait_task_stopped(p, ret == 2,
							   (options & WNOWAIT),
							   infop,
							   stat_addr, ru);
				if (retval == -EAGAIN)
					goto repeat;
				if (retval != 0) /* He released the lock.  */
					goto end;
				break;
			default:
			// case EXIT_DEAD:
				if (p->exit_state == EXIT_DEAD)
					continue;
			// case EXIT_ZOMBIE:
				if (p->exit_state == EXIT_ZOMBIE) {
					/*
					 * Eligible but we cannot release
					 * it yet:
					 */
					if (ret == 2)
						goto check_continued;
					if (!likely(options & WEXITED))
						continue;
					if (tracehook_inhibit_wait_zombie(p)) {
						flag = 1;
						continue;
					}
					retval = wait_task_zombie(
						p, (options & WNOWAIT),
						infop, stat_addr, ru);
					/* He released the lock.  */
					if (retval != 0)
						goto end;
					break;
				}
check_continued:
				/*
				 * It's running now, so it might later
				 * exit, stop, or stop and then continue.
				 */
				flag = 1;
				if (!unlikely(options & WCONTINUED))
					continue;
				if (tracehook_inhibit_wait_continued(p))
					continue;
				retval = wait_task_continued(
					p, (options & WNOWAIT),
					infop, stat_addr, ru);
				if (retval != 0) /* He released the lock.  */
					goto end;
				break;
			}
		}

		retval = ptrace_do_wait(tsk, pid, options,
					infop, stat_addr, ru);
		if (retval != -ECHILD) {
			flag = 1;
			if (retval != 0) /* He released the lock.  */
				goto end;
		}

		if (options & __WNOTHREAD)
			break;
		tsk = next_thread(tsk);
		BUG_ON(tsk->signal != current->signal);
	} while (tsk != current);

	read_unlock(&tasklist_lock);
	if (flag) {
		retval = 0;
		if (options & WNOHANG)
			goto end;
		retval = -ERESTARTSYS;
		if (signal_pending(current))
			goto end;
		schedule();
		goto repeat;
	}
	retval = -ECHILD;
end:
	current->state = TASK_RUNNING;
	remove_wait_queue(&current->signal->wait_chldexit,&wait);
	if (infop) {
		if (retval > 0)
			retval = 0;
		else {
			/*
			 * For a WNOHANG return, clear out all the fields
			 * we would set so the user can easily tell the
			 * difference.
			 */
			if (!retval)
				retval = put_user(0, &infop->si_signo);
			if (!retval)
				retval = put_user(0, &infop->si_errno);
			if (!retval)
				retval = put_user(0, &infop->si_code);
			if (!retval)
				retval = put_user(0, &infop->si_pid);
			if (!retval)
				retval = put_user(0, &infop->si_uid);
			if (!retval)
				retval = put_user(0, &infop->si_status);
		}
	}
	return retval;
}

asmlinkage long sys_waitid(int which, pid_t pid,
			   struct siginfo __user *infop, int options,
			   struct rusage __user *ru)
{
	long ret;

	if (options & ~(WNOHANG|WNOWAIT|WEXITED|WSTOPPED|WCONTINUED))
		return -EINVAL;
	if (!(options & (WEXITED|WSTOPPED|WCONTINUED)))
		return -EINVAL;

	switch (which) {
	case P_ALL:
		pid = -1;
		break;
	case P_PID:
		if (pid <= 0)
			return -EINVAL;
		break;
	case P_PGID:
		if (pid <= 0)
			return -EINVAL;
		pid = -pid;
		break;
	default:
		return -EINVAL;
	}

	ret = do_wait(pid, options, infop, NULL, ru);

	/* avoid REGPARM breakage on x86: */
	prevent_tail_call(ret);
	return ret;
}

asmlinkage long sys_wait4(pid_t pid, int __user *stat_addr,
			  int options, struct rusage __user *ru)
{
	long ret;

	if (options & ~(WNOHANG|WUNTRACED|WCONTINUED|
			__WNOTHREAD|__WCLONE|__WALL))
		return -EINVAL;
	ret = do_wait(pid, options | WEXITED, NULL, stat_addr, ru);

	/* avoid REGPARM breakage on x86: */
	prevent_tail_call(ret);
	return ret;
}

#ifdef __ARCH_WANT_SYS_WAITPID

/*
 * sys_waitpid() remains for compatibility. waitpid() should be
 * implemented by calling sys_wait4() from libc.a.
 */
asmlinkage long sys_waitpid(pid_t pid, int __user *stat_addr, int options)
{
	return sys_wait4(pid, stat_addr, options, NULL);
}

#endif
