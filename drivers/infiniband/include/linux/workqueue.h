#ifndef BACKPORT_LINUX_WORKQUEUE_TO_2_6_19
#define BACKPORT_LINUX_WORKQUEUE_TO_2_6_19

#include_next <linux/workqueue.h>

struct delayed_work {
	struct work_struct work;
};

static inline void
backport_INIT_WORK(struct work_struct *work, void *func)
{
	INIT_WORK(work, func, work);
}

static inline int backport_queue_delayed_work(struct workqueue_struct *wq,
					      struct delayed_work *work,
					      unsigned long delay)
{
	if (likely(!delay))
		return queue_work(wq, &work->work);
	else
		return queue_delayed_work(wq, &work->work, delay);
}

static inline int 
backport_cancel_delayed_work(struct delayed_work *work)
{
	return cancel_delayed_work(&work->work);
}

static inline void 
backport_cancel_rearming_delayed_workqueue(struct workqueue_struct *wq, struct delayed_work *work)
{
	cancel_rearming_delayed_workqueue(wq, &work->work);
}

static inline
int backport_schedule_delayed_work(struct delayed_work *work, unsigned long delay)
{
	if (likely(!delay))
		return schedule_work(&work->work);
	else
		return schedule_delayed_work(&work->work, delay);
}

#undef INIT_WORK
#define INIT_WORK(_work, _func) backport_INIT_WORK(_work, _func)
#define INIT_DELAYED_WORK(_work, _func) INIT_WORK(&(_work)->work, _func)
#define INIT_DELAYED_WORK_DEFERRABLE(_work, _func) INIT_DELAYED_WORK(_work, _func)

#undef DECLARE_WORK
#define DECLARE_WORK(n, f) \
	struct work_struct n = __WORK_INITIALIZER(n, (void (*)(void *))f, &(n))
#define DECLARE_DELAYED_WORK(n, f) \
	struct delayed_work n = { .work = __WORK_INITIALIZER(n.work, (void (*)(void *))f, &(n.work)) }

#define queue_delayed_work backport_queue_delayed_work
#define cancel_delayed_work backport_cancel_delayed_work
#define cancel_delayed_work_sync cancel_delayed_work
#define cancel_rearming_delayed_workqueue backport_cancel_rearming_delayed_workqueue
#define schedule_delayed_work backport_schedule_delayed_work
#define cancel_delayed_work_sync cancel_delayed_work

static inline void backport_cancel_rearming_delayed_work(struct delayed_work *work)
{
	cancel_delayed_work_sync(work);
}

#define cancel_rearming_delayed_work backport_cancel_rearming_delayed_work

#endif
