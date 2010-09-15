#include <assert.h>

#include "smp.h"
#include "interrupt.h"

unsigned ncpus;
unsigned ht_per_core;
unsigned bsp_cpu_id;

PUBLIC struct cpu cpus[CONFIG_MAX_CPUS];

/* info passed to another cpu along with a sched ipi */
struct sched_ipi_data {
	volatile u32_t	flags;
	volatile u32_t	data;
};

PRIVATE struct sched_ipi_data  sched_ipi_data[CONFIG_MAX_CPUS];

#define SCHED_IPI_STOP_PROC	1
#define SCHED_IPI_VM_INHIBIT	2

static volatile unsigned ap_cpus_booted;

SPINLOCK_DEFINE(big_kernel_lock)
SPINLOCK_DEFINE(boot_lock)

PUBLIC void wait_for_APs_to_finish_booting(void)
{
	/* we must let the other CPUs to run in kernel mode first */
	BKL_UNLOCK();
	while (ap_cpus_booted != (ncpus - 1))
		arch_pause();
	/* now we have to take the lock again as we continu execution */
	BKL_LOCK();
}

PUBLIC void ap_boot_finished(unsigned cpu)
{
	ap_cpus_booted++;
}

PUBLIC void smp_ipi_halt_handler(void)
{
	ipi_ack();
	stop_local_timer();
	arch_smp_halt_cpu();
}

PUBLIC void smp_schedule(unsigned cpu)
{
	/*
	 * check if the cpu is processing some other ipi already. If yes, no
	 * need to wake it up
	 */
	if ((volatile unsigned)sched_ipi_data[cpu].flags != 0)
		return;
	arch_send_smp_schedule_ipi(cpu);
}

/*
 * tell another cpu about a task to do and return only after the cpu acks that
 * the task is finished. Also wait before it finishes task sent by another cpu
 * to the same one.
 */
PRIVATE void smp_schedule_sync(struct proc * p, unsigned task)
{
	unsigned cpu = p->p_cpu;

	/* 
	 * if some other cpu made a request to the same cpu, wait until it is
	 * done before proceeding
	 */
	if ((volatile unsigned)sched_ipi_data[cpu].flags != 0) {
		BKL_UNLOCK();
		while ((volatile unsigned)sched_ipi_data[cpu].flags != 0);
		BKL_LOCK();
	}

	sched_ipi_data[cpu].flags |= task;
	sched_ipi_data[cpu].data = (u32_t) p;
	arch_send_smp_schedule_ipi(cpu);

	/* wait until the destination cpu finishes its job */
	BKL_UNLOCK();
	while ((volatile unsigned)sched_ipi_data[cpu].flags != 0);
	BKL_LOCK();
}

PUBLIC void smp_schedule_stop_proc(struct proc * p)
{
	if (proc_is_runnable(p))
		smp_schedule_sync(p, SCHED_IPI_STOP_PROC);
	else
		RTS_SET(p, RTS_PROC_STOP);
	assert(RTS_ISSET(p, RTS_PROC_STOP));
}

PUBLIC void smp_schedule_vminhibit(struct proc * p)
{
	if (proc_is_runnable(p))
		smp_schedule_sync(p, SCHED_IPI_VM_INHIBIT);
	else
		RTS_SET(p, RTS_VMINHIBIT);
	assert(RTS_ISSET(p, RTS_VMINHIBIT));
}

PUBLIC void smp_ipi_sched_handler(void)
{
	struct proc * curr;
	unsigned mycpu = cpuid;
	unsigned flgs;
	
	ipi_ack();
	
	curr = get_cpu_var(mycpu, proc_ptr);
	flgs = sched_ipi_data[mycpu].flags;

	if (flgs) {
		struct proc * p;
		p = (struct proc *)sched_ipi_data[mycpu].data;

		if (flgs & SCHED_IPI_STOP_PROC) {
			RTS_SET(p, RTS_PROC_STOP);
		}
		if (flgs & SCHED_IPI_VM_INHIBIT) {
			RTS_SET(p, RTS_VMINHIBIT);
		}
	}
	else if (curr->p_endpoint != IDLE) {
		RTS_SET(curr, RTS_PREEMPTED);
	}
	sched_ipi_data[cpuid].flags = 0;
}

