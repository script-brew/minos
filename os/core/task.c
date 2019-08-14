/*
 * Copyright (C) 2019 Min Le (lemin9538@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <minos/minos.h>
#include <minos/sched.h>
#include <minos/mm.h>
#include <minos/atomic.h>
#include <minos/vmodule.h>
#include <minos/task.h>

static DEFINE_SPIN_LOCK(pid_lock);
static DECLARE_BITMAP(pid_map, OS_NR_TASKS);
struct task *os_task_table[OS_NR_TASKS];

static atomic_t os_task_nr;

#define NR_TASK_EVENT	32
static DEFINE_SPIN_LOCK(task_event_lock);
static struct task_event task_events[NR_TASK_EVENT];
static DECLARE_BITMAP(task_event_map, NR_TASK_EVENT);

/* idle task needed be static defined */
static struct task idle_tasks[NR_CPUS];
static DEFINE_PER_CPU(struct task *, idle_task);

struct task_event *alloc_task_event(void)
{
	int bit;
	struct task_event *event = NULL;
	unsigned long flags;

	spin_lock_irqsave(&task_event_lock, flags);
	bit = find_next_zero_bit(task_event_map, NR_TASK_EVENT, 0);
	if (bit < NR_TASK_EVENT) {
		set_bit(bit, task_event_map);
		event = &task_events[bit];
	}

	spin_unlock_irqrestore(&task_event_lock, flags);

	return event;
}

void release_task_event(struct task_event *event)
{
	clear_bit(event->id, task_event_map);
}

int alloc_pid(prio_t prio, int cpuid)
{
	int pid = -1;
	struct pcpu *pcpu = get_per_cpu(pcpu, cpuid);

	/*
	 * check whether this task is a global task or
	 * a task need to attach to the special pcpu and
	 * also check the whether the prio is valid or
	 * invalid. by the side the idle and stat task is
	 * created by the pcpu itself at the boot stage
	 */
	spin_lock(&pid_lock);

	if (prio > OS_LOWEST_PRIO) {
		if (prio == OS_PRIO_IDLE) {
			if (pcpu->idle_task)
				goto out;
		}

		pid = find_next_zero_bit(pid_map, OS_NR_TASKS,
				OS_REALTIME_TASK);
		if (pid >= OS_NR_TASKS)
			pid = -1;
		else
			set_bit(pid, pid_map);
	} else {
		if (!test_and_set_bit(prio, pid_map)) {
			pid = prio;
			os_task_table[pid] = OS_TASK_RESERVED;
		}
	}

out:
	spin_unlock(&pid_lock);

	return pid;
}

void release_pid(int pid)
{
	if (pid > OS_NR_TASKS)
		return;

	spin_lock(&pid_lock);
	clear_bit(pid, pid_map);
	os_task_table[pid] = NULL;
	spin_unlock(&pid_lock);
}

struct task *pid_to_task(int pid)
{
	if (pid >= OS_NR_TASKS)
		return NULL;

	return os_task_table[pid];
}

static void task_timeout_handler(unsigned long data)
{
	struct task *task = (struct task *)data;

	/*
	 * when task is suspended by sleep or waitting
	 * for a event, it may set the delay time, when
	 * the delay time is arrvie, then it will called
	 * this function
	 */
	task_lock(task);

	if (is_task_pending(task)) {
		/* task is timeout and check its stat */
		task->delay = 0;

		set_task_ready(task);
		task->stat &= ~TASK_STAT_SUSPEND;

		task->stat &= ~TASK_STAT_PEND_ANY;
		task->pend_stat = TASK_STAT_PEND_TO;

		set_need_resched();
	} else {
		if (task->delay) {
			task->delay = 0;
			set_task_ready(task);
			task->stat &= ~TASK_STAT_SUSPEND;
			set_need_resched();
		} else {
			pr_warn("wrong task state s-%d ps-%d\n",
					task->stat, task->pend_stat);
		}
	}

	task_unlock(task);
}

static void task_init(struct task *task, char *name,
		void *stack, void *arg, prio_t prio,
		int pid, int aff,size_t stk_size, unsigned long opt)
{
	struct task_info *ti;

	if (stack) {
		task->stack_origin = stack - sizeof(struct task_info);
		task->stack_base = task->stack_origin;
		task->stack_size = stk_size;

		/* init the thread_info */
		ti = (struct task_info *)task->stack_origin;
		TASK_INFO_INIT(ti, task, aff);
	}

	task->udata = arg;
	task->flags = opt;
	task->pid = pid;
	task->prio = prio;

	if (prio <= OS_LOWEST_PRIO) {
		task->by = prio >> 3;
		task->bx = prio & 0x07;
		task->bity = 1ul << task->by;
		task->bitx = 1ul << task->bx;
	}

	task->pend_stat = 0;
	if (task->flags & TASK_FLAGS_VCPU)
		task->stat = TASK_STAT_SUSPEND;
	else
		task->stat = TASK_STAT_RDY;

	task->affinity = aff;
	task->flags = opt;
	task->del_req = 0;
	task->run_time = CONFIG_TASK_RUN_TIME;

	if (task->prio == OS_PRIO_IDLE)
		task->flags |= TASK_FLAGS_IDLE;

	spin_lock_init(&task->lock);

	init_timer_on_cpu(&task->delay_timer, aff);
	task->delay_timer.function = task_timeout_handler;
	task->delay_timer.data = (unsigned long)task;
	strncpy(task->name, name, MIN(strlen(name), TASK_NAME_SIZE));
}

static struct task *__create_task(char *name, task_func_t func,
		void *arg, prio_t prio, int pid, int aff,
		size_t stk_size, unsigned long opt)
{
	struct task *task;
	void *stack = NULL;

	/* now create the task and init it */
	task = zalloc(sizeof(*task));
	if (!task) {
		pr_err("no more memory for task\n");
		return NULL;
	}

	/* allocate the stack for this task */
	if (stk_size) {
		stk_size = BALIGN(stk_size, PAGE_SIZE);
		stack = __get_free_pages(PAGE_NR(stk_size), PAGE_NR(stk_size));
		if (stack == NULL) {
			pr_err("no more memory for task stack\n");
			free(task);
			return NULL;
		} else {
			pr_info("stack 0x%x for task-%d\n",
					(unsigned long)stack, pid);
		}
	}

	/* store this task to the task table */
	os_task_table[pid] = task;
	atomic_inc(&os_task_nr);

	if (aff == PCPU_AFF_NONE)
		aff = 0;
	else if (aff == PCPU_AFF_PERCPU)
		aff = smp_processor_id();

	task_init(task, name, stack, arg, prio,
			pid, aff, stk_size, opt);
	task_vmodules_init(task);

	return task;
}

static void task_create_hook(struct task *task)
{
	do_hooks((void *)task, NULL, OS_HOOK_CREATE_TASK);
}

static void task_ipi_event_handler(void *data)
{
	struct task *task;
	struct task_event *ev = (struct task_event *)data;

	if (data == NULL)
		pr_err("got invalid argument in %s\n", __func__);

	task = ev->task;
	if ((task->affinity != smp_processor_id()) ||
			!is_percpu_task(task)) {
		release_task_event(ev);
		return;
	}

	task_lock(task);

	switch (ev->action) {
	case TASK_EVENT_EVENT_READY:
		/* if the task has been timeout then skip it */
		if (!is_task_pending(task))
			break;

		task->msg = ev->msg;
		task->stat &= ~ev->msk;
		task->wait_event = NULL;

		set_task_ready(task);
		set_need_resched();
		break;

	case TASK_EVENT_FLAG_READY:
		if (!is_task_pending(task))
			break;

		task->delay = 0;
		task->flags_rdy = ev->flags;
		task->stat &= ev->msk;

		set_need_resched();
		break;

	default:
		break;
	}

	/*
	 * set resched flag according to the current prio
	 * BUG - Do not free memory in interrupt, need to
	 * fix it
	 */
	task_unlock(task);
	release_task_event(ev);
}

int task_ipi_event(struct task *task, struct task_event *ev, int wait)
{
	return smp_function_call(task->affinity,
			task_ipi_event_handler, (void *)ev, wait);
}

int create_task(char *name, task_func_t func,
		void *arg, prio_t prio,
		uint16_t aff, unsigned long opt)
{
	int pid = -1;
	struct task *task;
	unsigned long flags;
	struct pcpu *pcpu;

	if ((aff >= NR_CPUS) && (aff != PCPU_AFF_NONE))
		return -EINVAL;

	pid = alloc_pid(prio, aff);
	if (pid < 0)
		return -ENOPID;

	task = __create_task(name, func, arg, prio,
			pid, aff, TASK_STACK_SIZE, opt);
	if (!task) {
		release_pid(pid);
		pid = -ENOPID;
	}

	task_create_hook(task);
	arch_init_task(task, (void *)func, task->udata);

	aff = task->affinity;
	pcpu = get_per_cpu(pcpu, aff);

	/*
	 * after create the task, if the task is affinity to
	 * the current cpu, then it can add the task to
	 * the ready list directly, this action need done after
	 * the task has been finish all the related init things
	 */
	if ((aff < NR_CPUS) && (prio == OS_PRIO_PCPU)) {
		pcpu = get_per_cpu(pcpu, aff);
		spin_lock_irqsave(&pcpu->lock, flags);
		list_add_tail(&pcpu->task_list, &task->list);
		if (aff == smp_processor_id())
			list_add_tail(&pcpu->ready_list, &task->stat_list);
		else
			list_add_tail(&pcpu->new_list, &task->stat_list);
		pcpu->nr_pcpu_task++;
		spin_unlock_irqrestore(&pcpu->lock, flags);
	}

	/*
	 * the vcpu task's stat is different with the normal
	 * task, the vcpu task's init stat is controled by
	 * other mechism
	 */
	if (!(task->flags & TASK_FLAGS_VCPU)) {
		/*
		 * percpu task has already added the the
		 * ready list
		 */
		if (is_realtime_task(task)) {
			kernel_lock_irqsave(flags);
			set_task_ready(task);
			kernel_unlock_irqrestore(flags);
		}

		/*
		 * if the task is a realtime task and the os
		 * sched is running then resched the task
		 * otherwise send a ipi to the task
		 */
		if (is_realtime_task(task)) {
			if (os_is_running())
				sched();
		} else {
			if (aff != smp_processor_id())
				pcpu_resched(aff);
		}
	}

	return pid;
}

int create_idle_task(void)
{
	int pid;
	struct task *task;
	int aff = smp_processor_id();
	struct pcpu *pcpu = get_per_cpu(pcpu, aff);

	pid = alloc_pid(OS_PRIO_IDLE, aff);
	if (pid < -1)
		panic("can not create task, PID error\n");

	task = get_cpu_var(idle_task);
	if (!task)
		panic("error to get idle task\n");

	os_task_table[pid] = task;
	atomic_inc(&os_task_nr);
	task_init(task, "idle-task", NULL, NULL,
			OS_PRIO_IDLE, pid, aff, 0, 0);
	task_vmodules_init(task);

	/* reinit the task's stack information */
	task->stack_size = TASK_STACK_SIZE;
	task->stack_origin = (void *)current_sp() -
		sizeof(struct task_info);

	task->stat = TASK_STAT_RUNNING;
	task->flags |= TASK_FLAGS_IDLE;

	pcpu->idle_task = task;

	/* call the hooks for the idle task */
	task_create_hook(task);

	set_current_prio(OS_PRIO_PCPU);
	set_next_prio(OS_PRIO_PCPU);

	return 0;
}

/*
 * for preempt_disable and preempt_enable need
 * to set the current task at boot stage
 */
static int tasks_early_init(void)
{
	int i;
	struct task *task;
	struct task_info *ti;
	extern struct task *__current_tasks[NR_CPUS];
	extern struct task *__next_tasks[NR_CPUS];
	unsigned long stack_base = CONFIG_MINOS_ENTRY_ADDRESS;

	for (i = 0; i < NR_CPUS; i++) {
		task = &idle_tasks[i];
		memset(task, 0, sizeof(*task));
		get_per_cpu(idle_task, i) = task;
		__current_tasks[i] = task;
		__next_tasks[i] = task;

		/* init the task info for the thread */
		ti = (struct task_info *)(stack_base -
				sizeof(struct task_info));
		TASK_INFO_INIT(ti, task, i);
		stack_base -= TASK_STACK_SIZE;
	}

	return 0;
}
early_initcall(tasks_early_init);

int create_percpu_task(char *name, task_func_t func,
		void *arg, unsigned long flags)
{
	int cpu, ret = 0;

	for_each_online_cpu(cpu) {
		ret = create_task(name, func, arg,
				OS_PRIO_PCPU, cpu, flags);
		if (ret < 0) {
			pr_err("create [%s] fail on cpu%d\n",
					name, cpu);
		}
	}

	return 0;
}

int create_realtime_task(char *name, task_func_t func, void *arg,
		prio_t prio, unsigned long flags)
{
	return create_task(name, func, arg, prio, 0, flags);
}

int create_vcpu_task(char *name, task_func_t func,
		void *arg, int aff, unsigned long flags)
{
	return create_task(name, func, arg, OS_PRIO_PCPU,
			aff, flags & TASK_FLAGS_VCPU);
}

static int task_events_init(void)
{
	int i;

	for (i = 0; i < NR_TASK_EVENT; i++)
		task_events[i].id = i;

	return 0;
}
module_initcall(task_events_init);