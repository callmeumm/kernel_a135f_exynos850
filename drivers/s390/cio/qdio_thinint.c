// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright IBM Corp. 2000, 2009
 * Author(s): Utz Bacher <utz.bacher@de.ibm.com>
 *	      Cornelia Huck <cornelia.huck@de.ibm.com>
 *	      Jan Glauber <jang@linux.vnet.ibm.com>
 */
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/kernel_stat.h>
#include <linux/atomic.h>
#include <linux/rculist.h>

#include <asm/debug.h>
#include <asm/qdio.h>
#include <asm/airq.h>
#include <asm/isc.h>

#include "cio.h"
#include "ioasm.h"
#include "qdio.h"
#include "qdio_debug.h"

/*
 * Restriction: only 63 iqdio subchannels would have its own indicator,
 * after that, subsequent subchannels share one indicator
 */
#define TIQDIO_NR_NONSHARED_IND		63
#define TIQDIO_NR_INDICATORS		(TIQDIO_NR_NONSHARED_IND + 1)
#define TIQDIO_SHARED_IND		63

/* device state change indicators */
struct indicator_t {
	u32 ind;	/* u32 because of compare-and-swap performance */
	atomic_t count; /* use count, 0 or 1 for non-shared indicators */
};

/* list of thin interrupt input queues */
static LIST_HEAD(tiq_list);
static DEFINE_MUTEX(tiq_list_lock);

/* Adapter interrupt definitions */
static void tiqdio_thinint_handler(struct airq_struct *airq);

static struct airq_struct tiqdio_airq = {
	.handler = tiqdio_thinint_handler,
	.isc = QDIO_AIRQ_ISC,
};

static struct indicator_t *q_indicators;

u64 last_ai_time;

/* returns addr for the device state change indicator */
static u32 *get_indicator(void)
{
	int i;

	for (i = 0; i < TIQDIO_NR_NONSHARED_IND; i++)
		if (!atomic_cmpxchg(&q_indicators[i].count, 0, 1))
			return &q_indicators[i].ind;

	/* use the shared indicator */
	atomic_inc(&q_indicators[TIQDIO_SHARED_IND].count);
	return &q_indicators[TIQDIO_SHARED_IND].ind;
}

static void put_indicator(u32 *addr)
{
	struct indicator_t *ind = container_of(addr, struct indicator_t, ind);

	if (!addr)
		return;
	atomic_dec(&ind->count);
}

void tiqdio_add_input_queues(struct qdio_irq *irq_ptr)
{
	mutex_lock(&tiq_list_lock);
	list_add_rcu(&irq_ptr->input_qs[0]->entry, &tiq_list);
	mutex_unlock(&tiq_list_lock);
}

void tiqdio_remove_input_queues(struct qdio_irq *irq_ptr)
{
	struct qdio_q *q;

	q = irq_ptr->input_qs[0];
	if (!q)
		return;

	mutex_lock(&tiq_list_lock);
	list_del_rcu(&q->entry);
	mutex_unlock(&tiq_list_lock);
	synchronize_rcu();
	INIT_LIST_HEAD(&q->entry);
}

static inline int has_multiple_inq_on_dsci(struct qdio_irq *irq_ptr)
{
	return irq_ptr->nr_input_qs > 1;
}

static inline int references_shared_dsci(struct qdio_irq *irq_ptr)
{
	return irq_ptr->dsci == &q_indicators[TIQDIO_SHARED_IND].ind;
}

static inline int shared_ind(struct qdio_irq *irq_ptr)
{
	return references_shared_dsci(irq_ptr) ||
		has_multiple_inq_on_dsci(irq_ptr);
}

void clear_nonshared_ind(struct qdio_irq *irq_ptr)
{
	if (!is_thinint_irq(irq_ptr))
		return;
	if (shared_ind(irq_ptr))
		return;
	xchg(irq_ptr->dsci, 0);
}

int test_nonshared_ind(struct qdio_irq *irq_ptr)
{
	if (!is_thinint_irq(irq_ptr))
		return 0;
	if (shared_ind(irq_ptr))
		return 0;
	if (*irq_ptr->dsci)
		return 1;
	else
		return 0;
}

static inline u32 clear_shared_ind(void)
{
	if (!atomic_read(&q_indicators[TIQDIO_SHARED_IND].count))
		return 0;
	return xchg(&q_indicators[TIQDIO_SHARED_IND].ind, 0);
}

static inline void tiqdio_call_inq_handlers(struct qdio_irq *irq)
{
	struct qdio_q *q;
	int i;

	if (!references_shared_dsci(irq) &&
	    has_multiple_inq_on_dsci(irq))
		xchg(irq->dsci, 0);

	for_each_input_queue(irq, q, i) {
		if (q->u.in.queue_start_poll) {
			/* skip if polling is enabled or already in work */
			if (test_and_set_bit(QDIO_QUEUE_IRQS_DISABLED,
					     &q->u.in.queue_irq_state)) {
				qperf_inc(q, int_discarded);
				continue;
			}

			/* avoid dsci clear here, done after processing */
			q->u.in.queue_start_poll(irq->cdev, q->nr,
						 irq->int_parm);
		} else {
			if (!shared_ind(irq))
				xchg(irq->dsci, 0);

			/*
			 * Call inbound processing but not directly
			 * since that could starve other thinint queues.
			 */
			tasklet_schedule(&q->tasklet);
		}
	}
}

/**
 * tiqdio_thinint_handler - thin interrupt handler for qdio
 * @airq: pointer to adapter interrupt descriptor
 */
static void tiqdio_thinint_handler(struct airq_struct *airq)
{
	u32 si_used = clear_shared_ind();
	struct qdio_q *q;

	last_ai_time = S390_lowcore.int_clock;
	inc_irq_stat(IRQIO_QAI);

	/* protect tiq_list entries, only changed in activate or shutdown */
	rcu_read_lock();

	/* check for work on all inbound thinint queues */
	list_for_each_entry_rcu(q, &tiq_list, entry) {
		struct qdio_irq *irq;

		/* only process queues from changed sets */
		irq = q->irq_ptr;
		if (unlikely(references_shared_dsci(irq))) {
			if (!si_used)
				continue;
		} else if (!*irq->dsci)
			continue;

		tiqdio_call_inq_handlers(irq);

		qperf_inc(q, adapter_int);
	}
	rcu_read_unlock();
}

static int set_subchannel_ind(struct qdio_irq *irq_ptr, int reset)
{
	struct chsc_scssc_area *scssc = (void *)irq_ptr->chsc_page;
	u64 summary_indicator_addr, subchannel_indicator_addr;
	int rc;

	if (reset) {
		summary_indicator_addr = 0;
		subchannel_indicator_addr = 0;
	} else {
		summary_indicator_addr = virt_to_phys(tiqdio_airq.lsi_ptr);
		subchannel_indicator_addr = virt_to_phys(irq_ptr->dsci);
	}

	rc = chsc_sadc(irq_ptr->schid, scssc, summary_indicator_addr,
		       subchannel_indicator_addr);
	if (rc) {
		DBF_ERROR("%4x SSI r:%4x", irq_ptr->schid.sch_no,
			  scssc->response.code);
		goto out;
	}

	DBF_EVENT("setscind");
	DBF_HEX(&summary_indicator_addr, sizeof(summary_indicator_addr));
	DBF_HEX(&subchannel_indicator_addr, sizeof(subchannel_indicator_addr));
out:
	return rc;
}

/* allocate non-shared indicators and shared indicator */
int __init tiqdio_allocate_memory(void)
{
	q_indicators = kcalloc(TIQDIO_NR_INDICATORS,
			       sizeof(struct indicator_t),
			       GFP_KERNEL);
	if (!q_indicators)
		return -ENOMEM;
	return 0;
}

void tiqdio_free_memory(void)
{
	kfree(q_indicators);
}

int __init tiqdio_register_thinints(void)
{
	int rc;

	rc = register_adapter_interrupt(&tiqdio_airq);
	if (rc) {
		DBF_EVENT("RTI:%x", rc);
		return rc;
	}
	return 0;
}

int qdio_establish_thinint(struct qdio_irq *irq_ptr)
{
	int rc;

	if (!is_thinint_irq(irq_ptr))
		return 0;

	irq_ptr->dsci = get_indicator();
	DBF_HEX(&irq_ptr->dsci, sizeof(void *));

	rc = set_subchannel_ind(irq_ptr, 0);
	if (rc)
		put_indicator(irq_ptr->dsci);

	return rc;
}

void qdio_shutdown_thinint(struct qdio_irq *irq_ptr)
{
	if (!is_thinint_irq(irq_ptr))
		return;

	/* reset adapter interrupt indicators */
	set_subchannel_ind(irq_ptr, 1);
	put_indicator(irq_ptr->dsci);
}

void __exit tiqdio_unregister_thinints(void)
{
	WARN_ON(!list_empty(&tiq_list));
	unregister_adapter_interrupt(&tiqdio_airq);
}
