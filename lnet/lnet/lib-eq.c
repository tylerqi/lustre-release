/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2003, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2012, 2016, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lnet/lnet/lib-eq.c
 *
 * Library level Event queue management routines
 */

#define DEBUG_SUBSYSTEM S_LNET
#include <lnet/lib-lnet.h>

/**
 * Create an event queue that has room for \a count number of events.
 *
 * The event queue is circular and older events will be overwritten by
 * new ones if they are not removed in time by the user using the
 * function LNetEQPoll().  It is up to the user to determine the
 * appropriate size of the event queue to prevent this loss of events.
 * Note that when EQ handler is specified in \a callback, no event loss
 * can happen, since the handler is run for each event deposited into
 * the EQ.
 *
 * \param count The number of events to be stored in the event queue. It
 * will be rounded up to the next power of two.
 * \param callback A handler function that runs when an event is deposited
 * into the EQ. The constant value LNET_EQ_HANDLER_NONE can be used to
 * indicate that no event handler is desired.
 *
 * \retval eq	   On successful return, the newly created EQ is returned.
 *		   On failure, an error code encoded with ERR_PTR() is returned.
 * \retval -EINVAL If an parameter is not valid.
 * \retval -ENOMEM If memory for the EQ can't be allocated.
 *
 * \see lnet_eq_handler_t for the discussion on EQ handler semantics.
 */
struct lnet_eq *
LNetEQAlloc(unsigned int count, lnet_eq_handler_t callback)
{
	struct lnet_eq *eq;

	LASSERT(the_lnet.ln_refcount > 0);

	/* We need count to be a power of 2 so that when eq_{enq,deq}_seq
	 * overflow, they don't skip entries, so the queue has the same
	 * apparent capacity at all times */

	if (count)
		count = roundup_pow_of_two(count);

	if (callback != LNET_EQ_HANDLER_NONE && count != 0) {
		CWARN("EQ callback is guaranteed to get every event, "
		      "do you still want to set eqcount %d for polling "
		      "event which will have locking overhead? "
		      "Please contact with developer to confirm\n", count);
	}

	/* count can be 0 if only need callback, we can eliminate
	 * overhead of enqueue event */
	if (count == 0 && callback == LNET_EQ_HANDLER_NONE)
		return ERR_PTR(-EINVAL);

	eq = lnet_eq_alloc();
	if (eq == NULL)
		return ERR_PTR(-ENOMEM);

	if (count != 0) {
		LIBCFS_ALLOC(eq->eq_events, count * sizeof(*eq->eq_events));
		if (eq->eq_events == NULL)
			goto failed;
		/* NB allocator has set all event sequence numbers to 0,
		 * so all them should be earlier than eq_deq_seq */
	}

	eq->eq_deq_seq = 1;
	eq->eq_enq_seq = 1;
	eq->eq_size = count;
	eq->eq_callback = callback;

	eq->eq_refs = cfs_percpt_alloc(lnet_cpt_table(),
				       sizeof(*eq->eq_refs[0]));
	if (eq->eq_refs == NULL)
		goto failed;

	return eq;

failed:
	if (eq->eq_events != NULL)
		LIBCFS_FREE(eq->eq_events, count * sizeof(*eq->eq_events));

	if (eq->eq_refs != NULL)
		cfs_percpt_free(eq->eq_refs);

	lnet_eq_free(eq);
	return ERR_PTR(-ENOMEM);
}
EXPORT_SYMBOL(LNetEQAlloc);

/**
 * Release the resources associated with an event queue if it's idle;
 * otherwise do nothing and it's up to the user to try again.
 *
 * \param eq The event queue to be released.
 *
 * \retval 0 If the EQ is not in use and freed.
 * \retval -EBUSY  If the EQ is still in use by some MDs.
 */
int
LNetEQFree(struct lnet_eq *eq)
{
	struct lnet_event	*events = NULL;
	int		**refs = NULL;
	int		*ref;
	int		rc = 0;
	int		size = 0;
	int		i;

	lnet_res_lock(LNET_LOCK_EX);
	/* NB: hold lnet_eq_wait_lock for EQ link/unlink, so we can do
	 * both EQ lookup and poll event with only lnet_eq_wait_lock */
	lnet_eq_wait_lock();

	cfs_percpt_for_each(ref, i, eq->eq_refs) {
		LASSERT(*ref >= 0);
		if (*ref == 0)
			continue;

		CDEBUG(D_NET, "Event equeue (%d: %d) busy on destroy.\n",
		       i, *ref);
		rc = -EBUSY;
		goto out;
	}

	/* stash for free after lock dropped */
	events	= eq->eq_events;
	size	= eq->eq_size;
	refs	= eq->eq_refs;

	lnet_eq_free(eq);
 out:
	lnet_eq_wait_unlock();
	lnet_res_unlock(LNET_LOCK_EX);

	if (events != NULL)
		LIBCFS_FREE(events, size * sizeof(*events));
	if (refs != NULL)
		cfs_percpt_free(refs);

	return rc;
}
EXPORT_SYMBOL(LNetEQFree);

void
lnet_eq_enqueue_event(struct lnet_eq *eq, struct lnet_event *ev)
{
	/* MUST called with resource lock hold but w/o lnet_eq_wait_lock */
	int index;

	if (eq->eq_size == 0) {
		LASSERT(eq->eq_callback != LNET_EQ_HANDLER_NONE);
		eq->eq_callback(ev);
		return;
	}

	lnet_eq_wait_lock();
	ev->sequence = eq->eq_enq_seq++;

	LASSERT(eq->eq_size == LOWEST_BIT_SET(eq->eq_size));
	index = ev->sequence & (eq->eq_size - 1);

	eq->eq_events[index] = *ev;

	if (eq->eq_callback != LNET_EQ_HANDLER_NONE)
		eq->eq_callback(ev);

	/* Wake anyone waiting in LNetEQPoll() */
	if (waitqueue_active(&the_lnet.ln_eq_waitq))
		wake_up_all(&the_lnet.ln_eq_waitq);
	lnet_eq_wait_unlock();
}

static int
lnet_eq_dequeue_event(struct lnet_eq *eq, struct lnet_event *ev)
{
	int		new_index = eq->eq_deq_seq & (eq->eq_size - 1);
	struct lnet_event	*new_event = &eq->eq_events[new_index];
	int		rc;
	ENTRY;

	/* must called with lnet_eq_wait_lock hold */
	if (LNET_SEQ_GT(eq->eq_deq_seq, new_event->sequence))
		RETURN(0);

	/* We've got a new event... */
	*ev = *new_event;

	CDEBUG(D_INFO, "event: %p, sequence: %lu, eq->size: %u\n",
	       new_event, eq->eq_deq_seq, eq->eq_size);

	/* ...but did it overwrite an event we've not seen yet? */
	if (eq->eq_deq_seq == new_event->sequence) {
		rc = 1;
	} else {
		/* don't complain with CERROR: some EQs are sized small
		 * anyway; if it's important, the caller should complain */
		CDEBUG(D_NET, "Event Queue Overflow: eq seq %lu ev seq %lu\n",
		       eq->eq_deq_seq, new_event->sequence);
		rc = -EOVERFLOW;
	}

	eq->eq_deq_seq = new_event->sequence + 1;
	RETURN(rc);
}

static int
lnet_eq_wait_locked(signed long *timeout)
__must_hold(&the_lnet.ln_eq_wait_lock)
{
	signed long tms = *timeout;
	wait_queue_entry_t wl;
	int wait;

	if (tms == 0)
		return -ENXIO; /* don't want to wait and no new event */

	init_waitqueue_entry(&wl, current);
	add_wait_queue(&the_lnet.ln_eq_waitq, &wl);

	lnet_eq_wait_unlock();

	tms = schedule_timeout_interruptible(tms);
	wait = tms != 0; /* might need to call here again */
	*timeout = tms;

	lnet_eq_wait_lock();
	remove_wait_queue(&the_lnet.ln_eq_waitq, &wl);

	return wait;
}

/**
 * Block the calling process until there's an event from a set of EQs or
 * timeout happens.
 *
 * If an event handler is associated with the EQ, the handler will run before
 * this function returns successfully, in which case the corresponding event
 * is consumed.
 *
 * LNetEQPoll() provides a timeout to allow applications to poll, block for a
 * fixed period, or block indefinitely.
 *
 * \param eventqs,neq An array of lnet_eq, and size of the array.
 * \param timeout Time in jiffies to wait for an event to occur on
 * one of the EQs. The constant MAX_SCHEDULE_TIMEOUT can be used to indicate an
 * infinite timeout.
 * \param event,which On successful return (1 or -EOVERFLOW), \a event will
 * hold the next event in the EQs, and \a which will contain the index of the
 * EQ from which the event was taken.
 *
 * \retval 0	      No pending event in the EQs after timeout.
 * \retval 1	      Indicates success.
 * \retval -EOVERFLOW Indicates success (i.e., an event is returned) and that
 * at least one event between this event and the last event obtained from the
 * EQ indicated by \a which has been dropped due to limited space in the EQ.
 * \retval -ENOENT    If there's an invalid handle in \a eventqs.
 */
int
LNetEQPoll(struct lnet_eq **eventqs, int neq, signed long timeout,
	   struct lnet_event *event, int *which)
{
	int	wait = 1;
	int	rc;
	int	i;
	ENTRY;

	LASSERT(the_lnet.ln_refcount > 0);

	if (neq < 1)
		RETURN(-ENOENT);

	lnet_eq_wait_lock();

	for (;;) {
		for (i = 0; i < neq; i++) {
			struct lnet_eq *eq = eventqs[i];

			if (eq == NULL) {
				lnet_eq_wait_unlock();
				RETURN(-ENOENT);
			}

			rc = lnet_eq_dequeue_event(eq, event);
			if (rc != 0) {
				lnet_eq_wait_unlock();
				*which = i;
				RETURN(rc);
			}
		}

		if (wait == 0)
			break;

		/*
		 * return value of lnet_eq_wait_locked:
		 * -1 : did nothing and it's sure no new event
		 *  1 : sleep inside and wait until new event
		 *  0 : don't want to wait anymore, but might have new event
		 *	so need to call dequeue again
		 */
		wait = lnet_eq_wait_locked(&timeout);
		if (wait < 0) /* no new event */
			break;
	}

	lnet_eq_wait_unlock();
	RETURN(0);
}
