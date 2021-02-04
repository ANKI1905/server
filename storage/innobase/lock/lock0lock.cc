/*****************************************************************************

Copyright (c) 1996, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2014, 2021, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file lock/lock0lock.cc
The transaction lock system

Created 5/7/1996 Heikki Tuuri
*******************************************************/

#define LOCK_MODULE_IMPLEMENTATION

#include "univ.i"

#include <mysql/service_thd_error_context.h>
#include <mysql/service_thd_wait.h>
#include <sql_class.h>

#include "lock0lock.h"
#include "lock0priv.h"
#include "dict0mem.h"
#include "trx0purge.h"
#include "trx0sys.h"
#include "ut0vec.h"
#include "btr0cur.h"
#include "row0sel.h"
#include "row0mysql.h"
#include "row0vers.h"
#include "pars0pars.h"
#include "srv0mon.h"

#include <unordered_set>
#ifdef UNIV_DEBUG
# include <set>
#endif

#ifdef WITH_WSREP
#include <mysql/service_wsrep.h>
#endif /* WITH_WSREP */

/** The value of innodb_deadlock_detect */
my_bool innodb_deadlock_detect;
/** The value of innodb_deadlock_report */
ulong innodb_deadlock_report;

#ifdef HAVE_REPLICATION
extern "C" void thd_rpl_deadlock_check(MYSQL_THD thd, MYSQL_THD other_thd);
extern "C" int thd_need_wait_reports(const MYSQL_THD thd);
extern "C" int thd_need_ordering_with(const MYSQL_THD thd, const MYSQL_THD other_thd);
#endif

/** Pretty-print a table lock.
@param[in,out]	file	output stream
@param[in]	lock	table lock */
static void lock_table_print(FILE* file, const lock_t* lock);

/** Pretty-print a record lock.
@param[in,out]	file	output stream
@param[in]	lock	record lock
@param[in,out]	mtr	mini-transaction for accessing the record */
static void lock_rec_print(FILE* file, const lock_t* lock, mtr_t& mtr);

namespace Deadlock
{
  MY_ATTRIBUTE((nonnull, warn_unused_result))
  /** Check if a lock request results in a deadlock.
  Resolve a deadlock by choosing a transaction that will be rolled back.
  @param trx    transaction requesting a lock
  @return whether trx must report DB_DEADLOCK */
  static bool check_and_resolve(trx_t *trx);

  /** Quickly detect a deadlock using Brent's cycle detection algorithm.
  @param trx     transaction that is waiting for another transaction
  @return a transaction that is part of a cycle
  @retval nullptr if no cycle was found */
  inline trx_t *find_cycle(trx_t *trx)
  {
    mysql_mutex_assert_owner(&lock_sys.wait_mutex);
    trx_t *tortoise= trx, *hare= trx;
    for (unsigned power= 1, l= 1; (hare= hare->lock.wait_trx) != nullptr; l++)
    {
      if (tortoise == hare)
      {
        ut_ad(l > 1);
        lock_sys.deadlocks++;
        /* Note: Normally, trx should be part of any deadlock cycle
        that is found. However, if innodb_deadlock_detect=OFF had been
        in effect in the past, it is possible that trx will be waiting
        for a transaction that participates in a pre-existing deadlock
        cycle. In that case, our victim will not be trx. */
        return hare;
      }
      if (l == power)
      {
        /* The maximum concurrent number of TRX_STATE_ACTIVE transactions
        is TRX_RSEG_N_SLOTS * 128, or innodb_page_size / 16 * 128
        (default: 131,072, maximum: 524,288).
        Our maximum possible number of iterations should be twice that. */
        power<<= 1;
        l= 0;
        tortoise= hare;
      }
    }
    return nullptr;
  }
};

#ifdef UNIV_DEBUG
/*********************************************************************//**
Validates the lock system.
@return TRUE if ok */
static
bool
lock_validate();
/*============*/

/** Validate the record lock queues on a page.
@param block    buffer pool block
@param latched  whether the tablespace latch may be held
@return true if ok */
static bool lock_rec_validate_page(const buf_block_t *block, bool latched)
  MY_ATTRIBUTE((nonnull, warn_unused_result));
#endif /* UNIV_DEBUG */

/* The lock system */
lock_sys_t lock_sys;

/** Only created if !srv_read_only_mode. Protected by lock_sys.mutex. */
static FILE *lock_latest_err_file;

/*********************************************************************//**
Reports that a transaction id is insensible, i.e., in the future. */
ATTRIBUTE_COLD
void
lock_report_trx_id_insanity(
/*========================*/
	trx_id_t	trx_id,		/*!< in: trx id */
	const rec_t*	rec,		/*!< in: user record */
	dict_index_t*	index,		/*!< in: index */
	const rec_offs*	offsets,	/*!< in: rec_get_offsets(rec, index) */
	trx_id_t	max_trx_id)	/*!< in: trx_sys.get_max_trx_id() */
{
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(!rec_is_metadata(rec, *index));

	ib::error()
		<< "Transaction id " << ib::hex(trx_id)
		<< " associated with record" << rec_offsets_print(rec, offsets)
		<< " in index " << index->name
		<< " of table " << index->table->name
		<< " is greater than the global counter " << max_trx_id
		<< "! The table is corrupted.";
}

/*********************************************************************//**
Checks that a transaction id is sensible, i.e., not in the future.
@return true if ok */
bool
lock_check_trx_id_sanity(
/*=====================*/
	trx_id_t	trx_id,		/*!< in: trx id */
	const rec_t*	rec,		/*!< in: user record */
	dict_index_t*	index,		/*!< in: index */
	const rec_offs*	offsets)	/*!< in: rec_get_offsets(rec, index) */
{
  ut_ad(rec_offs_validate(rec, index, offsets));
  ut_ad(!rec_is_metadata(rec, *index));

  trx_id_t max_trx_id= trx_sys.get_max_trx_id();
  ut_ad(max_trx_id || srv_force_recovery >= SRV_FORCE_NO_UNDO_LOG_SCAN);

  if (UNIV_LIKELY(max_trx_id != 0) && UNIV_UNLIKELY(trx_id >= max_trx_id))
  {
    lock_report_trx_id_insanity(trx_id, rec, index, offsets, max_trx_id);
    return false;
  }
  return true;
}


/**
  Creates the lock system at database start.

  @param[in] n_cells number of slots in lock hash table
*/
void lock_sys_t::create(ulint n_cells)
{
  ut_ad(this == &lock_sys);
  ut_ad(!is_initialised());

  m_initialised= true;

  mysql_mutex_init(lock_mutex_key, &mutex, nullptr);
  mysql_mutex_init(lock_wait_mutex_key, &wait_mutex, nullptr);

  rec_hash.create(n_cells);
  prdt_hash.create(n_cells);
  prdt_page_hash.create(n_cells);

  if (!srv_read_only_mode)
  {
    lock_latest_err_file= os_file_create_tmpfile();
    ut_a(lock_latest_err_file);
  }
}


#ifdef HAVE_PSI_MUTEX_INTERFACE
/** Try to acquire lock_sys.mutex */
int lock_sys_t::mutex_trylock() { return mysql_mutex_trylock(&mutex); }
/** Acquire lock_sys.mutex */
void lock_sys_t::mutex_lock() { mysql_mutex_lock(&mutex); }
/** Release lock_sys.mutex */
void lock_sys_t::mutex_unlock() { mysql_mutex_unlock(&mutex); }
#endif


/** Calculates the fold value of a lock: used in migrating the hash table.
@param[in]	lock	record lock object
@return	folded value */
static ulint lock_rec_lock_fold(const lock_t *lock)
{
  return lock->un_member.rec_lock.page_id.fold();
}


/**
  Resize the lock hash table.

  @param[in] n_cells number of slots in lock hash table
*/
void lock_sys_t::resize(ulint n_cells)
{
	ut_ad(this == &lock_sys);

	mutex_lock();

	hash_table_t old_hash(rec_hash);
	rec_hash.create(n_cells);
	HASH_MIGRATE(&old_hash, &rec_hash, lock_t, hash,
		     lock_rec_lock_fold);
	old_hash.free();

	old_hash = prdt_hash;
	prdt_hash.create(n_cells);
	HASH_MIGRATE(&old_hash, &prdt_hash, lock_t, hash,
		     lock_rec_lock_fold);
	old_hash.free();

	old_hash = prdt_page_hash;
	prdt_page_hash.create(n_cells);
	HASH_MIGRATE(&old_hash, &prdt_page_hash, lock_t, hash,
		     lock_rec_lock_fold);
	old_hash.free();
	mutex_unlock();
}


/** Closes the lock system at database shutdown. */
void lock_sys_t::close()
{
  ut_ad(this == &lock_sys);

  if (!m_initialised)
    return;

  if (lock_latest_err_file)
  {
    my_fclose(lock_latest_err_file, MYF(MY_WME));
    lock_latest_err_file= nullptr;
  }

  rec_hash.free();
  prdt_hash.free();
  prdt_page_hash.free();

  mysql_mutex_destroy(&mutex);
  mysql_mutex_destroy(&wait_mutex);

  m_initialised= false;
}

#ifdef WITH_WSREP
/** Check if both conflicting lock and other record lock are brute force
(BF). This case is a bug so report lock information and wsrep state.
@param[in]	lock_rec1	conflicting waiting record lock or NULL
@param[in]	lock_rec2	other waiting record lock
@param[in]	trx1		lock_rec1 can be NULL, trx
*/
static void wsrep_assert_no_bf_bf_wait(
	const lock_t* lock_rec1,
	const lock_t* lock_rec2,
	const trx_t* trx1)
{
	ut_ad(!lock_rec1 || !lock_rec1->is_table());
	ut_ad(!lock_rec2->is_table());

	if (!trx1->is_wsrep() || !lock_rec2->trx->is_wsrep())
		return;
	if (UNIV_LIKELY(!wsrep_thd_is_BF(trx1->mysql_thd, FALSE)))
		return;
	if (UNIV_LIKELY(!wsrep_thd_is_BF(lock_rec2->trx->mysql_thd, FALSE)))
		return;

	/* if BF - BF order is honored, we can keep trx1 waiting for the lock */
	if (wsrep_thd_order_before(trx1->mysql_thd, lock_rec2->trx->mysql_thd))
		return;

	/* avoiding BF-BF conflict assert, if victim is already aborting
	   or rolling back for replaying
	*/
	wsrep_thd_LOCK(lock_rec2->trx->mysql_thd);
	if (wsrep_thd_is_aborting(lock_rec2->trx->mysql_thd)) {
		wsrep_thd_UNLOCK(lock_rec2->trx->mysql_thd);
		return;
	}
	wsrep_thd_UNLOCK(lock_rec2->trx->mysql_thd);

	mtr_t mtr;

	if (lock_rec1) {
		ib::error() << "Waiting lock on table: "
			    << lock_rec1->index->table->name
			    << " index: "
			    << lock_rec1->index->name()
			    << " that has conflicting lock ";
		lock_rec_print(stderr, lock_rec1, mtr);
	}

	ib::error() << "Conflicting lock on table: "
		    << lock_rec2->index->table->name
		    << " index: "
		    << lock_rec2->index->name()
		    << " that has lock ";
	lock_rec_print(stderr, lock_rec2, mtr);

	ib::error() << "WSREP state: ";

	wsrep_report_bf_lock_wait(trx1->mysql_thd,
				  trx1->id);
	wsrep_report_bf_lock_wait(lock_rec2->trx->mysql_thd,
				  lock_rec2->trx->id);
	/* BF-BF wait is a bug */
	ut_error;
}

/*********************************************************************//**
check if lock timeout was for priority thread,
as a side effect trigger lock monitor
@param[in]    trx    transaction owning the lock
@param[in]    locked true if trx and lock_sys.latch is held
@return	false for regular lock timeout */
static
bool
wsrep_is_BF_lock_timeout(
	const trx_t*	trx,
	bool		locked = true)
{
	if (trx->error_state != DB_DEADLOCK && trx->is_wsrep() &&
	    srv_monitor_timer && wsrep_thd_is_BF(trx->mysql_thd, FALSE)) {
		ib::info() << "WSREP: BF lock wait long for trx:" << ib::hex(trx->id)
			   << " query: " << wsrep_thd_query(trx->mysql_thd);
		if (!locked) {
			LockMutexGuard g;
			trx_print_latched(stderr, trx, 3000);
		} else {
			lock_sys.mutex_assert_locked();
			trx_print_latched(stderr, trx, 3000);
		}

		srv_print_innodb_monitor 	= TRUE;
		srv_print_innodb_lock_monitor 	= TRUE;
		srv_monitor_timer_schedule_now();
		return true;
	}
	return false;
}
#endif /* WITH_WSREP */

/*********************************************************************//**
Checks if a lock request for a new lock has to wait for request lock2.
@return TRUE if new lock has to wait for lock2 to be removed */
UNIV_INLINE
bool
lock_rec_has_to_wait(
/*=================*/
	bool		for_locking,
				/*!< in is called locking or releasing */
	const trx_t*	trx,	/*!< in: trx of new lock */
	unsigned	type_mode,/*!< in: precise mode of the new lock
				to set: LOCK_S or LOCK_X, possibly
				ORed to LOCK_GAP or LOCK_REC_NOT_GAP,
				LOCK_INSERT_INTENTION */
	const lock_t*	lock2,	/*!< in: another record lock; NOTE that
				it is assumed that this has a lock bit
				set on the same record as in the new
				lock we are setting */
	bool		lock_is_on_supremum)
				/*!< in: TRUE if we are setting the
				lock on the 'supremum' record of an
				index page: we know then that the lock
				request is really for a 'gap' type lock */
{
	ut_ad(trx);
	ut_ad(!lock2->is_table());

	if (trx == lock2->trx
	    || lock_mode_compatible(
		       static_cast<lock_mode>(LOCK_MODE_MASK & type_mode),
		       lock2->mode())) {
		return false;
	}

	/* We have somewhat complex rules when gap type record locks
	cause waits */

	if ((lock_is_on_supremum || (type_mode & LOCK_GAP))
	    && !(type_mode & LOCK_INSERT_INTENTION)) {

		/* Gap type locks without LOCK_INSERT_INTENTION flag
		do not need to wait for anything. This is because
		different users can have conflicting lock types
		on gaps. */

		return false;
	}

	if (!(type_mode & LOCK_INSERT_INTENTION) && lock2->is_gap()) {

		/* Record lock (LOCK_ORDINARY or LOCK_REC_NOT_GAP
		does not need to wait for a gap type lock */

		return false;
	}

	if ((type_mode & LOCK_GAP) && lock2->is_record_not_gap()) {

		/* Lock on gap does not need to wait for
		a LOCK_REC_NOT_GAP type lock */

		return false;
	}

	if (lock2->is_insert_intention()) {
		/* No lock request needs to wait for an insert
		intention lock to be removed. This is ok since our
		rules allow conflicting locks on gaps. This eliminates
		a spurious deadlock caused by a next-key lock waiting
		for an insert intention lock; when the insert
		intention lock was granted, the insert deadlocked on
		the waiting next-key lock.

		Also, insert intention locks do not disturb each
		other. */

		return false;
	}

#ifdef HAVE_REPLICATION
	if ((type_mode & LOCK_GAP || lock2->is_gap())
	    && !thd_need_ordering_with(trx->mysql_thd, lock2->trx->mysql_thd)) {
		/* If the upper server layer has already decided on the
		commit order between the transaction requesting the
		lock and the transaction owning the lock, we do not
		need to wait for gap locks. Such ordeering by the upper
		server layer happens in parallel replication, where the
		commit order is fixed to match the original order on the
		master.

		Such gap locks are mainly needed to get serialisability
		between transactions so that they will be binlogged in
		the correct order so that statement-based replication
		will give the correct results. Since the right order
		was already determined on the master, we do not need
		to enforce it again here.

		Skipping the locks is not essential for correctness,
		since in case of deadlock we will just kill the later
		transaction and retry it. But it can save some
		unnecessary rollbacks and retries. */

		return false;
	}
#endif /* HAVE_REPLICATION */

#ifdef WITH_WSREP
	/* There should not be two conflicting locks that are
	brute force. If there is it is a bug. */
	wsrep_assert_no_bf_bf_wait(NULL, lock2, trx);
#endif /* WITH_WSREP */

	return true;
}

/*********************************************************************//**
Checks if a lock request lock1 has to wait for request lock2.
@return TRUE if lock1 has to wait for lock2 to be removed */
bool
lock_has_to_wait(
/*=============*/
	const lock_t*	lock1,	/*!< in: waiting lock */
	const lock_t*	lock2)	/*!< in: another lock; NOTE that it is
				assumed that this has a lock bit set
				on the same record as in lock1 if the
				locks are record locks */
{
	ut_ad(lock1 && lock2);

	if (lock1->trx == lock2->trx
	    || lock_mode_compatible(lock1->mode(), lock2->mode())) {
		return false;
	}

	if (lock1->is_table()) {
		return true;
	}

	ut_ad(!lock2->is_table());

	if (lock1->type_mode & (LOCK_PREDICATE | LOCK_PRDT_PAGE)) {
		return lock_prdt_has_to_wait(lock1->trx, lock1->type_mode,
					     lock_get_prdt_from_lock(lock1),
					     lock2);
	}

	return lock_rec_has_to_wait(
		false, lock1->trx, lock1->type_mode, lock2,
		lock_rec_get_nth_bit(lock1, PAGE_HEAP_NO_SUPREMUM));
}

/*============== RECORD LOCK BASIC FUNCTIONS ============================*/

/**********************************************************************//**
Looks for a set bit in a record lock bitmap. Returns ULINT_UNDEFINED,
if none found.
@return bit index == heap number of the record, or ULINT_UNDEFINED if
none found */
ulint
lock_rec_find_set_bit(
/*==================*/
	const lock_t*	lock)	/*!< in: record lock with at least one bit set */
{
	for (ulint i = 0; i < lock_rec_get_n_bits(lock); ++i) {

		if (lock_rec_get_nth_bit(lock, i)) {

			return(i);
		}
	}

	return(ULINT_UNDEFINED);
}

/*********************************************************************//**
Resets the record lock bitmap to zero. NOTE: does not touch the wait_lock
pointer in the transaction! This function is used in lock object creation
and resetting. */
static
void
lock_rec_bitmap_reset(
/*==================*/
	lock_t*	lock)	/*!< in: record lock */
{
	ulint	n_bytes;

	ut_ad(!lock->is_table());

	/* Reset to zero the bitmap which resides immediately after the lock
	struct */

	n_bytes = lock_rec_get_n_bits(lock) / 8;

	ut_ad((lock_rec_get_n_bits(lock) % 8) == 0);

	memset(reinterpret_cast<void*>(&lock[1]), 0, n_bytes);
}

/*********************************************************************//**
Copies a record lock to heap.
@return copy of lock */
static
lock_t*
lock_rec_copy(
/*==========*/
	const lock_t*	lock,	/*!< in: record lock */
	mem_heap_t*	heap)	/*!< in: memory heap */
{
	ulint	size;

	ut_ad(!lock->is_table());

	size = sizeof(lock_t) + lock_rec_get_n_bits(lock) / 8;

	return(static_cast<lock_t*>(mem_heap_dup(heap, lock, size)));
}

/*********************************************************************//**
Gets the previous record lock set on a record.
@return previous lock on the same record, NULL if none exists */
const lock_t*
lock_rec_get_prev(
/*==============*/
	const lock_t*	in_lock,/*!< in: record lock */
	ulint		heap_no)/*!< in: heap number of the record */
{
	lock_t*		lock;
	lock_t*		found_lock	= NULL;

	ut_ad(!in_lock->is_table());
	const page_id_t id{in_lock->un_member.rec_lock.page_id};
	lock_sys.mutex_assert_locked();

	for (lock = lock_sys.get_first(*lock_hash_get(in_lock->type_mode), id);
	     lock != in_lock;
	     lock = lock_rec_get_next_on_page(lock)) {
		if (lock_rec_get_nth_bit(lock, heap_no)) {
			found_lock = lock;
		}
	}

	return found_lock;
}

/*============= FUNCTIONS FOR ANALYZING RECORD LOCK QUEUE ================*/

/*********************************************************************//**
Checks if a transaction has a GRANTED explicit lock on rec stronger or equal
to precise_mode.
@return lock or NULL */
UNIV_INLINE
lock_t*
lock_rec_has_expl(
/*==============*/
	ulint			precise_mode,/*!< in: LOCK_S or LOCK_X
					possibly ORed to LOCK_GAP or
					LOCK_REC_NOT_GAP, for a
					supremum record we regard this
					always a gap type request */
	const page_id_t		id,	/*!< in: page identifier */
	ulint			heap_no,/*!< in: heap number of the record */
	const trx_t*		trx)	/*!< in: transaction */
{
  ut_ad((precise_mode & LOCK_MODE_MASK) == LOCK_S
	|| (precise_mode & LOCK_MODE_MASK) == LOCK_X);
  ut_ad(!(precise_mode & LOCK_INSERT_INTENTION));

  for (lock_t *lock= lock_rec_get_first(&lock_sys.rec_hash, id, heap_no);
       lock; lock= lock_rec_get_next(heap_no, lock))
    if (lock->trx == trx &&
	!(lock->type_mode & (LOCK_WAIT | LOCK_INSERT_INTENTION)) &&
	(!((LOCK_REC_NOT_GAP | LOCK_GAP) & lock->type_mode) ||
	 heap_no == PAGE_HEAP_NO_SUPREMUM ||
	 ((LOCK_REC_NOT_GAP | LOCK_GAP) & precise_mode & lock->type_mode)) &&
	lock_mode_stronger_or_eq(lock->mode(), static_cast<lock_mode>
				 (precise_mode & LOCK_MODE_MASK)))
      return lock;

  return nullptr;
}

#ifdef UNIV_DEBUG
/*********************************************************************//**
Checks if some other transaction has a lock request in the queue.
@return lock or NULL */
static
lock_t*
lock_rec_other_has_expl_req(
/*========================*/
	lock_mode		mode,	/*!< in: LOCK_S or LOCK_X */
	const page_id_t		id,	/*!< in: page identifier */
	bool			wait,	/*!< in: whether also waiting locks
					are taken into account */
	ulint			heap_no,/*!< in: heap number of the record */
	const trx_t*		trx)	/*!< in: transaction, or NULL if
					requests by all transactions
					are taken into account */
{

	lock_sys.mutex_assert_locked();
	ut_ad(mode == LOCK_X || mode == LOCK_S);

	/* Only GAP lock can be on SUPREMUM, and we are not looking for
	GAP lock */
	if (heap_no == PAGE_HEAP_NO_SUPREMUM) {
		return(NULL);
	}

	for (lock_t* lock= lock_rec_get_first(&lock_sys.rec_hash, id, heap_no);
	     lock != NULL;
	     lock = lock_rec_get_next(heap_no, lock)) {

		if (lock->trx != trx
		    && !lock->is_gap()
		    && (!lock->is_waiting() || wait)
		    && lock_mode_stronger_or_eq(lock->mode(), mode)) {

			return(lock);
		}
	}

	return(NULL);
}
#endif /* UNIV_DEBUG */

#ifdef WITH_WSREP
static
void
wsrep_kill_victim(
/*==============*/
	const trx_t * const trx,
	const lock_t *lock)
{
	lock_sys.mutex_assert_locked();

	/* quit for native mysql */
	if (!trx->is_wsrep()) return;

	if (!wsrep_thd_is_BF(trx->mysql_thd, FALSE)) {
		return;
	}

	my_bool bf_other = wsrep_thd_is_BF(lock->trx->mysql_thd, FALSE);
	mtr_t mtr;

	if ((!bf_other) ||
		(wsrep_thd_order_before(
			trx->mysql_thd, lock->trx->mysql_thd))) {

		if (lock->trx->lock.wait_thr) {
			if (UNIV_UNLIKELY(wsrep_debug)) {
				ib::info() << "WSREP: BF victim waiting\n";
			}
			/* cannot release lock, until our lock
			is in the queue*/
		} else if (lock->trx != trx) {
			if (wsrep_log_conflicts) {
				ib::info() << "*** Priority TRANSACTION:";

				trx_print_latched(stderr, trx, 3000);

				if (bf_other) {
					ib::info() << "*** Priority TRANSACTION:";
				} else {
					ib::info() << "*** Victim TRANSACTION:";
				}
                                trx_print_latched(stderr, lock->trx, 3000);

				ib::info() << "*** WAITING FOR THIS LOCK TO BE GRANTED:";

				if (!lock->is_table()) {
					lock_rec_print(stderr, lock, mtr);
				} else {
					lock_table_print(stderr, lock);
				}

				ib::info() << " SQL1: "
					   << wsrep_thd_query(trx->mysql_thd);
				ib::info() << " SQL2: "
					   << wsrep_thd_query(lock->trx->mysql_thd);
			}

			wsrep_innobase_kill_one_trx(trx->mysql_thd,
						    lock->trx, true);
		}
	}
}
#endif /* WITH_WSREP */

/*********************************************************************//**
Checks if some other transaction has a conflicting explicit lock request
in the queue, so that we have to wait.
@return lock or NULL */
static
lock_t*
lock_rec_other_has_conflicting(
/*===========================*/
	unsigned		mode,	/*!< in: LOCK_S or LOCK_X,
					possibly ORed to LOCK_GAP or
					LOC_REC_NOT_GAP,
					LOCK_INSERT_INTENTION */
	const page_id_t		id,	/*!< in: page identifier */
	ulint			heap_no,/*!< in: heap number of the record */
	const trx_t*		trx)	/*!< in: our transaction */
{
	lock_t*		lock;

	lock_sys.mutex_assert_locked();

	bool	is_supremum = (heap_no == PAGE_HEAP_NO_SUPREMUM);

	for (lock = lock_rec_get_first(&lock_sys.rec_hash, id, heap_no);
	     lock != NULL;
	     lock = lock_rec_get_next(heap_no, lock)) {

		if (lock_rec_has_to_wait(true, trx, mode, lock, is_supremum)) {
#ifdef WITH_WSREP
			if (trx->is_wsrep()) {
				lock->trx->mutex.wr_lock();
				/* Below function will roll back either trx
				or lock->trx depending on priority of the
				transaction. */
				wsrep_kill_victim(const_cast<trx_t*>(trx), lock);
				lock->trx->mutex.wr_unlock();
			}
#endif /* WITH_WSREP */
			return(lock);
		}
	}

	return(NULL);
}

/*********************************************************************//**
Checks if some transaction has an implicit x-lock on a record in a secondary
index.
@return transaction id of the transaction which has the x-lock, or 0;
NOTE that this function can return false positives but never false
negatives. The caller must confirm all positive results by calling
trx_is_active(). */
static
trx_t*
lock_sec_rec_some_has_impl(
/*=======================*/
	trx_t*		caller_trx,/*!<in/out: trx of current thread */
	const rec_t*	rec,	/*!< in: user record */
	dict_index_t*	index,	/*!< in: secondary index */
	const rec_offs*	offsets)/*!< in: rec_get_offsets(rec, index) */
{
	trx_t*		trx;
	trx_id_t	max_trx_id;
	const page_t*	page = page_align(rec);

	lock_sys.mutex_assert_unlocked();
	ut_ad(!dict_index_is_clust(index));
	ut_ad(page_rec_is_user_rec(rec));
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(!rec_is_metadata(rec, *index));

	max_trx_id = page_get_max_trx_id(page);

	/* Some transaction may have an implicit x-lock on the record only
	if the max trx id for the page >= min trx id for the trx list, or
	database recovery is running. */

	if (max_trx_id < trx_sys.get_min_trx_id()) {

		trx = 0;

	} else if (!lock_check_trx_id_sanity(max_trx_id, rec, index, offsets)) {

		/* The page is corrupt: try to avoid a crash by returning 0 */
		trx = 0;

	/* In this case it is possible that some transaction has an implicit
	x-lock. We have to look in the clustered index. */

	} else {
		trx = row_vers_impl_x_locked(caller_trx, rec, index, offsets);
	}

	return(trx);
}

/*********************************************************************//**
Return the number of table locks for a transaction.
The caller must be holding lock_sys.mutex. */
ulint
lock_number_of_tables_locked(
/*=========================*/
	const trx_lock_t*	trx_lock)	/*!< in: transaction locks */
{
	const lock_t*	lock;
	ulint		n_tables = 0;

	lock_sys.mutex_assert_locked();

	for (lock = UT_LIST_GET_FIRST(trx_lock->trx_locks);
	     lock != NULL;
	     lock = UT_LIST_GET_NEXT(trx_locks, lock)) {

		if (lock->is_table()) {
			n_tables++;
		}
	}

	return(n_tables);
}

/*============== RECORD LOCK CREATION AND QUEUE MANAGEMENT =============*/

#ifdef WITH_WSREP
ATTRIBUTE_COLD
static
void
wsrep_print_wait_locks(
/*===================*/
	lock_t*		c_lock) /* conflicting lock to print */
{
	const lock_t *wait_lock = c_lock->trx->lock.wait_lock;

	if (wait_lock != c_lock) {
		mtr_t mtr;
		ib::info() << "WSREP: c_lock != wait lock";
		ib::info() << " SQL: "
			   << wsrep_thd_query(c_lock->trx->mysql_thd);

		if (c_lock->is_table()) {
			lock_table_print(stderr, c_lock);
		} else {
			lock_rec_print(stderr, c_lock, mtr);
		}

		if (wait_lock->is_table()) {
			lock_table_print(stderr, wait_lock);
		} else {
			lock_rec_print(stderr, wait_lock, mtr);
		}
	}
}
#endif /* WITH_WSREP */

/** Reset the wait status of a lock.
@param[in,out]	lock	lock that was possibly being waited for */
static void lock_reset_lock_and_trx_wait(lock_t *lock)
{
  lock_sys.mutex_assert_locked();
  mysql_mutex_assert_owner(&lock_sys.wait_mutex);
  trx_t *trx= lock->trx;
  ut_ad(lock->is_waiting());
  ut_ad(!trx->lock.wait_lock || trx->lock.wait_lock == lock);
  trx->lock.wait_lock= nullptr;
  trx->lock.wait_trx= nullptr;
  lock->type_mode&= ~LOCK_WAIT;
}

#ifdef WITH_WSREP
/** Set the wait status of a lock.
@param[in,out]	lock	lock that will be waited for
@param[in,out]	trx	transaction that will wait for the lock
@param[in]      ctrx	holder of the conflicting lock */
static void lock_set_lock_and_trx_wait(lock_t *lock, trx_t *trx, trx_t *ctrx)
{
  ut_ad(lock);
  ut_ad(lock->trx == trx);
  lock_sys.mutex_assert_locked();
  if (trx->lock.wait_trx)
  {
    ut_ad(trx->lock.wait_trx == ctrx);
    ut_ad(trx->lock.wait_lock != lock);
    ut_ad((*trx->lock.wait_lock).trx == trx);
  }
  else
  {
    trx->lock.wait_trx= ctrx;
    ut_ad(!trx->lock.wait_lock);
  }

  trx->lock.wait_lock= lock;
  lock->type_mode|= LOCK_WAIT;
}
#endif

/** Create a new record lock and inserts it to the lock queue,
without checking for deadlocks or conflicts.
@param[in]	c_lock		conflicting lock
@param[in]	type_mode	lock mode and wait flag
@param[in]	page_id		index page number
@param[in]	page		R-tree index page, or NULL
@param[in]	heap_no		record heap number in the index page
@param[in]	index		the index tree
@param[in,out]	trx		transaction
@param[in]	holds_trx_mutex	whether the caller holds trx->mutex
@return created lock */
lock_t*
lock_rec_create_low(
	lock_t*		c_lock,
#ifdef WITH_WSREP
	que_thr_t*	thr,	/*!< thread owning trx */
#endif
	unsigned	type_mode,
	const page_id_t	page_id,
	const page_t*	page,
	ulint		heap_no,
	dict_index_t*	index,
	trx_t*		trx,
	bool		holds_trx_mutex)
{
	lock_t*		lock;
	ulint		n_bytes;

	lock_sys.mutex_assert_locked();
	ut_ad(dict_index_is_clust(index) || !dict_index_is_online_ddl(index));
	ut_ad(!(type_mode & LOCK_TABLE));

#ifdef UNIV_DEBUG
	/* Non-locking autocommit read-only transactions should not set
	any locks. See comment in trx_set_rw_mode explaining why this
	conditional check is required in debug code. */
	if (holds_trx_mutex) {
		check_trx_state(trx);
	}
#endif /* UNIV_DEBUG */

	/* If rec is the supremum record, then we reset the gap and
	LOCK_REC_NOT_GAP bits, as all locks on the supremum are
	automatically of the gap type */

	if (UNIV_UNLIKELY(heap_no == PAGE_HEAP_NO_SUPREMUM)) {
		ut_ad(!(type_mode & LOCK_REC_NOT_GAP));
		type_mode = type_mode & ~(LOCK_GAP | LOCK_REC_NOT_GAP);
	}

	if (UNIV_LIKELY(!(type_mode & (LOCK_PREDICATE | LOCK_PRDT_PAGE)))) {
		n_bytes = (page_dir_get_n_heap(page) + 7) / 8;
	} else {
		ut_ad(heap_no == PRDT_HEAPNO);

		/* The lock is always on PAGE_HEAP_NO_INFIMUM (0), so
		we only need 1 bit (which round up to 1 byte) for
		lock bit setting */
		n_bytes = 1;

		if (type_mode & LOCK_PREDICATE) {
			ulint	tmp = UNIV_WORD_SIZE - 1;

			/* We will attach predicate structure after lock.
			Make sure the memory is aligned on 8 bytes,
			the mem_heap_alloc will align it with
			MEM_SPACE_NEEDED anyway. */
			n_bytes = (n_bytes + sizeof(lock_prdt_t) + tmp) & ~tmp;
			ut_ad(n_bytes == sizeof(lock_prdt_t) + UNIV_WORD_SIZE);
		}
	}

	if (trx->lock.rec_cached >= UT_ARR_SIZE(trx->lock.rec_pool)
	    || sizeof *lock + n_bytes > sizeof *trx->lock.rec_pool) {
		lock = static_cast<lock_t*>(
			mem_heap_alloc(trx->lock.lock_heap,
				       sizeof *lock + n_bytes));
	} else {
		lock = &trx->lock.rec_pool[trx->lock.rec_cached++].lock;
	}

	lock->trx = trx;
	lock->type_mode = type_mode;
	lock->index = index;
	lock->un_member.rec_lock.page_id = page_id;

	if (UNIV_LIKELY(!(type_mode & (LOCK_PREDICATE | LOCK_PRDT_PAGE)))) {
		lock->un_member.rec_lock.n_bits = uint32_t(n_bytes * 8);
	} else {
		/* Predicate lock always on INFIMUM (0) */
		lock->un_member.rec_lock.n_bits = 8;
 	}
	lock_rec_bitmap_reset(lock);
	lock_rec_set_nth_bit(lock, heap_no);
	index->table->n_rec_locks++;
	ut_ad(index->table->get_ref_count() > 0 || !index->table->can_be_evicted);

#ifdef WITH_WSREP
	if (c_lock && trx->is_wsrep()
	    && wsrep_thd_is_BF(trx->mysql_thd, FALSE)) {
		lock_t *hash	= (lock_t *)c_lock->hash;
		lock_t *prev	= NULL;

		while (hash && wsrep_thd_is_BF(hash->trx->mysql_thd, FALSE)
		       && wsrep_thd_order_before(hash->trx->mysql_thd,
						 trx->mysql_thd)) {
			prev = hash;
			hash = (lock_t *)hash->hash;
		}
		lock->hash = hash;
		if (prev) {
			prev->hash = lock;
		} else {
			c_lock->hash = lock;
		}
		/*
		 * delayed conflict resolution '...kill_one_trx' was not called,
		 * if victim was waiting for some other lock
		 */
		if (holds_trx_mutex) {
			trx->mutex.wr_unlock();
		}
		mysql_mutex_lock(&lock_sys.wait_mutex);
		if (holds_trx_mutex) {
			trx->mutex.wr_lock();
		}
		trx_t *ctrx = c_lock->trx;
		ctrx->mutex.wr_lock();
		if (ctrx->lock.wait_thr) {
			ctrx->lock.was_chosen_as_deadlock_victim = true;
			ctrx->lock.was_chosen_as_wsrep_victim = true;

			if (UNIV_UNLIKELY(wsrep_debug)) {
				wsrep_print_wait_locks(c_lock);
			}

			lock_set_lock_and_trx_wait(lock, trx, ctrx);
			UT_LIST_ADD_LAST(trx->lock.trx_locks, lock);

			trx->lock.wait_thr = thr;

			/* have to release trx mutex for the duration of
			   victim lock release. This will eventually call
			   lock_grant, which wants to grant trx mutex again
			*/
			if (holds_trx_mutex) {
				trx->mutex.wr_unlock();
			}
			lock_cancel_waiting_and_release(
				ctrx->lock.wait_lock);

			if (holds_trx_mutex) {
				trx->mutex.wr_lock();
			}

			ctrx->mutex.wr_unlock();

			/* have to bail out here to avoid lock_set_lock... */
			mysql_mutex_unlock(&lock_sys.wait_mutex);
			return(lock);
		}
		mysql_mutex_unlock(&lock_sys.wait_mutex);
		ctrx->mutex.wr_unlock();
	} else
#endif /* WITH_WSREP */
		HASH_INSERT(lock_t, hash, lock_hash_get(type_mode),
			    page_id.fold(), lock);

	if (!holds_trx_mutex) {
		trx->mutex.wr_lock();
	}
	if (type_mode & LOCK_WAIT) {
		if (trx->lock.wait_trx) {
			ut_ad(!c_lock || trx->lock.wait_trx == c_lock->trx);
			ut_ad(trx->lock.wait_lock);
			ut_ad((*trx->lock.wait_lock).trx == trx);
		} else {
			ut_ad(c_lock);
			trx->lock.wait_trx = c_lock->trx;
			ut_ad(!trx->lock.wait_lock);
		}
		trx->lock.wait_lock = lock;
	}
	UT_LIST_ADD_LAST(trx->lock.trx_locks, lock);
	if (!holds_trx_mutex) {
		trx->mutex.wr_unlock();
	}
	MONITOR_INC(MONITOR_RECLOCK_CREATED);
	MONITOR_INC(MONITOR_NUM_RECLOCK);

	return lock;
}

/** Enqueue a waiting request for a lock which cannot be granted immediately.
Check for deadlocks.
@param[in]	c_lock		conflicting lock
@param[in]	type_mode	the requested lock mode (LOCK_S or LOCK_X)
				possibly ORed with LOCK_GAP or
				LOCK_REC_NOT_GAP, ORed with
				LOCK_INSERT_INTENTION if this
				waiting lock request is set
				when performing an insert of
				an index record
@param[in]	id		page identifier
@param[in]	page		leaf page in the index
@param[in]	heap_no		record heap number in the block
@param[in]	index		index tree
@param[in,out]	thr		query thread
@param[in]	prdt		minimum bounding box (spatial index)
@retval	DB_LOCK_WAIT		if the waiting lock was enqueued
@retval	DB_DEADLOCK		if this transaction was chosen as the victim */
dberr_t
lock_rec_enqueue_waiting(
	lock_t*			c_lock,
	unsigned		type_mode,
	const page_id_t		id,
	const page_t*		page,
	ulint			heap_no,
	dict_index_t*		index,
	que_thr_t*		thr,
	lock_prdt_t*		prdt)
{
	lock_sys.mutex_assert_locked();
	ut_ad(!srv_read_only_mode);
	ut_ad(dict_index_is_clust(index) || !dict_index_is_online_ddl(index));

	trx_t* trx = thr_get_trx(thr);

	switch (trx_get_dict_operation(trx)) {
	case TRX_DICT_OP_NONE:
		break;
	case TRX_DICT_OP_TABLE:
	case TRX_DICT_OP_INDEX:
		ib::error() << "A record lock wait happens in a dictionary"
			" operation. index "
			<< index->name
			<< " of table "
			<< index->table->name
			<< ". " << BUG_REPORT_MSG;
		ut_ad(0);
	}

	if (trx->mysql_thd && thd_lock_wait_timeout(trx->mysql_thd) == 0) {
		trx->error_state = DB_LOCK_WAIT_TIMEOUT;
		return DB_LOCK_WAIT_TIMEOUT;
	}

	/* Enqueue the lock request that will wait to be granted, note that
	we already own the trx mutex. */
	lock_t* lock = lock_rec_create_low(
		c_lock,
#ifdef WITH_WSREP
		thr,
#endif
		type_mode | LOCK_WAIT, id, page, heap_no, index, trx, true);

	if (prdt && type_mode & LOCK_PREDICATE) {
		lock_prdt_set_prdt(lock, prdt);
	}

	trx->lock.wait_thr = thr;
	trx->lock.was_chosen_as_deadlock_victim = false;

	DBUG_LOG("ib_lock", "trx " << ib::hex(trx->id)
		 << " waits for lock in index " << index->name
		 << " of table " << index->table->name);

	MONITOR_INC(MONITOR_LOCKREC_WAIT);

	return DB_LOCK_WAIT;
}

/*********************************************************************//**
Looks for a suitable type record lock struct by the same trx on the same page.
This can be used to save space when a new record lock should be set on a page:
no new struct is needed, if a suitable old is found.
@return lock or NULL */
static inline
lock_t*
lock_rec_find_similar_on_page(
	ulint           type_mode,      /*!< in: lock type_mode field */
	ulint           heap_no,        /*!< in: heap number of the record */
	lock_t*         lock,           /*!< in: lock_sys.get_first() */
	const trx_t*    trx)            /*!< in: transaction */
{
	lock_sys.mutex_assert_locked();

	for (/* No op */;
	     lock != NULL;
	     lock = lock_rec_get_next_on_page(lock)) {

		if (lock->trx == trx
		    && lock->type_mode == type_mode
		    && lock_rec_get_n_bits(lock) > heap_no) {

			return(lock);
		}
	}

	return(NULL);
}

/*********************************************************************//**
Adds a record lock request in the record queue. The request is normally
added as the last in the queue, but if there are no waiting lock requests
on the record, and the request to be added is not a waiting request, we
can reuse a suitable record lock object already existing on the same page,
just setting the appropriate bit in its bitmap. This is a low-level function
which does NOT check for deadlocks or lock compatibility!
@return lock where the bit was set */
static
void
lock_rec_add_to_queue(
/*==================*/
	unsigned		type_mode,/*!< in: lock mode, wait, gap
					etc. flags */
	const page_id_t		id,	/*!< in: page identifier */
	const page_t*		page,	/*!< in: buffer block containing
					the record */
	ulint			heap_no,/*!< in: heap number of the record */
	dict_index_t*		index,	/*!< in: index of record */
	trx_t*			trx,	/*!< in/out: transaction */
	bool			caller_owns_trx_mutex)
					/*!< in: TRUE if caller owns the
					transaction mutex */
{
	lock_sys.mutex_assert_locked();
	ut_ad(index->is_primary()
	      || dict_index_get_online_status(index) != ONLINE_INDEX_CREATION);
	ut_ad(!(type_mode & LOCK_TABLE));
#ifdef UNIV_DEBUG
	switch (type_mode & LOCK_MODE_MASK) {
	case LOCK_X:
	case LOCK_S:
		break;
	default:
		ut_error;
	}

	if (!(type_mode & (LOCK_WAIT | LOCK_GAP))) {
		lock_mode	mode = (type_mode & LOCK_MODE_MASK) == LOCK_S
			? LOCK_X
			: LOCK_S;
		const lock_t*	other_lock
			= lock_rec_other_has_expl_req(
				mode, id, false, heap_no, trx);
#ifdef WITH_WSREP
		if (UNIV_LIKELY_NULL(other_lock) && trx->is_wsrep()) {
			/* Only BF transaction may be granted lock
			before other conflicting lock request. */
			if (!wsrep_thd_is_BF(trx->mysql_thd, FALSE)
			    && !wsrep_thd_is_BF(other_lock->trx->mysql_thd, FALSE)) {
				/* If it is not BF, this case is a bug. */
				wsrep_report_bf_lock_wait(trx->mysql_thd, trx->id);
				wsrep_report_bf_lock_wait(other_lock->trx->mysql_thd, other_lock->trx->id);
				ut_error;
			}
		} else
#endif /* WITH_WSREP */
		ut_ad(!other_lock);
	}
#endif /* UNIV_DEBUG */

	/* If rec is the supremum record, then we can reset the gap bit, as
	all locks on the supremum are automatically of the gap type, and we
	try to avoid unnecessary memory consumption of a new record lock
	struct for a gap type lock */

	if (heap_no == PAGE_HEAP_NO_SUPREMUM) {
		ut_ad(!(type_mode & LOCK_REC_NOT_GAP));

		/* There should never be LOCK_REC_NOT_GAP on a supremum
		record, but let us play safe */

		type_mode &= ~(LOCK_GAP | LOCK_REC_NOT_GAP);
	}

	if (type_mode & LOCK_WAIT) {
		goto create;
	} else if (lock_t *first_lock =
		   lock_sys.get_first(*lock_hash_get(type_mode), id)) {
		for (lock_t* lock = first_lock;;) {
			if (lock->is_waiting()
			    && lock_rec_get_nth_bit(lock, heap_no)) {
				goto create;
			}
			if (!(lock = lock_rec_get_next_on_page(lock))) {
				break;
			}
		}

		/* Look for a similar record lock on the same page:
		if one is found and there are no waiting lock requests,
		we can just set the bit */
		if (lock_t* lock = lock_rec_find_similar_on_page(
			    type_mode, heap_no, first_lock, trx)) {
			lock_rec_set_nth_bit(lock, heap_no);
			return;
		}
	}

create:
	/* Note: We will not pass any conflicting lock to lock_rec_create(),
	because we should be moving an existing waiting lock request. */
	ut_ad(!(type_mode & LOCK_WAIT) || trx->lock.wait_trx);

	lock_rec_create_low(nullptr,
#ifdef WITH_WSREP
			    nullptr,
#endif
			    type_mode, id, page, heap_no, index, trx,
			    caller_owns_trx_mutex);
}

/*********************************************************************//**
Tries to lock the specified record in the mode requested. If not immediately
possible, enqueues a waiting lock request. This is a low-level function
which does NOT look at implicit locks! Checks lock compatibility within
explicit locks. This function sets a normal next-key lock, or in the case
of a page supremum record, a gap type lock.
@return DB_SUCCESS, DB_SUCCESS_LOCKED_REC, DB_LOCK_WAIT, or DB_DEADLOCK */
static
dberr_t
lock_rec_lock(
/*==========*/
	bool			impl,	/*!< in: if true, no lock is set
					if no wait is necessary: we
					assume that the caller will
					set an implicit lock */
	unsigned		mode,	/*!< in: lock mode: LOCK_X or
					LOCK_S possibly ORed to either
					LOCK_GAP or LOCK_REC_NOT_GAP */
	const buf_block_t*	block,	/*!< in: buffer block containing
					the record */
	ulint			heap_no,/*!< in: heap number of record */
	dict_index_t*		index,	/*!< in: index of record */
	que_thr_t*		thr)	/*!< in: query thread */
{
  trx_t *trx= thr_get_trx(thr);

  ut_ad(!srv_read_only_mode);
  ut_ad(((LOCK_MODE_MASK | LOCK_TABLE) & mode) == LOCK_S ||
        ((LOCK_MODE_MASK | LOCK_TABLE) & mode) == LOCK_X);
  ut_ad(~mode & (LOCK_GAP | LOCK_REC_NOT_GAP));
  ut_ad(dict_index_is_clust(index) || !dict_index_is_online_ddl(index));
  DBUG_EXECUTE_IF("innodb_report_deadlock", return DB_DEADLOCK;);

  ut_ad((LOCK_MODE_MASK & mode) != LOCK_S ||
        lock_table_has(trx, index->table, LOCK_IS));
  ut_ad((LOCK_MODE_MASK & mode) != LOCK_X ||
         lock_table_has(trx, index->table, LOCK_IX));

  if (lock_table_has(trx, index->table,
                     static_cast<lock_mode>(LOCK_MODE_MASK & mode)))
    return DB_SUCCESS;

  MONITOR_ATOMIC_INC(MONITOR_NUM_RECLOCK_REQ);
  const page_id_t id{block->page.id()};
  LockMutexGuard g;

  if (lock_t *lock= lock_sys.get_first(id))
  {
    dberr_t err= DB_SUCCESS;
    trx->mutex.wr_lock();
    if (lock_rec_get_next_on_page(lock) ||
        lock->trx != trx ||
        lock->type_mode != mode ||
        lock_rec_get_n_bits(lock) <= heap_no)
    {
      /* Do nothing if the trx already has a strong enough lock on rec */
      if (!lock_rec_has_expl(mode, id, heap_no, trx))
      {
        if (lock_t *c_lock= lock_rec_other_has_conflicting(mode, id,
                                                           heap_no, trx))
          /*
            If another transaction has a non-gap conflicting
            request in the queue, as this transaction does not
            have a lock strong enough already granted on the
            record, we have to wait.
          */
          err= lock_rec_enqueue_waiting(c_lock, mode, id, block->frame, heap_no,
                                        index, thr, nullptr);
        else if (!impl)
        {
          /* Set the requested lock on the record. */
          lock_rec_add_to_queue(mode, id, block->frame, heap_no, index,
                                trx, true);
          err= DB_SUCCESS_LOCKED_REC;
        }
      }
    }
    else if (!impl)
    {
      /*
        If the nth bit of the record lock is already set then we do not set
        a new lock bit, otherwise we do set
      */
      if (!lock_rec_get_nth_bit(lock, heap_no))
      {
        lock_rec_set_nth_bit(lock, heap_no);
        err= DB_SUCCESS_LOCKED_REC;
      }
    }
    trx->mutex.wr_unlock();
    return err;
  }
  else
  {
    /*
      Simplified and faster path for the most common cases
      Note that we don't own the trx mutex.
    */
    if (!impl)
      lock_rec_create(nullptr,
#ifdef WITH_WSREP
                      nullptr,
#endif
                      mode, block, heap_no, index, trx, false);

    return DB_SUCCESS_LOCKED_REC;
  }
}

/*********************************************************************//**
Checks if a waiting record lock request still has to wait in a queue.
@return lock that is causing the wait */
static
const lock_t*
lock_rec_has_to_wait_in_queue(
/*==========================*/
	const lock_t*	wait_lock)	/*!< in: waiting record lock */
{
	const lock_t*	lock;
	ulint		heap_no;
	ulint		bit_mask;
	ulint		bit_offset;

	ut_ad(wait_lock->is_waiting());
	ut_ad(!wait_lock->is_table());
	lock_sys.mutex_assert_locked();

	heap_no = lock_rec_find_set_bit(wait_lock);

	bit_offset = heap_no / 8;
	bit_mask = static_cast<ulint>(1) << (heap_no % 8);

	for (lock = lock_sys.get_first(*lock_hash_get(wait_lock->type_mode),
				       wait_lock->un_member.rec_lock.page_id);
	     lock != wait_lock;
	     lock = lock_rec_get_next_on_page_const(lock)) {
		const byte*	p = (const byte*) &lock[1];

		if (heap_no < lock_rec_get_n_bits(lock)
		    && (p[bit_offset] & bit_mask)
		    && lock_has_to_wait(wait_lock, lock)) {
			return(lock);
		}
	}

	return(NULL);
}

/** Note that a record lock wait started */
inline void lock_sys_t::wait_start()
{
  mysql_mutex_assert_owner(&wait_mutex);
  wait_pending++;
  wait_count++;
}

/** Note that a record lock wait resumed */
inline
void lock_sys_t::wait_resume(THD *thd, my_hrtime_t start, my_hrtime_t now)
{
  mysql_mutex_assert_owner(&wait_mutex);
  wait_pending--;
  if (now.val >= start.val)
  {
    const ulint diff_time= static_cast<ulint>((now.val - start.val) / 1000);
    wait_time+= diff_time;

    if (diff_time > wait_time_max)
      wait_time_max= diff_time;

    thd_storage_lock_wait(thd, diff_time);
  }
}

#ifdef HAVE_REPLICATION
ATTRIBUTE_NOINLINE MY_ATTRIBUTE((nonnull))
/** Report lock waits to parallel replication.
@param trx       transaction that may be waiting for a lock
@param wait_lock lock that is being waited for */
static void lock_wait_rpl_report(trx_t *trx)
{
  mysql_mutex_assert_owner(&lock_sys.wait_mutex);
  ut_ad(trx->state == TRX_STATE_ACTIVE);
  THD *const thd= trx->mysql_thd;
  ut_ad(thd);
  const lock_t *wait_lock= trx->lock.wait_lock;
  if (!wait_lock)
    return;
  ut_ad(!(wait_lock->type_mode & LOCK_AUTO_INC));
  mysql_mutex_unlock(&lock_sys.wait_mutex);
  LockMutexGuard g;
  mysql_mutex_lock(&lock_sys.wait_mutex);
  wait_lock= trx->lock.wait_lock;
  if (!wait_lock)
    return;
  ut_ad(wait_lock->is_waiting());
  ut_ad(!(wait_lock->type_mode & LOCK_AUTO_INC));

  if (wait_lock->is_table())
  {
    if (lock_t *lock=
        UT_LIST_GET_FIRST(wait_lock->un_member.tab_lock.table->locks))
    {
      do
        if (!(lock->type_mode & LOCK_AUTO_INC) &&
            lock->trx->mysql_thd != thd)
          thd_rpl_deadlock_check(thd, lock->trx->mysql_thd);
      while ((lock= UT_LIST_GET_NEXT(un_member.tab_lock.locks, lock)));
    }
  }
  else if (lock_t *lock=
           lock_sys.get_first(wait_lock->type_mode & LOCK_PREDICATE
                              ? lock_sys.prdt_hash : lock_sys.rec_hash,
                              wait_lock->un_member.rec_lock.page_id))
  {
    const ulint heap_no= lock_rec_find_set_bit(wait_lock);
    if (!lock_rec_get_nth_bit(lock, heap_no))
      lock= lock_rec_get_next(heap_no, lock);
    do
      if (lock->trx->mysql_thd != thd)
        thd_rpl_deadlock_check(thd, lock->trx->mysql_thd);
    while ((lock= lock_rec_get_next(heap_no, lock)));
  }
}
#endif /* HAVE_REPLICATION */

/** Wait for a lock to be released.
@retval DB_DEADLOCK if this transaction was chosen as the deadlock victim
@retval DB_INTERRUPTED if the execution was interrupted by the user
@retval DB_LOCK_WAIT_TIMEOUT if the lock wait timed out
@retval DB_SUCCESS if the lock was granted */
dberr_t lock_wait(que_thr_t *thr)
{
  trx_t *trx= thr_get_trx(thr);

  if (trx->mysql_thd)
    DEBUG_SYNC_C("lock_wait_suspend_thread_enter");

  /* InnoDB system transactions may use the global value of
  innodb_lock_wait_timeout, because trx->mysql_thd == NULL. */
  const ulong innodb_lock_wait_timeout= trx_lock_wait_timeout_get(trx);
  const bool no_timeout= innodb_lock_wait_timeout > 100000000;
  const my_hrtime_t suspend_time= my_hrtime_coarse();
  ut_ad(!trx->dict_operation_lock_mode ||
        trx->dict_operation_lock_mode == RW_S_LATCH);
  const bool row_lock_wait= thr->lock_state == QUE_THR_LOCK_ROW;
  bool had_dict_lock= trx->dict_operation_lock_mode != 0;

  mysql_mutex_lock(&lock_sys.wait_mutex);
  trx->mutex.wr_lock();
  trx->error_state= DB_SUCCESS;

  if (!trx->lock.wait_lock)
  {
    /* The lock has already been released or this transaction
    was chosen as a deadlock victim: no need to suspend */

    if (trx->lock.was_chosen_as_deadlock_victim
        IF_WSREP(|| trx->lock.was_chosen_as_wsrep_victim,))
    {
      trx->error_state= DB_DEADLOCK;
      trx->lock.was_chosen_as_deadlock_victim= false;
    }

    mysql_mutex_unlock(&lock_sys.wait_mutex);
    trx->mutex.wr_unlock();
    return trx->error_state;
  }

  trx->lock.suspend_time= suspend_time;
  trx->mutex.wr_unlock();

  if (row_lock_wait)
    lock_sys.wait_start();

  int err= 0;
  int wait_for= 0;

  /* The wait_lock can be cleared by another thread in lock_grant(),
  lock_rec_cancel(), or lock_cancel_waiting_and_release(). But, a wait
  can only be initiated by the current thread which owns the transaction. */
  if (const lock_t *wait_lock= trx->lock.wait_lock)
  {
    const auto type_mode= wait_lock->type_mode;
    mysql_mutex_unlock(&lock_sys.wait_mutex);

    if (had_dict_lock) /* Release foreign key check latch */
      row_mysql_unfreeze_data_dictionary(trx);

    static_assert(THD_WAIT_TABLE_LOCK != 0, "compatibility");
    static_assert(THD_WAIT_ROW_LOCK != 0, "compatibility");
    wait_for= type_mode & LOCK_TABLE ? THD_WAIT_TABLE_LOCK : THD_WAIT_ROW_LOCK;

#ifdef HAVE_REPLICATION
    /* Even though lock_wait_rpl_report() has nothing to do with
    deadlock detection, it was always disabled by innodb_deadlock_detect=OFF.
    We will keep it in that way, because unfortunately
    thd_need_wait_reports() will hold even if parallel (or any) replication
    is not being used. We want to be allow the user to skip
    lock_wait_rpl_report(). */
    const bool rpl= !(type_mode & LOCK_AUTO_INC) && trx->mysql_thd &&
      innodb_deadlock_detect && thd_need_wait_reports(trx->mysql_thd);
#endif

    timespec abstime;
    set_timespec_time_nsec(abstime, suspend_time.val * 1000);
    abstime.MY_tv_sec+= innodb_lock_wait_timeout;
    thd_wait_begin(trx->mysql_thd, wait_for);

    if (Deadlock::check_and_resolve(trx))
    {
      trx->error_state= DB_DEADLOCK;
      goto end_wait;
    }

#ifdef HAVE_REPLICATION
    if (rpl)
      lock_wait_rpl_report(trx);
#endif

    while (trx->lock.wait_lock)
    {
      if (no_timeout)
        mysql_cond_wait(&trx->lock.cond, &lock_sys.wait_mutex);
      else
        err= mysql_cond_timedwait(&trx->lock.cond, &lock_sys.wait_mutex,
                                  &abstime);
      switch (trx->error_state) {
      default:
        if (trx_is_interrupted(trx))
          /* innobase_kill_query() can only set trx->error_state=DB_INTERRUPTED
          for any transaction that is attached to a connection. */
          trx->error_state= DB_INTERRUPTED;
        else if (!err)
          continue;
        else
          break;
        /* fall through */
      case DB_DEADLOCK:
      case DB_INTERRUPTED:
        err= 0;
      }
      break;
    }
  }
  else
    had_dict_lock= false;

end_wait:
  if (row_lock_wait)
    lock_sys.wait_resume(trx->mysql_thd, suspend_time, my_hrtime_coarse());

  mysql_mutex_unlock(&lock_sys.wait_mutex);

  if (wait_for)
    thd_wait_end(trx->mysql_thd);

  if (had_dict_lock)
    row_mysql_freeze_data_dictionary(trx);

  if (!err);
#ifdef WITH_WSREP
  else if (trx->is_wsrep() && wsrep_is_BF_lock_timeout(trx, false));
#endif
  else
  {
    trx->error_state= DB_LOCK_WAIT_TIMEOUT;
    MONITOR_INC(MONITOR_TIMEOUT);
  }

  if (trx->lock.wait_lock)
  {
    {
      LockMutexGuard g;
      mysql_mutex_lock(&lock_sys.wait_mutex);
      if (lock_t *lock= trx->lock.wait_lock)
      {
        trx->mutex.wr_lock();
        lock_cancel_waiting_and_release(lock);
        trx->mutex.wr_unlock();
      }
    }
    mysql_mutex_unlock(&lock_sys.wait_mutex);
  }

  return trx->error_state;
}

/** Resume a lock wait */
static void lock_wait_end(trx_t *trx)
{
  mysql_mutex_assert_owner(&lock_sys.wait_mutex);
  ut_ad(trx->state == TRX_STATE_ACTIVE);

  que_thr_t *thr= trx->lock.wait_thr;
  ut_ad(thr);
  if (trx->lock.was_chosen_as_deadlock_victim)
  {
    trx->error_state= DB_DEADLOCK;
    trx->lock.was_chosen_as_deadlock_victim= false;
  }
  trx->lock.wait_thr= nullptr;
  mysql_cond_signal(&trx->lock.cond);
}

/** Grant a waiting lock request and release the waiting transaction. */
static void lock_grant(lock_t *lock)
{
  lock_reset_lock_and_trx_wait(lock);
  trx_t *trx= lock->trx;
  trx->mutex.wr_lock();
  if (lock->mode() == LOCK_AUTO_INC)
  {
    dict_table_t *table= lock->un_member.tab_lock.table;
    ut_ad(!table->autoinc_trx);
    table->autoinc_trx= trx;
    ib_vector_push(trx->autoinc_locks, &lock);
  }

  DBUG_PRINT("ib_lock", ("wait for trx " TRX_ID_FMT " ends", trx->id));

  /* If we are resolving a deadlock by choosing another transaction as
  a victim, then our original transaction may not be waiting anymore */

  if (trx->lock.wait_thr)
    lock_wait_end(trx);

  trx->mutex.wr_unlock();
}

/*************************************************************//**
Cancels a waiting record lock request and releases the waiting transaction
that requested it. NOTE: does NOT check if waiting lock requests behind this
one can now be granted! */
static
void
lock_rec_cancel(
/*============*/
	lock_t*	lock)	/*!< in: waiting record lock request */
{
	ut_ad(!lock->is_table());
	lock_sys.mutex_assert_locked();

	/* Reset the bit (there can be only one set bit) in the lock bitmap */
	lock_rec_reset_nth_bit(lock, lock_rec_find_set_bit(lock));

	/* Reset the wait flag and the back pointer to lock in trx */

	mysql_mutex_lock(&lock_sys.wait_mutex);
	lock_reset_lock_and_trx_wait(lock);

	/* The following releases the trx from lock wait */
	trx_t *trx = lock->trx;
	trx->mutex.wr_lock();
	lock_wait_end(trx);
	mysql_mutex_unlock(&lock_sys.wait_mutex);
	trx->mutex.wr_unlock();
}

/** Remove a record lock request, waiting or granted, from the queue and
grant locks to other transactions in the queue if they now are entitled
to a lock. NOTE: all record locks contained in in_lock are removed.
@param[in,out]	in_lock		record lock */
static void lock_rec_dequeue_from_page(lock_t* in_lock)
{
	mysql_mutex_assert_owner(&lock_sys.wait_mutex);
	ut_ad(!in_lock->is_table());
	/* We may or may not be holding in_lock->trx->mutex here. */

	const page_id_t page_id{in_lock->un_member.rec_lock.page_id};
	lock_sys.mutex_assert_locked();

	in_lock->index->table->n_rec_locks--;

	hash_table_t* lock_hash = lock_hash_get(in_lock->type_mode);
	const ulint rec_fold = page_id.fold();

	HASH_DELETE(lock_t, hash, lock_hash, rec_fold, in_lock);
	UT_LIST_REMOVE(in_lock->trx->lock.trx_locks, in_lock);

	MONITOR_INC(MONITOR_RECLOCK_REMOVED);
	MONITOR_DEC(MONITOR_NUM_RECLOCK);

	/* Check if waiting locks in the queue can now be granted:
	grant locks if there are no conflicting locks ahead. Stop at
	the first X lock that is waiting or has been granted. */

	for (lock_t* lock = lock_sys.get_first(*lock_hash, page_id);
	     lock != NULL;
	     lock = lock_rec_get_next_on_page(lock)) {

		if (!lock->is_waiting()) {
			continue;
		}

		ut_ad(lock->trx->lock.wait_trx);
		ut_ad(lock->trx->lock.wait_lock);

		if (const lock_t* c = lock_rec_has_to_wait_in_queue(lock)) {
#ifdef WITH_WSREP
			wsrep_assert_no_bf_bf_wait(c, lock, c->trx);
#endif /* WITH_WSREP */
			lock->trx->lock.wait_trx = c->trx;
		} else {
			/* Grant the lock */
			ut_ad(lock->trx != in_lock->trx);
			lock_grant(lock);
		}
	}
}

/*************************************************************//**
Removes a record lock request, waiting or granted, from the queue. */
void
lock_rec_discard(
/*=============*/
	lock_t*		in_lock)	/*!< in: record lock object: all
					record locks which are contained
					in this lock object are removed */
{
	trx_lock_t*	trx_lock;

	ut_ad(!in_lock->is_table());
	lock_sys.mutex_assert_locked();

	trx_lock = &in_lock->trx->lock;

	in_lock->index->table->n_rec_locks--;

	HASH_DELETE(lock_t, hash, lock_hash_get(in_lock->type_mode),
		    in_lock->un_member.rec_lock.page_id.fold(), in_lock);

	UT_LIST_REMOVE(trx_lock->trx_locks, in_lock);

	MONITOR_INC(MONITOR_RECLOCK_REMOVED);
	MONITOR_DEC(MONITOR_NUM_RECLOCK);
}

/*************************************************************//**
Removes record lock objects set on an index page which is discarded. This
function does not move locks, or check for waiting locks, therefore the
lock bitmaps must already be reset when this function is called. */
static void lock_rec_free_all_from_discard_page_low(const page_id_t id,
                                                    hash_table_t *lock_hash)
{
  lock_t *lock= lock_sys.get_first(*lock_hash, id);

  while (lock)
  {
    ut_ad(lock_rec_find_set_bit(lock) == ULINT_UNDEFINED);
    ut_ad(!lock->is_waiting());
    lock_t *next_lock= lock_rec_get_next_on_page(lock);
    lock_rec_discard(lock);
    lock= next_lock;
  }
}

/** Remove record locks for an index page which is discarded. This
function does not move locks, or check for waiting locks, therefore the
lock bitmaps must already be reset when this function is called. */
void lock_rec_free_all_from_discard_page(const page_id_t page_id)
{
  lock_rec_free_all_from_discard_page_low(page_id, &lock_sys.rec_hash);
  lock_rec_free_all_from_discard_page_low(page_id, &lock_sys.prdt_hash);
  lock_rec_free_all_from_discard_page_low(page_id, &lock_sys.prdt_page_hash);
}

/*============= RECORD LOCK MOVING AND INHERITING ===================*/

/*************************************************************//**
Resets the lock bits for a single record. Releases transactions waiting for
lock requests here. */
static
void
lock_rec_reset_and_release_wait_low(
/*================================*/
	hash_table_t*		hash,	/*!< in: hash table */
	const page_id_t		id,	/*!< in: page identifier */
	ulint			heap_no)/*!< in: heap number of record */
{
  for (lock_t *lock= lock_rec_get_first(hash, id, heap_no); lock;
       lock= lock_rec_get_next(heap_no, lock))
    if (lock->is_waiting())
      lock_rec_cancel(lock);
    else
      lock_rec_reset_nth_bit(lock, heap_no);
}

/*************************************************************//**
Resets the lock bits for a single record. Releases transactions waiting for
lock requests here. */
static
void
lock_rec_reset_and_release_wait(
/*============================*/
	const page_id_t		id,	/*!< in: page identifier */
	ulint			heap_no)/*!< in: heap number of record */
{
	lock_rec_reset_and_release_wait_low(
		&lock_sys.rec_hash, id, heap_no);

	lock_rec_reset_and_release_wait_low(
		&lock_sys.prdt_hash, id, PAGE_HEAP_NO_INFIMUM);
	lock_rec_reset_and_release_wait_low(
		&lock_sys.prdt_page_hash, id, PAGE_HEAP_NO_INFIMUM);
}

/*************************************************************//**
Makes a record to inherit the locks (except LOCK_INSERT_INTENTION type)
of another record as gap type locks, but does not reset the lock bits of
the other record. Also waiting lock requests on rec are inherited as
GRANTED gap locks. */
static
void
lock_rec_inherit_to_gap(
/*====================*/
	const buf_block_t&	heir_block,	/*!< in: block containing the
						record which inherits */
	const page_id_t		id,		/*!< in: page containing the
						record from which inherited;
						does NOT reset the locks on
						this record */
	ulint			heir_heap_no,	/*!< in: heap_no of the
						inheriting record */
	ulint			heap_no)	/*!< in: heap_no of the
						donating record */
{
	const page_id_t heir_id{heir_block.page.id()};
	lock_sys.mutex_assert_locked();

	/* At READ UNCOMMITTED or READ COMMITTED isolation level,
	we do not want locks set
	by an UPDATE or a DELETE to be inherited as gap type locks. But we
	DO want S-locks/X-locks(taken for replace) set by a consistency
	constraint to be inherited also then. */

	for (lock_t* lock= lock_rec_get_first(&lock_sys.rec_hash, id, heap_no);
	     lock != NULL;
	     lock = lock_rec_get_next(heap_no, lock)) {

		if (!lock->is_insert_intention()
		    && (lock->trx->isolation_level > TRX_ISO_READ_COMMITTED
			|| lock->mode() !=
			(lock->trx->duplicates ? LOCK_S : LOCK_X))) {
			lock_rec_add_to_queue(LOCK_GAP | lock->mode(),
					      heir_id, heir_block.frame,
					      heir_heap_no,
					      lock->index, lock->trx, false);
		}
	}
}

/*************************************************************//**
Makes a record to inherit the gap locks (except LOCK_INSERT_INTENTION type)
of another record as gap type locks, but does not reset the lock bits of the
other record. Also waiting lock requests are inherited as GRANTED gap locks. */
static
void
lock_rec_inherit_to_gap_if_gap_lock(
/*================================*/
	const buf_block_t*	block,		/*!< in: buffer block */
	ulint			heir_heap_no,	/*!< in: heap_no of
						record which inherits */
	ulint			heap_no)	/*!< in: heap_no of record
						from which inherited;
						does NOT reset the locks
						on this record */
{
  const page_id_t id{block->page.id()};
  LockMutexGuard g;

  for (lock_t *lock= lock_rec_get_first(&lock_sys.rec_hash, id, heap_no);
       lock; lock= lock_rec_get_next(heap_no, lock))
     if (!lock->is_insert_intention() && (heap_no == PAGE_HEAP_NO_SUPREMUM ||
                                          !lock->is_record_not_gap()) &&
         !lock_table_has(lock->trx, lock->index->table, LOCK_X))
       lock_rec_add_to_queue(LOCK_GAP | lock->mode(), id, block->frame,
                             heir_heap_no, lock->index, lock->trx, false);
}

/*************************************************************//**
Moves the locks of a record to another record and resets the lock bits of
the donating record. */
static
void
lock_rec_move_low(
/*==============*/
	hash_table_t*		lock_hash,	/*!< in: hash table to use */
	const buf_block_t&	receiver,	/*!< in: buffer block containing
						the receiving record */
	const page_id_t		donator_id,	/*!< in: page identifier of
						the donating record */
	ulint			receiver_heap_no,/*!< in: heap_no of the record
						which gets the locks; there
						must be no lock requests
						on it! */
	ulint			donator_heap_no)/*!< in: heap_no of the record
						which gives the locks */
{
	const page_id_t receiver_id{receiver.page.id()};

	lock_sys.mutex_assert_locked();

	/* If the lock is predicate lock, it resides on INFIMUM record */
	ut_ad(!lock_rec_get_first(lock_hash, receiver_id, receiver_heap_no)
	      || lock_hash == &lock_sys.prdt_hash
	      || lock_hash == &lock_sys.prdt_page_hash);

	for (lock_t *lock =
	     lock_rec_get_first(lock_hash, donator_id, donator_heap_no);
	     lock != NULL;
	     lock = lock_rec_get_next(donator_heap_no, lock)) {

		lock_rec_reset_nth_bit(lock, donator_heap_no);

		const auto type_mode = lock->type_mode;
		if (type_mode & LOCK_WAIT) {
			ut_ad(lock->trx->lock.wait_lock == lock);
			lock->type_mode &= ~LOCK_WAIT;
		}

		/* Note that we FIRST reset the bit, and then set the lock:
		the function works also if donator_id == receiver_id */

		lock_rec_add_to_queue(type_mode, receiver_id, receiver.frame,
				      receiver_heap_no,
				      lock->index, lock->trx, false);
	}

	ut_ad(!lock_rec_get_first(&lock_sys.rec_hash,
				  donator_id, donator_heap_no));
}

/** Move all the granted locks to the front of the given lock list.
All the waiting locks will be at the end of the list.
@param[in,out]	lock_list	the given lock list.  */
static
void
lock_move_granted_locks_to_front(
	UT_LIST_BASE_NODE_T(lock_t)&	lock_list)
{
	lock_t*	lock;

	bool seen_waiting_lock = false;

	for (lock = UT_LIST_GET_FIRST(lock_list); lock;
	     lock = UT_LIST_GET_NEXT(trx_locks, lock)) {

		if (!seen_waiting_lock) {
			if (lock->is_waiting()) {
				seen_waiting_lock = true;
			}
			continue;
		}

		ut_ad(seen_waiting_lock);

		if (!lock->is_waiting()) {
			lock_t* prev = UT_LIST_GET_PREV(trx_locks, lock);
			ut_a(prev);
			ut_list_move_to_front(lock_list, lock);
			lock = prev;
		}
	}
}

/*************************************************************//**
Moves the locks of a record to another record and resets the lock bits of
the donating record. */
UNIV_INLINE
void
lock_rec_move(
/*==========*/
	const buf_block_t&	receiver,       /*!< in: buffer block containing
						the receiving record */
	const page_id_t		donator_id,	/*!< in: page identifier of
						the donating record */
	ulint			receiver_heap_no,/*!< in: heap_no of the record
						which gets the locks; there
						must be no lock requests
						on it! */
	ulint			donator_heap_no)/*!< in: heap_no of the record
                                                which gives the locks */
{
  lock_rec_move_low(&lock_sys.rec_hash, receiver, donator_id,
                    receiver_heap_no, donator_heap_no);
}

/*************************************************************//**
Updates the lock table when we have reorganized a page. NOTE: we copy
also the locks set on the infimum of the page; the infimum may carry
locks if an update of a record is occurring on the page, and its locks
were temporarily stored on the infimum. */
void
lock_move_reorganize_page(
/*======================*/
	const buf_block_t*	block,	/*!< in: old index page, now
					reorganized */
	const buf_block_t*	oblock)	/*!< in: copy of the old, not
					reorganized page */
{
	lock_t*		lock;
	UT_LIST_BASE_NODE_T(lock_t)	old_locks;
	mem_heap_t*	heap		= NULL;
	ulint		comp;
	const page_id_t id{block->page.id()};

	lock_sys.mutex_lock();

	/* FIXME: This needs to deal with predicate lock too */
	lock = lock_sys.get_first(id);

	if (lock == NULL) {
		lock_sys.mutex_unlock();

		return;
	}

	heap = mem_heap_create(256);

	/* Copy first all the locks on the page to heap and reset the
	bitmaps in the original locks; chain the copies of the locks
	using the trx_locks field in them. */

	UT_LIST_INIT(old_locks, &lock_t::trx_locks);

	do {
		/* Make a copy of the lock */
		lock_t*	old_lock = lock_rec_copy(lock, heap);

		UT_LIST_ADD_LAST(old_locks, old_lock);

		/* Reset bitmap of lock */
		lock_rec_bitmap_reset(lock);

		if (lock->is_waiting()) {
			ut_ad(lock->trx->lock.wait_lock == lock);
			lock->type_mode &= ~LOCK_WAIT;
		}

		lock = lock_rec_get_next_on_page(lock);
	} while (lock != NULL);

	comp = page_is_comp(block->frame);
	ut_ad(comp == page_is_comp(oblock->frame));

	lock_move_granted_locks_to_front(old_locks);

	DBUG_EXECUTE_IF("do_lock_reverse_page_reorganize",
			ut_list_reverse(old_locks););

	for (lock = UT_LIST_GET_FIRST(old_locks); lock;
	     lock = UT_LIST_GET_NEXT(trx_locks, lock)) {

		/* NOTE: we copy also the locks set on the infimum and
		supremum of the page; the infimum may carry locks if an
		update of a record is occurring on the page, and its locks
		were temporarily stored on the infimum */
		const rec_t*	rec1 = page_get_infimum_rec(
			buf_block_get_frame(block));
		const rec_t*	rec2 = page_get_infimum_rec(
			buf_block_get_frame(oblock));

		/* Set locks according to old locks */
		for (;;) {
			ulint	old_heap_no;
			ulint	new_heap_no;
			ut_d(const rec_t* const orec = rec1);
			ut_ad(page_rec_is_metadata(rec1)
			      == page_rec_is_metadata(rec2));

			if (comp) {
				old_heap_no = rec_get_heap_no_new(rec2);
				new_heap_no = rec_get_heap_no_new(rec1);

				rec1 = page_rec_get_next_low(rec1, TRUE);
				rec2 = page_rec_get_next_low(rec2, TRUE);
			} else {
				old_heap_no = rec_get_heap_no_old(rec2);
				new_heap_no = rec_get_heap_no_old(rec1);
				ut_ad(!memcmp(rec1, rec2,
					      rec_get_data_size_old(rec2)));

				rec1 = page_rec_get_next_low(rec1, FALSE);
				rec2 = page_rec_get_next_low(rec2, FALSE);
			}

			/* Clear the bit in old_lock. */
			if (old_heap_no < lock->un_member.rec_lock.n_bits
			    && lock_rec_reset_nth_bit(lock, old_heap_no)) {
				ut_ad(!page_rec_is_metadata(orec));

				/* NOTE that the old lock bitmap could be too
				small for the new heap number! */

				lock_rec_add_to_queue(
					lock->type_mode, id, block->frame,
					new_heap_no,
					lock->index, lock->trx, FALSE);
			}

			if (new_heap_no == PAGE_HEAP_NO_SUPREMUM) {
				ut_ad(old_heap_no == PAGE_HEAP_NO_SUPREMUM);
				break;
			}
		}

		ut_ad(lock_rec_find_set_bit(lock) == ULINT_UNDEFINED);
	}

	lock_sys.mutex_unlock();

	mem_heap_free(heap);

#ifdef UNIV_DEBUG_LOCK_VALIDATE
	if (fil_space_t* space = fil_space_t::get(page_id.space())) {
		ut_ad(lock_rec_validate_page(block, space->is_latched()));
		space->release();
	}
#endif
}

/*************************************************************//**
Moves the explicit locks on user records to another page if a record
list end is moved to another page. */
void
lock_move_rec_list_end(
/*===================*/
	const buf_block_t*	new_block,	/*!< in: index page to move to */
	const buf_block_t*	block,		/*!< in: index page */
	const rec_t*		rec)		/*!< in: record on page: this
						is the first record moved */
{
  const ulint comp= page_rec_is_comp(rec);

  ut_ad(block->frame == page_align(rec));
  ut_ad(comp == page_is_comp(new_block->frame));

  const page_id_t id{block->page.id()};
  const page_id_t new_id{new_block->page.id()};
  {
    LockMutexGuard g;

    /* Note: when we move locks from record to record, waiting locks
    and possible granted gap type locks behind them are enqueued in
    the original order, because new elements are inserted to a hash
    table to the end of the hash chain, and lock_rec_add_to_queue
    does not reuse locks if there are waiters in the queue. */
    for (lock_t *lock= lock_sys.get_first(id); lock;
         lock= lock_rec_get_next_on_page(lock))
    {
      const rec_t *rec1= rec;
      const rec_t *rec2;
      const auto type_mode= lock->type_mode;

      if (comp)
      {
        if (page_offset(rec1) == PAGE_NEW_INFIMUM)
          rec1= page_rec_get_next_low(rec1, TRUE);
        rec2= page_rec_get_next_low(new_block->frame + PAGE_NEW_INFIMUM, TRUE);
      }
      else
      {
        if (page_offset(rec1) == PAGE_OLD_INFIMUM)
          rec1= page_rec_get_next_low(rec1, FALSE);
        rec2= page_rec_get_next_low(new_block->frame + PAGE_OLD_INFIMUM,FALSE);
      }

      /* Copy lock requests on user records to new page and
      reset the lock bits on the old */
      for (;;)
      {
        ut_ad(page_rec_is_metadata(rec1) == page_rec_is_metadata(rec2));
        ut_d(const rec_t* const orec= rec1);

        ulint rec1_heap_no;
        ulint rec2_heap_no;

        if (comp)
        {
          rec1_heap_no= rec_get_heap_no_new(rec1);
          if (rec1_heap_no == PAGE_HEAP_NO_SUPREMUM)
            break;

          rec2_heap_no= rec_get_heap_no_new(rec2);
          rec1= page_rec_get_next_low(rec1, TRUE);
          rec2= page_rec_get_next_low(rec2, TRUE);
        }
        else
        {
          rec1_heap_no= rec_get_heap_no_old(rec1);

          if (rec1_heap_no == PAGE_HEAP_NO_SUPREMUM)
            break;
          rec2_heap_no= rec_get_heap_no_old(rec2);

          ut_ad(rec_get_data_size_old(rec1) == rec_get_data_size_old(rec2));
          ut_ad(!memcmp(rec1, rec2, rec_get_data_size_old(rec1)));

          rec1= page_rec_get_next_low(rec1, FALSE);
          rec2= page_rec_get_next_low(rec2, FALSE);
        }

        if (rec1_heap_no < lock->un_member.rec_lock.n_bits &&
            lock_rec_reset_nth_bit(lock, rec1_heap_no))
        {
          ut_ad(!page_rec_is_metadata(orec));

          if (type_mode & LOCK_WAIT)
          {
            ut_ad(lock->trx->lock.wait_lock == lock);
            lock->type_mode&= ~LOCK_WAIT;
          }

          lock_rec_add_to_queue(type_mode, new_id, new_block->frame,
                                rec2_heap_no, lock->index, lock->trx, false);
        }
      }
    }
  }

#ifdef UNIV_DEBUG_LOCK_VALIDATE
  if (fil_space_t *space= fil_space_t::get(id.space()))
  {
    const bool is_latched{space->is_latched()};
    ut_ad(lock_rec_validate_page(block, is_latched));
    ut_ad(lock_rec_validate_page(new_block, is_latched));
    space->release();
  }
#endif
}

/*************************************************************//**
Moves the explicit locks on user records to another page if a record
list start is moved to another page. */
void
lock_move_rec_list_start(
/*=====================*/
	const buf_block_t*	new_block,	/*!< in: index page to
						move to */
	const buf_block_t*	block,		/*!< in: index page */
	const rec_t*		rec,		/*!< in: record on page:
						this is the first
						record NOT copied */
	const rec_t*		old_end)	/*!< in: old
						previous-to-last
						record on new_page
						before the records
						were copied */
{
  const ulint comp= page_rec_is_comp(rec);

  ut_ad(block->frame == page_align(rec));
  ut_ad(comp == page_is_comp(new_block->frame));
  ut_ad(new_block->frame == page_align(old_end));
  ut_ad(!page_rec_is_metadata(rec));
  const page_id_t id{block->page.id()};
  const page_id_t new_id{new_block->page.id()};

  {
    LockMutexGuard g;

    for (lock_t *lock= lock_sys.get_first(id); lock;
         lock= lock_rec_get_next_on_page(lock))
    {
      const rec_t *rec1;
      const rec_t *rec2;
      const auto type_mode= lock->type_mode;

      if (comp)
      {
        rec1= page_rec_get_next_low(block->frame + PAGE_NEW_INFIMUM, TRUE);
        rec2= page_rec_get_next_low(old_end, TRUE);
      }
      else
      {
        rec1= page_rec_get_next_low(block->frame + PAGE_OLD_INFIMUM, FALSE);
        rec2= page_rec_get_next_low(old_end, FALSE);
      }

      /* Copy lock requests on user records to new page and
      reset the lock bits on the old */

      while (rec1 != rec)
      {
        ut_ad(page_rec_is_metadata(rec1) == page_rec_is_metadata(rec2));
        ut_d(const rec_t* const prev= rec1);

        ulint rec1_heap_no;
        ulint rec2_heap_no;

        if (comp)
        {
          rec1_heap_no= rec_get_heap_no_new(rec1);
          rec2_heap_no= rec_get_heap_no_new(rec2);

          rec1= page_rec_get_next_low(rec1, TRUE);
          rec2= page_rec_get_next_low(rec2, TRUE);
        }
        else
        {
          rec1_heap_no= rec_get_heap_no_old(rec1);
          rec2_heap_no= rec_get_heap_no_old(rec2);

          ut_ad(!memcmp(rec1, rec2, rec_get_data_size_old(rec2)));

          rec1= page_rec_get_next_low(rec1, FALSE);
          rec2= page_rec_get_next_low(rec2, FALSE);
        }

        if (rec1_heap_no < lock->un_member.rec_lock.n_bits &&
            lock_rec_reset_nth_bit(lock, rec1_heap_no))
        {
          ut_ad(!page_rec_is_metadata(prev));

          if (type_mode & LOCK_WAIT)
          {
            ut_ad(lock->trx->lock.wait_lock == lock);
            lock->type_mode&= ~LOCK_WAIT;
          }

          lock_rec_add_to_queue(type_mode, new_id, new_block->frame,
                                rec2_heap_no, lock->index, lock->trx, false);
        }
      }

#ifdef UNIV_DEBUG
      if (page_rec_is_supremum(rec))
        for (auto i= lock_rec_get_n_bits(lock); --i > PAGE_HEAP_NO_USER_LOW; )
          ut_ad(!lock_rec_get_nth_bit(lock, i));
#endif /* UNIV_DEBUG */
    }
  }

#ifdef UNIV_DEBUG_LOCK_VALIDATE
  ut_ad(lock_rec_validate_page(block));
#endif
}

/*************************************************************//**
Moves the explicit locks on user records to another page if a record
list start is moved to another page. */
void
lock_rtr_move_rec_list(
/*===================*/
	const buf_block_t*	new_block,	/*!< in: index page to
						move to */
	const buf_block_t*	block,		/*!< in: index page */
	rtr_rec_move_t*		rec_move,       /*!< in: recording records
						moved */
	ulint			num_move)       /*!< in: num of rec to move */
{
  if (!num_move)
    return;

  const ulint comp= page_rec_is_comp(rec_move[0].old_rec);

  ut_ad(block->frame == page_align(rec_move[0].old_rec));
  ut_ad(new_block->frame == page_align(rec_move[0].new_rec));
  ut_ad(comp == page_rec_is_comp(rec_move[0].new_rec));
  const page_id_t id{block->page.id()};
  const page_id_t new_id{new_block->page.id()};

  {
    LockMutexGuard g;

    for (lock_t *lock= lock_sys.get_first(id); lock;
	 lock= lock_rec_get_next_on_page(lock))
    {
      const rec_t *rec1;
      const rec_t *rec2;
      const auto type_mode= lock->type_mode;

      /* Copy lock requests on user records to new page and
      reset the lock bits on the old */

      for (ulint moved= 0; moved < num_move; moved++)
      {
        ulint rec1_heap_no;
	ulint rec2_heap_no;

	rec1= rec_move[moved].old_rec;
	rec2= rec_move[moved].new_rec;
	ut_ad(!page_rec_is_metadata(rec1));
	ut_ad(!page_rec_is_metadata(rec2));

	if (comp)
        {
          rec1_heap_no= rec_get_heap_no_new(rec1);
	  rec2_heap_no= rec_get_heap_no_new(rec2);
	}
	else
        {
          rec1_heap_no= rec_get_heap_no_old(rec1);
	  rec2_heap_no= rec_get_heap_no_old(rec2);

	  ut_ad(!memcmp(rec1, rec2, rec_get_data_size_old(rec2)));
	}

	if (rec1_heap_no < lock->un_member.rec_lock.n_bits &&
	    lock_rec_reset_nth_bit(lock, rec1_heap_no))
        {
          if (type_mode & LOCK_WAIT)
          {
            ut_ad(lock->trx->lock.wait_lock == lock);
            lock->type_mode&= ~LOCK_WAIT;
          }

          lock_rec_add_to_queue(type_mode, new_id, new_block->frame,
                                rec2_heap_no, lock->index, lock->trx, false);

	  rec_move[moved].moved= true;
	}
      }
    }
  }

#ifdef UNIV_DEBUG_LOCK_VALIDATE
  ut_ad(lock_rec_validate_page(block));
#endif
}
/*************************************************************//**
Updates the lock table when a page is split to the right. */
void
lock_update_split_right(
/*====================*/
	const buf_block_t*	right_block,	/*!< in: right page */
	const buf_block_t*	left_block)	/*!< in: left page */
{
  const ulint heap_no= lock_get_min_heap_no(right_block);
  const page_id_t l{left_block->page.id()};
  const page_id_t r{right_block->page.id()};

  LockMutexGuard g;

  /* Move the locks on the supremum of the left page to the supremum
  of the right page */

  lock_rec_move(*right_block, l, PAGE_HEAP_NO_SUPREMUM, PAGE_HEAP_NO_SUPREMUM);

  /* Inherit the locks to the supremum of left page from the successor
  of the infimum on right page */
  lock_rec_inherit_to_gap(*left_block, r, PAGE_HEAP_NO_SUPREMUM, heap_no);
}

/*************************************************************//**
Updates the lock table when a page is merged to the right. */
void
lock_update_merge_right(
/*====================*/
	const buf_block_t*	right_block,	/*!< in: right page to
						which merged */
	const rec_t*		orig_succ,	/*!< in: original
						successor of infimum
						on the right page
						before merge */
	const buf_block_t*	left_block)	/*!< in: merged index
						page which will be
						discarded */
{
	ut_ad(!page_rec_is_metadata(orig_succ));

	const page_id_t l{left_block->page.id()};
	LockMutexGuard g;

	/* Inherit the locks from the supremum of the left page to the
	original successor of infimum on the right page, to which the left
	page was merged */

	lock_rec_inherit_to_gap(*right_block, l,
				page_rec_get_heap_no(orig_succ),
				PAGE_HEAP_NO_SUPREMUM);

	/* Reset the locks on the supremum of the left page, releasing
	waiting transactions */

	lock_rec_reset_and_release_wait_low(
		&lock_sys.rec_hash, l, PAGE_HEAP_NO_SUPREMUM);

	/* there should exist no page lock on the left page,
	otherwise, it will be blocked from merge */
	ut_ad(!lock_sys.get_first_prdt_page(l));

	lock_rec_free_all_from_discard_page(l);
}

/** Update locks when the root page is copied to another in
btr_root_raise_and_insert(). Note that we leave lock structs on the
root page, even though they do not make sense on other than leaf
pages: the reason is that in a pessimistic update the infimum record
of the root page will act as a dummy carrier of the locks of the record
to be updated. */
void lock_update_root_raise(const buf_block_t &block, const page_id_t root)
{
  LockMutexGuard g;
  /* Move the locks on the supremum of the root to the supremum of block */
  lock_rec_move(block, root, PAGE_HEAP_NO_SUPREMUM, PAGE_HEAP_NO_SUPREMUM);
}

/** Update the lock table when a page is copied to another.
@param new_block  the target page
@param old        old page (not index root page) */
void lock_update_copy_and_discard(const buf_block_t &new_block, page_id_t old)
{
  LockMutexGuard g;
  /* Move the locks on the supremum of the old page to the supremum of new */
  lock_rec_move(new_block, old, PAGE_HEAP_NO_SUPREMUM, PAGE_HEAP_NO_SUPREMUM);
  lock_rec_free_all_from_discard_page(old);
}

/*************************************************************//**
Updates the lock table when a page is split to the left. */
void
lock_update_split_left(
/*===================*/
	const buf_block_t*	right_block,	/*!< in: right page */
	const buf_block_t*	left_block)	/*!< in: left page */
{
  ulint	heap_no= lock_get_min_heap_no(right_block);
  const page_id_t r{right_block->page.id()};

  LockMutexGuard g;
  /* Inherit the locks to the supremum of the left page from the
  successor of the infimum on the right page */
  lock_rec_inherit_to_gap(*left_block, r, PAGE_HEAP_NO_SUPREMUM, heap_no);
}

/** Update the lock table when a page is merged to the left.
@param left      left page
@param orig_pred original predecessor of supremum on the left page before merge
@param right     merged, to-be-discarded right page */
void lock_update_merge_left(const buf_block_t& left, const rec_t *orig_pred,
                            const page_id_t right)
{
  ut_ad(left.frame == page_align(orig_pred));

  const page_id_t l{left.page.id()};

  LockMutexGuard g;
  const rec_t *left_next_rec= page_rec_get_next_const(orig_pred);

  if (!page_rec_is_supremum(left_next_rec))
  {
    /* Inherit the locks on the supremum of the left page to the
    first record which was moved from the right page */
    lock_rec_inherit_to_gap(left, l, page_rec_get_heap_no(left_next_rec),
                            PAGE_HEAP_NO_SUPREMUM);

    /* Reset the locks on the supremum of the left page,
    releasing waiting transactions */
    lock_rec_reset_and_release_wait_low(&lock_sys.rec_hash, l,
                                        PAGE_HEAP_NO_SUPREMUM);
  }

  /* Move the locks from the supremum of right page to the supremum
  of the left page */
  lock_rec_move(left, right, PAGE_HEAP_NO_SUPREMUM, PAGE_HEAP_NO_SUPREMUM);

  /* there should exist no page lock on the right page,
  otherwise, it will be blocked from merge */
  ut_ad(!lock_sys.get_first_prdt_page(right));
  lock_rec_free_all_from_discard_page(right);
}

/*************************************************************//**
Resets the original locks on heir and replaces them with gap type locks
inherited from rec. */
void
lock_rec_reset_and_inherit_gap_locks(
/*=================================*/
	const buf_block_t&	heir_block,	/*!< in: block containing the
						record which inherits */
	const page_id_t		donor,		/*!< in: page containing the
						record from which inherited;
						does NOT reset the locks on
						this record */
	ulint			heir_heap_no,	/*!< in: heap_no of the
						inheriting record */
	ulint			heap_no)	/*!< in: heap_no of the
						donating record */
{
  const page_id_t heir{heir_block.page.id()};
  LockMutexGuard g;
  lock_rec_reset_and_release_wait(heir, heir_heap_no);
  lock_rec_inherit_to_gap(heir_block, donor, heir_heap_no, heap_no);
}

/*************************************************************//**
Updates the lock table when a page is discarded. */
void
lock_update_discard(
/*================*/
	const buf_block_t*	heir_block,	/*!< in: index page
						which will inherit the locks */
	ulint			heir_heap_no,	/*!< in: heap_no of the record
						which will inherit the locks */
	const buf_block_t*	block)		/*!< in: index page
						which will be discarded */
{
	const page_t*	page = block->frame;
	const rec_t*	rec;
	ulint		heap_no;
	const page_id_t	page_id(block->page.id());

	lock_sys.mutex_lock();

	if (lock_sys.get_first(page_id)) {
		ut_ad(!lock_sys.get_first_prdt(page_id));
		ut_ad(!lock_sys.get_first_prdt_page(page_id));
		/* Inherit all the locks on the page to the record and
		reset all the locks on the page */

		if (page_is_comp(page)) {
			rec = page + PAGE_NEW_INFIMUM;

			do {
				heap_no = rec_get_heap_no_new(rec);

				lock_rec_inherit_to_gap(*heir_block, page_id,
							heir_heap_no, heap_no);

				lock_rec_reset_and_release_wait(
					page_id, heap_no);

				rec = page + rec_get_next_offs(rec, TRUE);
			} while (heap_no != PAGE_HEAP_NO_SUPREMUM);
		} else {
			rec = page + PAGE_OLD_INFIMUM;

			do {
				heap_no = rec_get_heap_no_old(rec);

				lock_rec_inherit_to_gap(*heir_block, page_id,
							heir_heap_no, heap_no);

				lock_rec_reset_and_release_wait(
					page_id, heap_no);

				rec = page + rec_get_next_offs(rec, FALSE);
			} while (heap_no != PAGE_HEAP_NO_SUPREMUM);
		}

		lock_rec_free_all_from_discard_page_low(page_id,
							&lock_sys.rec_hash);
	} else {
		lock_rec_free_all_from_discard_page_low(page_id,
							&lock_sys.prdt_hash);
		lock_rec_free_all_from_discard_page_low(
			page_id, &lock_sys.prdt_page_hash);
	}

	lock_sys.mutex_unlock();
}

/*************************************************************//**
Updates the lock table when a new user record is inserted. */
void
lock_update_insert(
/*===============*/
	const buf_block_t*	block,	/*!< in: buffer block containing rec */
	const rec_t*		rec)	/*!< in: the inserted record */
{
	ulint	receiver_heap_no;
	ulint	donator_heap_no;

	ut_ad(block->frame == page_align(rec));
	ut_ad(!page_rec_is_metadata(rec));

	/* Inherit the gap-locking locks for rec, in gap mode, from the next
	record */

	if (page_rec_is_comp(rec)) {
		receiver_heap_no = rec_get_heap_no_new(rec);
		donator_heap_no = rec_get_heap_no_new(
			page_rec_get_next_low(rec, TRUE));
	} else {
		receiver_heap_no = rec_get_heap_no_old(rec);
		donator_heap_no = rec_get_heap_no_old(
			page_rec_get_next_low(rec, FALSE));
	}

	lock_rec_inherit_to_gap_if_gap_lock(
		block, receiver_heap_no, donator_heap_no);
}

/*************************************************************//**
Updates the lock table when a record is removed. */
void
lock_update_delete(
/*===============*/
	const buf_block_t*	block,	/*!< in: buffer block containing rec */
	const rec_t*		rec)	/*!< in: the record to be removed */
{
	const page_t*	page = block->frame;
	ulint		heap_no;
	ulint		next_heap_no;

	ut_ad(page == page_align(rec));
	ut_ad(!page_rec_is_metadata(rec));

	if (page_is_comp(page)) {
		heap_no = rec_get_heap_no_new(rec);
		next_heap_no = rec_get_heap_no_new(page
						   + rec_get_next_offs(rec,
								       TRUE));
	} else {
		heap_no = rec_get_heap_no_old(rec);
		next_heap_no = rec_get_heap_no_old(page
						   + rec_get_next_offs(rec,
								       FALSE));
	}

	const page_id_t id{block->page.id()};
	LockMutexGuard g;

	/* Let the next record inherit the locks from rec, in gap mode */

	lock_rec_inherit_to_gap(*block, id, next_heap_no, heap_no);

	/* Reset the lock bits on rec and release waiting transactions */

	lock_rec_reset_and_release_wait(id, heap_no);
}

/*********************************************************************//**
Stores on the page infimum record the explicit locks of another record.
This function is used to store the lock state of a record when it is
updated and the size of the record changes in the update. The record
is moved in such an update, perhaps to another page. The infimum record
acts as a dummy carrier record, taking care of lock releases while the
actual record is being moved. */
void
lock_rec_store_on_page_infimum(
/*===========================*/
	const buf_block_t*	block,	/*!< in: buffer block containing rec */
	const rec_t*		rec)	/*!< in: record whose lock state
					is stored on the infimum
					record of the same page; lock
					bits are reset on the
					record */
{
  const ulint heap_no= page_rec_get_heap_no(rec);

  ut_ad(block->frame == page_align(rec));
  const page_id_t id{block->page.id()};

  LockMutexGuard g;
  lock_rec_move(*block, id, PAGE_HEAP_NO_INFIMUM, heap_no);
}

/** Restore the explicit lock requests on a single record, where the
state was stored on the infimum of a page.
@param block   buffer block containing rec
@param rec     record whose lock state is restored
@param donator page (rec is not necessarily on this page)
whose infimum stored the lock state; lock bits are reset on the infimum */
void lock_rec_restore_from_page_infimum(const buf_block_t &block,
					const rec_t *rec, page_id_t donator)
{
  const ulint heap_no= page_rec_get_heap_no(rec);
  LockMutexGuard g;
  lock_rec_move(block, donator, heap_no, PAGE_HEAP_NO_INFIMUM);
}

/*========================= TABLE LOCKS ==============================*/

/** Functor for accessing the embedded node within a table lock. */
struct TableLockGetNode {
	ut_list_node<lock_t>& operator() (lock_t& elem)
	{
		return(elem.un_member.tab_lock.locks);
	}
};

/*********************************************************************//**
Creates a table lock object and adds it as the last in the lock queue
of the table. Does NOT check for deadlocks or lock compatibility.
@return own: new lock object */
UNIV_INLINE
lock_t*
lock_table_create(
/*==============*/
	dict_table_t*	table,	/*!< in/out: database table
				in dictionary cache */
	unsigned	type_mode,/*!< in: lock mode possibly ORed with
				LOCK_WAIT */
	trx_t*		trx,	/*!< in: trx */
	lock_t*		c_lock)	/*!< in: conflicting lock */
{
	lock_t*		lock;

	ut_ad(table && trx);
	lock_sys.mutex_assert_locked();

	check_trx_state(trx);

	switch (LOCK_MODE_MASK & type_mode) {
	case LOCK_AUTO_INC:
		++table->n_waiting_or_granted_auto_inc_locks;
		/* For AUTOINC locking we reuse the lock instance only if
		there is no wait involved else we allocate the waiting lock
		from the transaction lock heap. */
		if (type_mode == LOCK_AUTO_INC) {
			lock = table->autoinc_lock;

			ut_ad(!table->autoinc_trx);
			table->autoinc_trx = trx;

			ib_vector_push(trx->autoinc_locks, &lock);
			goto allocated;
		}

		break;
	case LOCK_X:
	case LOCK_S:
		++table->n_lock_x_or_s;
		break;
	}

	lock = trx->lock.table_cached < array_elements(trx->lock.table_pool)
		? &trx->lock.table_pool[trx->lock.table_cached++]
		: static_cast<lock_t*>(
			mem_heap_alloc(trx->lock.lock_heap, sizeof *lock));

allocated:
	lock->type_mode = ib_uint32_t(type_mode | LOCK_TABLE);
	lock->trx = trx;

	lock->un_member.tab_lock.table = table;

	ut_ad(table->get_ref_count() > 0 || !table->can_be_evicted);

	UT_LIST_ADD_LAST(trx->lock.trx_locks, lock);

#ifdef WITH_WSREP
	if (c_lock && trx->is_wsrep()) {
		if (wsrep_thd_is_BF(trx->mysql_thd, FALSE)) {
			ut_list_insert(table->locks, c_lock, lock,
				       TableLockGetNode());
			if (UNIV_UNLIKELY(wsrep_debug)) {
				wsrep_report_bf_lock_wait(trx->mysql_thd, trx->id);
				wsrep_report_bf_lock_wait(c_lock->trx->mysql_thd, c_lock->trx->id);
			}
		} else {
			ut_list_append(table->locks, lock, TableLockGetNode());
		}

		trx->mutex.wr_unlock();
		mysql_mutex_lock(&lock_sys.wait_mutex);
		c_lock->trx->mutex.wr_lock();

		if (c_lock->trx->lock.wait_thr) {
			c_lock->trx->lock.was_chosen_as_deadlock_victim = TRUE;

			if (UNIV_UNLIKELY(wsrep_debug)) {
				wsrep_report_bf_lock_wait(trx->mysql_thd, trx->id);
				wsrep_report_bf_lock_wait(c_lock->trx->mysql_thd, c_lock->trx->id);
				wsrep_print_wait_locks(c_lock);
			}

			/* The lock release will call lock_grant(),
			which would acquire trx->mutex again. */
			lock_cancel_waiting_and_release(
				c_lock->trx->lock.wait_lock);
		}

		mysql_mutex_unlock(&lock_sys.wait_mutex);
		c_lock->trx->mutex.wr_unlock();
		trx->mutex.wr_lock();
	} else
#endif /* WITH_WSREP */
	ut_list_append(table->locks, lock, TableLockGetNode());

	if (type_mode & LOCK_WAIT) {
		if (trx->lock.wait_trx) {
			ut_ad(!c_lock || trx->lock.wait_trx == c_lock->trx);
			ut_ad(trx->lock.wait_lock);
			ut_ad((*trx->lock.wait_lock).trx == trx);
		} else {
			ut_ad(c_lock);
			trx->lock.wait_trx = c_lock->trx;
			ut_ad(!trx->lock.wait_lock);
		}
		trx->lock.wait_lock = lock;
	}

	lock->trx->lock.table_locks.push_back(lock);

	MONITOR_INC(MONITOR_TABLELOCK_CREATED);
	MONITOR_INC(MONITOR_NUM_TABLELOCK);

	return(lock);
}

/*************************************************************//**
Pops autoinc lock requests from the transaction's autoinc_locks. We
handle the case where there are gaps in the array and they need to
be popped off the stack. */
UNIV_INLINE
void
lock_table_pop_autoinc_locks(
/*=========================*/
	trx_t*	trx)	/*!< in/out: transaction that owns the AUTOINC locks */
{
	lock_sys.mutex_assert_locked();
	ut_ad(!ib_vector_is_empty(trx->autoinc_locks));

	/* Skip any gaps, gaps are NULL lock entries in the
	trx->autoinc_locks vector. */

	do {
		ib_vector_pop(trx->autoinc_locks);

		if (ib_vector_is_empty(trx->autoinc_locks)) {
			return;
		}

	} while (*(lock_t**) ib_vector_get_last(trx->autoinc_locks) == NULL);
}

/*************************************************************//**
Removes an autoinc lock request from the transaction's autoinc_locks. */
UNIV_INLINE
void
lock_table_remove_autoinc_lock(
/*===========================*/
	lock_t*	lock,	/*!< in: table lock */
	trx_t*	trx)	/*!< in/out: transaction that owns the lock */
{
	lock_t*	autoinc_lock;
	lint	i = ib_vector_size(trx->autoinc_locks) - 1;

	ut_ad(lock->type_mode == (LOCK_AUTO_INC | LOCK_TABLE));
	lock_sys.mutex_assert_locked();
	ut_ad(!ib_vector_is_empty(trx->autoinc_locks));

	/* With stored functions and procedures the user may drop
	a table within the same "statement". This special case has
	to be handled by deleting only those AUTOINC locks that were
	held by the table being dropped. */

	autoinc_lock = *static_cast<lock_t**>(
		ib_vector_get(trx->autoinc_locks, i));

	/* This is the default fast case. */

	if (autoinc_lock == lock) {
		lock_table_pop_autoinc_locks(trx);
	} else {
		/* The last element should never be NULL */
		ut_a(autoinc_lock != NULL);

		/* Handle freeing the locks from within the stack. */

		while (--i >= 0) {
			autoinc_lock = *static_cast<lock_t**>(
				ib_vector_get(trx->autoinc_locks, i));

			if (autoinc_lock == lock) {
				void*	null_var = NULL;
				ib_vector_set(trx->autoinc_locks, i, &null_var);
				return;
			}
		}

		/* Must find the autoinc lock. */
		ut_error;
	}
}

/*************************************************************//**
Removes a table lock request from the queue and the trx list of locks;
this is a low-level function which does NOT check if waiting requests
can now be granted. */
UNIV_INLINE
void
lock_table_remove_low(
/*==================*/
	lock_t*	lock)	/*!< in/out: table lock */
{
	trx_t*		trx;
	dict_table_t*	table;

	lock_sys.mutex_assert_locked();

	trx = lock->trx;
	table = lock->un_member.tab_lock.table;

	/* Remove the table from the transaction's AUTOINC vector, if
	the lock that is being released is an AUTOINC lock. */
	switch (lock->mode()) {
	case LOCK_AUTO_INC:
		ut_ad((table->autoinc_trx == trx) == !lock->is_waiting());

		if (table->autoinc_trx == trx) {
			table->autoinc_trx = NULL;
			/* The locks must be freed in the reverse order from
			the one in which they were acquired. This is to avoid
			traversing the AUTOINC lock vector unnecessarily.

			We only store locks that were granted in the
			trx->autoinc_locks vector (see lock_table_create()
			and lock_grant()). */
			lock_table_remove_autoinc_lock(lock, trx);
		}

		ut_ad(table->n_waiting_or_granted_auto_inc_locks);
		--table->n_waiting_or_granted_auto_inc_locks;
		break;
	case LOCK_X:
	case LOCK_S:
		ut_ad(table->n_lock_x_or_s);
		--table->n_lock_x_or_s;
		break;
	default:
		break;
	}

	UT_LIST_REMOVE(trx->lock.trx_locks, lock);
	ut_list_remove(table->locks, lock, TableLockGetNode());

	MONITOR_INC(MONITOR_TABLELOCK_REMOVED);
	MONITOR_DEC(MONITOR_NUM_TABLELOCK);
}

/*********************************************************************//**
Enqueues a waiting request for a table lock which cannot be granted
immediately. Checks for deadlocks.
@retval	DB_LOCK_WAIT	if the waiting lock was enqueued
@retval	DB_DEADLOCK	if this transaction was chosen as the victim */
static
dberr_t
lock_table_enqueue_waiting(
/*=======================*/
	unsigned	mode,	/*!< in: lock mode this transaction is
				requesting */
	dict_table_t*	table,	/*!< in/out: table */
	que_thr_t*	thr,	/*!< in: query thread */
	lock_t*		c_lock)	/*!< in: conflicting lock or NULL */
{
	trx_t*		trx;

	lock_sys.mutex_assert_locked();
	ut_ad(!srv_read_only_mode);

	trx = thr_get_trx(thr);

	switch (trx_get_dict_operation(trx)) {
	case TRX_DICT_OP_NONE:
		break;
	case TRX_DICT_OP_TABLE:
	case TRX_DICT_OP_INDEX:
		ib::error() << "A table lock wait happens in a dictionary"
			" operation. Table " << table->name
			<< ". " << BUG_REPORT_MSG;
		ut_ad(0);
	}

#ifdef WITH_WSREP
	if (trx->is_wsrep() && trx->lock.was_chosen_as_deadlock_victim) {
		return(DB_DEADLOCK);
	}
#endif /* WITH_WSREP */

	/* Enqueue the lock request that will wait to be granted */
	lock_table_create(table, mode | LOCK_WAIT, trx, c_lock);

	trx->lock.wait_thr = thr;
	trx->lock.was_chosen_as_deadlock_victim = false;

	MONITOR_INC(MONITOR_TABLELOCK_WAIT);
	return(DB_LOCK_WAIT);
}

/*********************************************************************//**
Checks if other transactions have an incompatible mode lock request in
the lock queue.
@return lock or NULL */
UNIV_INLINE
lock_t*
lock_table_other_has_incompatible(
/*==============================*/
	const trx_t*		trx,	/*!< in: transaction, or NULL if all
					transactions should be included */
	ulint			wait,	/*!< in: LOCK_WAIT if also
					waiting locks are taken into
					account, or 0 if not */
	const dict_table_t*	table,	/*!< in: table */
	lock_mode		mode)	/*!< in: lock mode */
{
	lock_sys.mutex_assert_locked();

	static_assert(LOCK_IS == 0, "compatibility");
	static_assert(LOCK_IX == 1, "compatibility");

	if (UNIV_LIKELY(mode <= LOCK_IX && !table->n_lock_x_or_s)) {
		return(NULL);
	}

	for (lock_t* lock = UT_LIST_GET_LAST(table->locks);
	     lock;
	     lock = UT_LIST_GET_PREV(un_member.tab_lock.locks, lock)) {

		if (lock->trx != trx
		    && !lock_mode_compatible(lock->mode(), mode)
		    && (wait || !lock->is_waiting())) {

#ifdef WITH_WSREP
			if (lock->trx->is_wsrep()) {
				if (UNIV_UNLIKELY(wsrep_debug)) {
					ib::info() << "WSREP: table lock abort for table:"
						   << table->name;
					ib::info() << " SQL: "
					   << wsrep_thd_query(lock->trx->mysql_thd);
				}
				lock->trx->mutex.wr_lock();
				wsrep_kill_victim((trx_t *)trx, (lock_t *)lock);
				lock->trx->mutex.wr_unlock();
			}
#endif /* WITH_WSREP */

			return(lock);
		}
	}

	return(NULL);
}

/*********************************************************************//**
Locks the specified database table in the mode given. If the lock cannot
be granted immediately, the query thread is put to wait.
@return DB_SUCCESS, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_table(
/*=======*/
	dict_table_t*	table,	/*!< in/out: database table
				in dictionary cache */
	lock_mode	mode,	/*!< in: lock mode */
	que_thr_t*	thr)	/*!< in: query thread */
{
	trx_t*		trx;
	dberr_t		err;
	lock_t*		wait_for;

	if (table->is_temporary()) {
		return DB_SUCCESS;
	}

	trx = thr_get_trx(thr);

	/* Look for equal or stronger locks the same trx already
	has on the table. No need to acquire LockMutexGuard here
	because only this transacton can add/access table locks
	to/from trx_t::table_locks. */

	if (lock_table_has(trx, table, mode) || srv_read_only_mode) {
		return(DB_SUCCESS);
	}

	/* Read only transactions can write to temp tables, we don't want
	to promote them to RW transactions. Their updates cannot be visible
	to other transactions. Therefore we can keep them out
	of the read views. */

	if ((mode == LOCK_IX || mode == LOCK_X)
	    && !trx->read_only
	    && trx->rsegs.m_redo.rseg == 0) {

		trx_set_rw_mode(trx);
	}

	err = DB_SUCCESS;

	lock_sys.mutex_lock();

	/* We have to check if the new lock is compatible with any locks
	other transactions have in the table lock queue. */

	wait_for = lock_table_other_has_incompatible(
		trx, LOCK_WAIT, table, mode);

	trx->mutex.wr_lock();

	if (wait_for) {
		err = lock_table_enqueue_waiting(mode, table, thr, wait_for);
	} else {
		lock_table_create(table, mode, trx, wait_for);
	}

	lock_sys.mutex_unlock();

	trx->mutex.wr_unlock();

	return(err);
}

/** Create a table lock object for a resurrected transaction.
@param table    table to be X-locked
@param trx      transaction
@param mode     LOCK_X or LOCK_IX */
void lock_table_resurrect(dict_table_t *table, trx_t *trx, lock_mode mode)
{
  ut_ad(trx->is_recovered);
  ut_ad(mode == LOCK_X || mode == LOCK_IX);

  if (lock_table_has(trx, table, mode))
    return;

  {
    LockMutexGuard g;
    ut_ad(!lock_table_other_has_incompatible(trx, LOCK_WAIT, table, mode));

    trx->mutex.wr_lock();
    lock_table_create(table, mode, trx, nullptr);
  }
  trx->mutex.wr_unlock();
}

/** Find a lock that a waiting table lock request still has to wait for. */
static const lock_t *lock_table_has_to_wait_in_queue(const lock_t *wait_lock)
{
	ut_ad(wait_lock->is_waiting());
	ut_ad(wait_lock->is_table());

	dict_table_t *table = wait_lock->un_member.tab_lock.table;
	lock_sys.mutex_assert_locked();

	static_assert(LOCK_IS == 0, "compatibility");
	static_assert(LOCK_IX == 1, "compatibility");

	if (UNIV_LIKELY(wait_lock->mode() <= LOCK_IX
			&& !table->n_lock_x_or_s)) {
		return nullptr;
	}

	for (const lock_t *lock = UT_LIST_GET_FIRST(table->locks);
	     lock != wait_lock;
	     lock = UT_LIST_GET_NEXT(un_member.tab_lock.locks, lock)) {

		if (lock_has_to_wait(wait_lock, lock)) {

			return lock;
		}
	}

	return nullptr;
}

/*************************************************************//**
Removes a table lock request, waiting or granted, from the queue and grants
locks to other transactions in the queue, if they now are entitled to a
lock. */
static
void
lock_table_dequeue(
/*===============*/
	lock_t*	in_lock)/*!< in/out: table lock object; transactions waiting
			behind will get their lock requests granted, if
			they are now qualified to it */
{
	mysql_mutex_assert_owner(&lock_sys.wait_mutex);
	ut_a(in_lock->is_table());
	const dict_table_t* table = in_lock->un_member.tab_lock.table;
	lock_sys.mutex_assert_locked();
	lock_t*	lock = UT_LIST_GET_NEXT(un_member.tab_lock.locks, in_lock);

	lock_table_remove_low(in_lock);

	static_assert(LOCK_IS == 0, "compatibility");
	static_assert(LOCK_IX == 1, "compatibility");

	if (UNIV_LIKELY(in_lock->mode() <= LOCK_IX && !table->n_lock_x_or_s)) {
		return;
	}

	/* Check if waiting locks in the queue can now be granted: grant
	locks if there are no conflicting locks ahead. */

	for (/* No op */;
	     lock != NULL;
	     lock = UT_LIST_GET_NEXT(un_member.tab_lock.locks, lock)) {
		if (!lock->is_waiting()) {
			continue;
		}

		ut_ad(lock->trx->lock.wait_trx);
		ut_ad(lock->trx->lock.wait_lock);

		if (const lock_t* c = lock_table_has_to_wait_in_queue(lock)) {
			lock->trx->lock.wait_trx = c->trx;
		} else {
			/* Grant the lock */
			ut_ad(in_lock->trx != lock->trx);
			lock_grant(lock);
		}
	}
}

/** Release a table X lock after rolling back an insert into an empty table
(which was covered by a TRX_UNDO_EMPTY record).
@param table    table to be X-unlocked
@param trx      transaction */
void lock_table_x_unlock(dict_table_t *table, trx_t *trx)
{
  ut_ad(!trx->is_recovered);

  lock_sys.mutex_lock();
  mysql_mutex_lock(&lock_sys.wait_mutex);

  for (lock_t*& lock : trx->lock.table_locks)
  {
    if (!lock || lock->trx != trx)
      continue;
    ut_ad(!lock->is_waiting());
    if (lock->type_mode == (LOCK_TABLE | LOCK_X))
    {
      lock_table_dequeue(lock);
      lock= nullptr;
      goto func_exit;
    }
  }
  ut_ad("lock not found" == 0);

func_exit:
  lock_sys.mutex_unlock();
  mysql_mutex_unlock(&lock_sys.wait_mutex);
}

/** Sets a lock on a table based on the given mode.
@param[in]	table	table to lock
@param[in,out]	trx	transaction
@param[in]	mode	LOCK_X or LOCK_S
@return error code or DB_SUCCESS. */
dberr_t
lock_table_for_trx(
	dict_table_t*	table,
	trx_t*		trx,
	enum lock_mode	mode)
{
	mem_heap_t*	heap;
	que_thr_t*	thr;
	dberr_t		err;
	sel_node_t*	node;
	heap = mem_heap_create(512);

	node = sel_node_create(heap);
	thr = pars_complete_graph_for_exec(node, trx, heap, NULL);
	thr->graph->state = QUE_FORK_ACTIVE;

	/* We use the select query graph as the dummy graph needed
	in the lock module call */

	thr = static_cast<que_thr_t*>(
		que_fork_get_first_thr(
			static_cast<que_fork_t*>(que_node_get_parent(thr))));

run_again:
	thr->run_node = thr;
	thr->prev_node = thr->common.parent;

	err = lock_table(table, mode, thr);

	trx->error_state = err;

	if (UNIV_UNLIKELY(err != DB_SUCCESS)) {
		if (row_mysql_handle_errors(&err, trx, thr, NULL)) {
			goto run_again;
		}
	}

	que_graph_free(thr->graph);
	trx->op_info = "";

	return(err);
}

/*=========================== LOCK RELEASE ==============================*/

/*************************************************************//**
Removes a granted record lock of a transaction from the queue and grants
locks to other transactions waiting in the queue if they now are entitled
to a lock. */
void
lock_rec_unlock(
/*============*/
	trx_t*			trx,	/*!< in/out: transaction that has
					set a record lock */
	const page_id_t		id,	/*!< in: page containing rec */
	const rec_t*		rec,	/*!< in: record */
	lock_mode		lock_mode)/*!< in: LOCK_S or LOCK_X */
{
	lock_t*		first_lock;
	lock_t*		lock;
	ulint		heap_no;

	ut_ad(trx);
	ut_ad(rec);
	ut_ad(!trx->lock.wait_lock);
	ut_ad(trx_state_eq(trx, TRX_STATE_ACTIVE));
	ut_ad(!page_rec_is_metadata(rec));

	heap_no = page_rec_get_heap_no(rec);

	lock_sys.mutex_lock();

	first_lock = lock_rec_get_first(&lock_sys.rec_hash, id, heap_no);

	/* Find the last lock with the same lock_mode and transaction
	on the record. */

	for (lock = first_lock; lock != NULL;
	     lock = lock_rec_get_next(heap_no, lock)) {
		if (lock->trx == trx && lock->mode() == lock_mode) {
			goto released;
		}
	}

	lock_sys.mutex_unlock();

	{
		ib::error	err;
		err << "Unlock row could not find a " << lock_mode
			<< " mode lock on the record. Current statement: ";
		size_t		stmt_len;
		if (const char* stmt = innobase_get_stmt_unsafe(
			    trx->mysql_thd, &stmt_len)) {
			err.write(stmt, stmt_len);
		}
	}

	return;

released:
	ut_a(!lock->is_waiting());
	lock_rec_reset_nth_bit(lock, heap_no);

	/* Check if we can now grant waiting lock requests */

	for (lock = first_lock; lock != NULL;
	     lock = lock_rec_get_next(heap_no, lock)) {
		if (!lock->is_waiting()) {
			continue;
		}
		mysql_mutex_lock(&lock_sys.wait_mutex);
		ut_ad(lock->trx->lock.wait_trx);
		ut_ad(lock->trx->lock.wait_lock);

		if (const lock_t* c = lock_rec_has_to_wait_in_queue(lock)) {
#ifdef WITH_WSREP
			wsrep_assert_no_bf_bf_wait(c, lock, c->trx);
#endif /* WITH_WSREP */
			lock->trx->lock.wait_trx = c->trx;
		} else {
			/* Grant the lock */
			ut_ad(trx != lock->trx);
			lock_grant(lock);
		}
		mysql_mutex_unlock(&lock_sys.wait_mutex);
	}

	lock_sys.mutex_unlock();
}

#ifdef UNIV_DEBUG
/*********************************************************************//**
Check if a transaction that has X or IX locks has set the dict_op
code correctly. */
static
void
lock_check_dict_lock(
/*==================*/
	const lock_t*	lock)	/*!< in: lock to check */
{
	if (!lock->is_table()) {
		ut_ad(!lock->index->table->is_temporary());

		/* Check if the transcation locked a record
		in a system table in X mode. It should have set
		the dict_op code correctly if it did. */
		if (lock->mode() == LOCK_X
		    && lock->index->table->id < DICT_HDR_FIRST_ID) {
			ut_ad(lock->trx->dict_operation != TRX_DICT_OP_NONE);
		}
	} else {
		const dict_table_t* table = lock->un_member.tab_lock.table;
		ut_ad(!table->is_temporary());
		if (table->id >= DICT_HDR_FIRST_ID) {
			return;
		}

		/* Check if the transcation locked a system table
		in IX mode. It should have set the dict_op code
		correctly if it did. */
		switch (lock->mode()) {
		case LOCK_X:
		case LOCK_IX:
			ut_ad(lock->trx->dict_operation != TRX_DICT_OP_NONE);
			break;
		default:
			break;
		}
	}
}
#endif /* UNIV_DEBUG */

/** Release the explicit locks of a committing transaction,
and release possible other transactions waiting because of these locks. */
void lock_release(trx_t* trx)
{
	ulint		count = 0;
	trx_id_t	max_trx_id = trx_sys.get_max_trx_id();

	lock_sys.mutex_lock();
	mysql_mutex_lock(&lock_sys.wait_mutex);

	for (lock_t* lock = UT_LIST_GET_LAST(trx->lock.trx_locks);
	     lock != NULL;
	     lock = UT_LIST_GET_LAST(trx->lock.trx_locks)) {

		ut_d(lock_check_dict_lock(lock));

		if (!lock->is_table()) {
			lock_rec_dequeue_from_page(lock);
		} else {
			if (lock->mode() != LOCK_IS && trx->undo_no) {
				/* The trx may have modified the table. We
				block the use of the query cache for
				all currently active transactions. */
				lock->un_member.tab_lock.table
					->query_cache_inv_trx_id = max_trx_id;
			}

			lock_table_dequeue(lock);
		}

		if (count == 1000) {
			/* Release the  mutex for a while, so that we
			do not monopolize it */

			lock_sys.mutex_unlock();
			mysql_mutex_unlock(&lock_sys.wait_mutex);
			count = 0;
			lock_sys.mutex_lock();
			mysql_mutex_lock(&lock_sys.wait_mutex);
		}

		++count;
	}

	lock_sys.mutex_unlock();
	mysql_mutex_unlock(&lock_sys.wait_mutex);
}

/*********************************************************************//**
Removes table locks of the transaction on a table to be dropped. */
static
void
lock_trx_table_locks_remove(
/*========================*/
	const lock_t*	lock_to_remove)		/*!< in: lock to remove */
{
	trx_t*		trx = lock_to_remove->trx;

	ut_ad(lock_to_remove->is_table());
	lock_sys.mutex_assert_locked();

	/* It is safe to read this because we are holding the lock mutex */
	const bool have_mutex = trx->lock.cancel;
	if (!have_mutex) {
		trx->mutex.wr_lock();
	}

	for (lock_list::iterator it = trx->lock.table_locks.begin(),
             end = trx->lock.table_locks.end(); it != end; ++it) {
		const lock_t*	lock = *it;

		ut_ad(!lock || trx == lock->trx);
		ut_ad(!lock || lock->is_table());
		ut_ad(!lock || lock->un_member.tab_lock.table);

		if (lock == lock_to_remove) {
			*it = NULL;

			if (!have_mutex) {
				trx->mutex.wr_unlock();
			}

			return;
		}
	}

	/* Lock must exist in the vector. */
	ut_error;
}

/*===================== VALIDATION AND DEBUGGING ====================*/

/** Print info of a table lock.
@param[in,out]	file	output stream
@param[in]	lock	table lock */
static
void
lock_table_print(FILE* file, const lock_t* lock)
{
	lock_sys.mutex_assert_locked();
	ut_a(lock->is_table());

	fputs("TABLE LOCK table ", file);
	ut_print_name(file, lock->trx,
		      lock->un_member.tab_lock.table->name.m_name);
	fprintf(file, " trx id " TRX_ID_FMT, lock->trx->id);

	switch (auto mode = lock->mode()) {
	case LOCK_S:
		fputs(" lock mode S", file);
		break;
	case LOCK_X:
		ut_ad(lock->trx->id != 0);
		fputs(" lock mode X", file);
		break;
	case LOCK_IS:
		fputs(" lock mode IS", file);
		break;
	case LOCK_IX:
		ut_ad(lock->trx->id != 0);
		fputs(" lock mode IX", file);
		break;
	case LOCK_AUTO_INC:
		fputs(" lock mode AUTO-INC", file);
		break;
	default:
		fprintf(file, " unknown lock mode %u", mode);
	}

	if (lock->is_waiting()) {
		fputs(" waiting", file);
	}

	putc('\n', file);
}

/** Pretty-print a record lock.
@param[in,out]	file	output stream
@param[in]	lock	record lock
@param[in,out]	mtr	mini-transaction for accessing the record */
static void lock_rec_print(FILE* file, const lock_t* lock, mtr_t& mtr)
{
	ut_ad(!lock->is_table());

	const page_id_t page_id{lock->un_member.rec_lock.page_id};
	lock_sys.mutex_assert_locked();

	fprintf(file, "RECORD LOCKS space id %u page no %u n bits " ULINTPF
		" index %s of table ",
		page_id.space(), page_id.page_no(),
		lock_rec_get_n_bits(lock),
		lock->index->name());
	ut_print_name(file, lock->trx, lock->index->table->name.m_name);
	fprintf(file, " trx id " TRX_ID_FMT, lock->trx->id);

	switch (lock->mode()) {
	case LOCK_S:
		fputs(" lock mode S", file);
		break;
	case LOCK_X:
		fputs(" lock_mode X", file);
		break;
	default:
		ut_error;
	}

	if (lock->is_gap()) {
		fputs(" locks gap before rec", file);
	}

	if (lock->is_record_not_gap()) {
		fputs(" locks rec but not gap", file);
	}

	if (lock->is_insert_intention()) {
		fputs(" insert intention", file);
	}

	if (lock->is_waiting()) {
		fputs(" waiting", file);
	}

	putc('\n', file);

	mem_heap_t*		heap		= NULL;
	rec_offs		offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*		offsets		= offsets_;
	rec_offs_init(offsets_);

	mtr.start();
	const buf_block_t* block = buf_page_try_get(page_id, &mtr);

	for (ulint i = 0; i < lock_rec_get_n_bits(lock); ++i) {

		if (!lock_rec_get_nth_bit(lock, i)) {
			continue;
		}

		fprintf(file, "Record lock, heap no %lu", (ulong) i);

		if (block) {
			ut_ad(page_is_leaf(block->frame));
			const rec_t*	rec;

			rec = page_find_rec_with_heap_no(
				buf_block_get_frame(block), i);
			ut_ad(!page_rec_is_metadata(rec));

			offsets = rec_get_offsets(
				rec, lock->index, offsets, true,
				ULINT_UNDEFINED, &heap);

			putc(' ', file);
			rec_print_new(file, rec, offsets);
		}

		putc('\n', file);
	}

	mtr.commit();

	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}
}

#ifdef UNIV_DEBUG
/* Print the number of lock structs from lock_print_info_summary() only
in non-production builds for performance reasons, see
http://bugs.mysql.com/36942 */
#define PRINT_NUM_OF_LOCK_STRUCTS
#endif /* UNIV_DEBUG */

#ifdef PRINT_NUM_OF_LOCK_STRUCTS
/*********************************************************************//**
Calculates the number of record lock structs in the record lock hash table.
@return number of record locks */
static ulint lock_get_n_rec_locks()
{
	ulint	n_locks	= 0;
	ulint	i;

	lock_sys.mutex_assert_locked();

	for (i = 0; i < lock_sys.rec_hash.n_cells; i++) {
		const lock_t*	lock;

		for (lock = static_cast<const lock_t*>(
			     HASH_GET_FIRST(&lock_sys.rec_hash, i));
		     lock != 0;
		     lock = static_cast<const lock_t*>(
				HASH_GET_NEXT(hash, lock))) {

			n_locks++;
		}
	}

	return(n_locks);
}
#endif /* PRINT_NUM_OF_LOCK_STRUCTS */

/*********************************************************************//**
Prints info of locks for all transactions.
@return FALSE if not able to obtain lock mutex
and exits without printing info */
ibool
lock_print_info_summary(
/*====================*/
	FILE*	file,	/*!< in: file where to print */
	ibool	nowait)	/*!< in: whether to wait for the lock mutex */
{
	/* if nowait is FALSE, wait on the lock mutex,
	otherwise return immediately if fail to obtain the
	mutex. */
	if (!nowait) {
		lock_sys.mutex_lock();
	} else if (lock_sys.mutex_trylock()) {
		fputs("FAIL TO OBTAIN LOCK MUTEX,"
		      " SKIP LOCK INFO PRINTING\n", file);
		return(FALSE);
	}

	if (lock_sys.deadlocks) {
		fputs("------------------------\n"
		      "LATEST DETECTED DEADLOCK\n"
		      "------------------------\n", file);

		if (!srv_read_only_mode) {
			ut_copy_file(file, lock_latest_err_file);
		}
	}

	fputs("------------\n"
	      "TRANSACTIONS\n"
	      "------------\n", file);

	fprintf(file, "Trx id counter " TRX_ID_FMT "\n",
		trx_sys.get_max_trx_id());

	fprintf(file,
		"Purge done for trx's n:o < " TRX_ID_FMT
		" undo n:o < " TRX_ID_FMT " state: %s\n"
		"History list length %u\n",
		purge_sys.tail.trx_no(),
		purge_sys.tail.undo_no,
		purge_sys.enabled()
		? (purge_sys.running() ? "running"
		   : purge_sys.paused() ? "stopped" : "running but idle")
		: "disabled",
		uint32_t{trx_sys.rseg_history_len});

#ifdef PRINT_NUM_OF_LOCK_STRUCTS
	fprintf(file,
		"Total number of lock structs in row lock hash table %lu\n",
		(ulong) lock_get_n_rec_locks());
#endif /* PRINT_NUM_OF_LOCK_STRUCTS */
	return(TRUE);
}

/** Prints transaction lock wait and MVCC state.
@param[in,out]	file	file where to print
@param[in]	trx	transaction
@param[in]	now	current my_hrtime_coarse() */
void lock_trx_print_wait_and_mvcc_state(FILE *file, const trx_t *trx,
                                        my_hrtime_t now)
{
	fprintf(file, "---");

	trx_print_latched(file, trx, 600);
	trx->read_view.print_limits(file);

	if (const lock_t* wait_lock = trx->lock.wait_lock) {
		fprintf(file,
			"------- TRX HAS BEEN WAITING %llu ns"
			" FOR THIS LOCK TO BE GRANTED:\n",
			now.val - trx->lock.suspend_time.val);

		if (!wait_lock->is_table()) {
			mtr_t mtr;
			lock_rec_print(file, wait_lock, mtr);
		} else {
			lock_table_print(file, wait_lock);
		}

		fprintf(file, "------------------\n");
	}
}

/*********************************************************************//**
Prints info of locks for a transaction. */
static
void
lock_trx_print_locks(
/*=================*/
	FILE*		file,		/*!< in/out: File to write */
	const trx_t*	trx)		/*!< in: current transaction */
{
	mtr_t mtr;
	uint32_t i= 0;
	/* Iterate over the transaction's locks. */
	for (lock_t *lock = UT_LIST_GET_FIRST(trx->lock.trx_locks);
	     lock != NULL;
	     lock = UT_LIST_GET_NEXT(trx_locks, lock)) {
		if (!lock->is_table()) {
			lock_rec_print(file, lock, mtr);
		} else {
			lock_table_print(file, lock);
		}

		if (++i == 10) {

			fprintf(file,
				"10 LOCKS PRINTED FOR THIS TRX:"
				" SUPPRESSING FURTHER PRINTS\n");

			break;
		}
	}
}

/** Functor to display all transactions */
struct lock_print_info
{
  lock_print_info(FILE* file, my_hrtime_t now) :
    file(file), now(now),
    purge_trx(purge_sys.query ? purge_sys.query->trx : nullptr)
  {}

  void operator()(const trx_t &trx) const
  {
    if (UNIV_UNLIKELY(&trx == purge_trx))
      return;
    lock_trx_print_wait_and_mvcc_state(file, &trx, now);

    if (trx.will_lock && srv_print_innodb_lock_monitor)
      lock_trx_print_locks(file, &trx);
  }

  FILE* const file;
  const my_hrtime_t now;
  const trx_t* const purge_trx;
};

/*********************************************************************//**
Prints info of locks for each transaction. This function assumes that the
caller holds the lock mutex and more importantly it will release the lock
mutex on behalf of the caller. (This should be fixed in the future). */
void
lock_print_info_all_transactions(
/*=============================*/
	FILE*		file)	/*!< in/out: file where to print */
{
	lock_sys.mutex_assert_locked();

	fprintf(file, "LIST OF TRANSACTIONS FOR EACH SESSION:\n");

	trx_sys.trx_list.for_each(lock_print_info(file, my_hrtime_coarse()));
	lock_sys.mutex_unlock();

	ut_ad(lock_validate());
}

#ifdef UNIV_DEBUG
/*********************************************************************//**
Find the the lock in the trx_t::trx_lock_t::table_locks vector.
@return true if found */
static
bool
lock_trx_table_locks_find(
/*======================*/
	trx_t*		trx,		/*!< in: trx to validate */
	const lock_t*	find_lock)	/*!< in: lock to find */
{
	bool		found = false;

	for (lock_list::const_iterator it = trx->lock.table_locks.begin(),
             end = trx->lock.table_locks.end(); it != end; ++it) {

		const lock_t*	lock = *it;

		if (lock == NULL) {

			continue;

		} else if (lock == find_lock) {

			/* Can't be duplicates. */
			ut_a(!found);
			found = true;
		}

		ut_a(trx == lock->trx);
		ut_a(lock->is_table());
		ut_a(lock->un_member.tab_lock.table != NULL);
	}

	return(found);
}

/*********************************************************************//**
Validates the lock queue on a table.
@return TRUE if ok */
static
ibool
lock_table_queue_validate(
/*======================*/
	const dict_table_t*	table)	/*!< in: table */
{
	const lock_t*	lock;

	lock_sys.mutex_assert_locked();

	for (lock = UT_LIST_GET_FIRST(table->locks);
	     lock != NULL;
	     lock = UT_LIST_GET_NEXT(un_member.tab_lock.locks, lock)) {

		/* lock->trx->state cannot change from or to NOT_STARTED
		while we are holding the lock_sys.mutex. It may change
		from ACTIVE or PREPARED to PREPARED or COMMITTED. */
		lock->trx->mutex.wr_lock();
		check_trx_state(lock->trx);

		if (lock->trx->state == TRX_STATE_COMMITTED_IN_MEMORY) {
		} else if (!lock->is_waiting()) {
			ut_a(!lock_table_other_has_incompatible(
				     lock->trx, 0, table,
				     lock->mode()));
		} else {
			ut_a(lock_table_has_to_wait_in_queue(lock));
		}

		ut_a(lock_trx_table_locks_find(lock->trx, lock));
		lock->trx->mutex.wr_unlock();
	}

	return(TRUE);
}

/*********************************************************************//**
Validates the lock queue on a single record.
@return TRUE if ok */
static
bool
lock_rec_queue_validate(
/*====================*/
	bool			locked_lock_trx_sys,
					/*!< in: if the caller holds
					both the lock mutex and
					trx_sys_t->lock. */
	const page_id_t		id,	/*!< in: page identifier */
	const rec_t*		rec,	/*!< in: record to look at */
	const dict_index_t*	index,	/*!< in: index, or NULL if not known */
	const rec_offs*		offsets)/*!< in: rec_get_offsets(rec, index) */
{
	const lock_t*	lock;
	ulint		heap_no;

	ut_a(rec);
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(!page_rec_is_comp(rec) == !rec_offs_comp(offsets));
	ut_ad(page_rec_is_leaf(rec));
	ut_ad(!index || dict_index_is_clust(index)
	      || !dict_index_is_online_ddl(index));

	heap_no = page_rec_get_heap_no(rec);

	if (!locked_lock_trx_sys) {
		lock_sys.mutex_lock();
	}

	lock_sys.mutex_assert_locked();

	if (!page_rec_is_user_rec(rec)) {

		for (lock = lock_rec_get_first(&lock_sys.rec_hash,
					       id, heap_no);
		     lock != NULL;
		     lock = lock_rec_get_next_const(heap_no, lock)) {

			ut_ad(!index || lock->index == index);

			lock->trx->mutex.wr_lock();
			ut_ad(!trx_is_ac_nl_ro(lock->trx));
			ut_ad(trx_state_eq(lock->trx,
					   TRX_STATE_COMMITTED_IN_MEMORY)
			      || !lock->is_waiting()
			      || lock_rec_has_to_wait_in_queue(lock));
			lock->trx->mutex.wr_unlock();
		}

func_exit:
		if (!locked_lock_trx_sys) {
			lock_sys.mutex_unlock();
		}

		return true;
	}

	ut_ad(page_rec_is_leaf(rec));
	lock_sys.mutex_assert_locked();

	const trx_id_t impl_trx_id = index && index->is_primary()
		? lock_clust_rec_some_has_impl(rec, index, offsets)
		: 0;

	if (trx_t *impl_trx = impl_trx_id
	    ? trx_sys.find(current_trx(), impl_trx_id, false)
	    : 0) {
		/* impl_trx could have been committed before we
		acquire its mutex, but not thereafter. */

		impl_trx->mutex.wr_lock();
		ut_ad(impl_trx->state != TRX_STATE_NOT_STARTED);
		if (impl_trx->state == TRX_STATE_COMMITTED_IN_MEMORY) {
		} else if (const lock_t* other_lock
			   = lock_rec_other_has_expl_req(
				   LOCK_S, id, true, heap_no, impl_trx)) {
			/* The impl_trx is holding an implicit lock on the
			given record 'rec'. So there cannot be another
			explicit granted lock.  Also, there can be another
			explicit waiting lock only if the impl_trx has an
			explicit granted lock. */

#ifdef WITH_WSREP
			/** Galera record locking rules:
			* If there is no other record lock to the same record, we may grant
			the lock request.
			* If there is other record lock but this requested record lock is
			compatible, we may grant the lock request.
			* If there is other record lock and it is not compatible with
			requested lock, all normal transactions must wait.
			* BF (brute force) additional exceptions :
			** If BF already holds record lock for requested record, we may
			grant new record lock even if there is conflicting record lock(s)
			waiting on a queue.
			** If conflicting transaction holds requested record lock,
			we will cancel this record lock and select conflicting transaction
			for BF abort or kill victim.
			** If conflicting transaction is waiting for requested record lock
			we will cancel this wait and select conflicting transaction
			for BF abort or kill victim.
			** There should not be two BF transactions waiting for same record lock
			*/
			if (other_lock->trx->is_wsrep() && !other_lock->is_waiting()) {
				wsrep_report_bf_lock_wait(impl_trx->mysql_thd, impl_trx->id);
				wsrep_report_bf_lock_wait(other_lock->trx->mysql_thd, other_lock->trx->id);

				if (!lock_rec_has_expl(LOCK_X | LOCK_REC_NOT_GAP,
						       id, heap_no,
						       impl_trx)) {
					ib::info() << "WSREP impl BF lock conflict";
				}
			} else
#endif /* WITH_WSREP */
			{
				ut_ad(other_lock->is_waiting());
				ut_ad(lock_rec_has_expl(LOCK_X | LOCK_REC_NOT_GAP,
						        id, heap_no, impl_trx));
			}
		}

		impl_trx->mutex.wr_unlock();
	}

	for (lock = lock_rec_get_first(&lock_sys.rec_hash, id, heap_no);
	     lock != NULL;
	     lock = lock_rec_get_next_const(heap_no, lock)) {

		ut_ad(!trx_is_ac_nl_ro(lock->trx));
		ut_ad(!page_rec_is_metadata(rec));

		if (index) {
			ut_a(lock->index == index);
		}

		if (lock->is_waiting()) {
			ut_a(lock->is_gap()
			     || lock_rec_has_to_wait_in_queue(lock));
		} else if (!lock->is_gap()) {
			const lock_mode	mode = lock->mode() == LOCK_S
				? LOCK_X : LOCK_S;

			const lock_t*	other_lock
				= lock_rec_other_has_expl_req(
					mode, id, false, heap_no, lock->trx);
#ifdef WITH_WSREP
			if (UNIV_UNLIKELY(other_lock && lock->trx->is_wsrep())) {
				/* Only BF transaction may be granted
				lock before other conflicting lock
				request. */
				if (!wsrep_thd_is_BF(lock->trx->mysql_thd, FALSE)
				    && !wsrep_thd_is_BF(other_lock->trx->mysql_thd, FALSE)) {
					/* If no BF, this case is a bug. */
					wsrep_report_bf_lock_wait(lock->trx->mysql_thd, lock->trx->id);
					wsrep_report_bf_lock_wait(other_lock->trx->mysql_thd, other_lock->trx->id);
					ut_error;
				}
			} else
#endif /* WITH_WSREP */
			ut_ad(!other_lock);
		}
	}

	goto func_exit;
}

/** Validate the record lock queues on a page.
@param block    buffer pool block
@param latched  whether the tablespace latch may be held
@return true if ok */
static bool lock_rec_validate_page(const buf_block_t *block, bool latched)
{
	const lock_t*	lock;
	const rec_t*	rec;
	ulint		nth_lock	= 0;
	ulint		nth_bit		= 0;
	ulint		i;
	mem_heap_t*	heap		= NULL;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets		= offsets_;
	rec_offs_init(offsets_);

	const page_id_t id{block->page.id()};

	LockMutexGuard g;
loop:
	lock = lock_sys.get_first(id);

	if (!lock) {
		goto function_exit;
	}

	DBUG_ASSERT(block->page.status != buf_page_t::FREED);

	for (i = 0; i < nth_lock; i++) {

		lock = lock_rec_get_next_on_page_const(lock);

		if (!lock) {
			goto function_exit;
		}
	}

	ut_ad(!trx_is_ac_nl_ro(lock->trx));

	/* Only validate the record queues when this thread is not
	holding a tablespace latch. */
	if (!latched)
	for (i = nth_bit; i < lock_rec_get_n_bits(lock); i++) {

		if (i == PAGE_HEAP_NO_SUPREMUM
		    || lock_rec_get_nth_bit(lock, i)) {

			rec = page_find_rec_with_heap_no(block->frame, i);
			ut_a(rec);
			ut_ad(!lock_rec_get_nth_bit(lock, i)
			      || page_rec_is_leaf(rec));
			offsets = rec_get_offsets(rec, lock->index, offsets,
						  true, ULINT_UNDEFINED,
						  &heap);

			/* If this thread is holding the file space
			latch (fil_space_t::latch), the following
			check WILL break the latching order and may
			cause a deadlock of threads. */

			lock_rec_queue_validate(
				true, id, rec, lock->index, offsets);

			nth_bit = i + 1;

			goto loop;
		}
	}

	nth_bit = 0;
	nth_lock++;

	goto loop;

function_exit:
	if (heap != NULL) {
		mem_heap_free(heap);
	}
	return(TRUE);
}

/*********************************************************************//**
Validate record locks up to a limit.
@return lock at limit or NULL if no more locks in the hash bucket */
static MY_ATTRIBUTE((warn_unused_result))
const lock_t*
lock_rec_validate(
/*==============*/
	ulint		start,		/*!< in: lock_sys.rec_hash
					bucket */
	page_id_t*	limit)		/*!< in/out: upper limit of
					(space, page_no) */
{
	lock_sys.mutex_assert_locked();

	for (const lock_t* lock = static_cast<const lock_t*>(
		     HASH_GET_FIRST(&lock_sys.rec_hash, start));
	     lock != NULL;
	     lock = static_cast<const lock_t*>(HASH_GET_NEXT(hash, lock))) {

		ut_ad(!trx_is_ac_nl_ro(lock->trx));
		ut_ad(!lock->is_table());

		page_id_t current(lock->un_member.rec_lock.page_id);

		if (current > *limit) {
			*limit = current + 1;
			return(lock);
		}
	}

	return(0);
}

/*********************************************************************//**
Validate a record lock's block */
static void lock_rec_block_validate(const page_id_t page_id)
{
	/* The lock and the block that it is referring to may be freed at
	this point. We pass BUF_GET_POSSIBLY_FREED to skip a debug check.
	If the lock exists in lock_rec_validate_page() we assert
	block->page.status != FREED. */

	buf_block_t*	block;
	mtr_t		mtr;

	/* Transactional locks should never refer to dropped
	tablespaces, because all DDL operations that would drop or
	discard or rebuild a tablespace do hold an exclusive table
	lock, which would conflict with any locks referring to the
	tablespace from other transactions. */
	if (fil_space_t* space = fil_space_t::get(page_id.space())) {
		dberr_t err = DB_SUCCESS;
		mtr_start(&mtr);

		block = buf_page_get_gen(
			page_id,
			space->zip_size(),
			RW_X_LATCH, NULL,
			BUF_GET_POSSIBLY_FREED,
			&mtr, &err);

		if (err != DB_SUCCESS) {
			ib::error() << "Lock rec block validate failed for tablespace "
				   << space->name
				   << page_id << " err " << err;
		}

		ut_ad(!block || lock_rec_validate_page(block,
						       space->is_latched()));

		mtr_commit(&mtr);

		space->release();
	}
}


static my_bool lock_validate_table_locks(rw_trx_hash_element_t *element, void*)
{
  lock_sys.mutex_assert_locked();
  mysql_mutex_lock(&element->mutex);
  if (element->trx)
  {
    check_trx_state(element->trx);
    for (const lock_t *lock= UT_LIST_GET_FIRST(element->trx->lock.trx_locks);
         lock != NULL;
         lock= UT_LIST_GET_NEXT(trx_locks, lock))
      if (lock->is_table())
        lock_table_queue_validate(lock->un_member.tab_lock.table);
  }
  mysql_mutex_unlock(&element->mutex);
  return 0;
}


/*********************************************************************//**
Validates the lock system.
@return TRUE if ok */
static
bool
lock_validate()
/*===========*/
{
	std::set<page_id_t> pages;

	lock_sys.mutex_lock();

	/* Validate table locks */
	trx_sys.rw_trx_hash.iterate(lock_validate_table_locks);

	/* Iterate over all the record locks and validate the locks. We
	don't want to hog the lock_sys_t::mutex. Release it during the
	validation check. */

	for (ulint i = 0; i < lock_sys.rec_hash.n_cells; i++) {
		page_id_t limit(0, 0);

		while (const lock_t* lock = lock_rec_validate(i, &limit)) {
			if (lock_rec_find_set_bit(lock) == ULINT_UNDEFINED) {
				/* The lock bitmap is empty; ignore it. */
				continue;
			}
			pages.insert(lock->un_member.rec_lock.page_id);
		}
	}

	lock_sys.mutex_unlock();

	for (page_id_t page_id : pages) {
		lock_rec_block_validate(page_id);
	}

	return(true);
}
#endif /* UNIV_DEBUG */
/*============ RECORD LOCK CHECKS FOR ROW OPERATIONS ====================*/

/*********************************************************************//**
Checks if locks of other transactions prevent an immediate insert of
a record. If they do, first tests if the query thread should anyway
be suspended for some reason; if not, then puts the transaction and
the query thread to the lock wait state and inserts a waiting request
for a gap x-lock to the lock queue.
@return DB_SUCCESS, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_rec_insert_check_and_lock(
/*===========================*/
	const rec_t*	rec,	/*!< in: record after which to insert */
	buf_block_t*	block,	/*!< in/out: buffer block of rec */
	dict_index_t*	index,	/*!< in: index */
	que_thr_t*	thr,	/*!< in: query thread */
	mtr_t*		mtr,	/*!< in/out: mini-transaction */
	bool*		inherit)/*!< out: set to true if the new
				inserted record maybe should inherit
				LOCK_GAP type locks from the successor
				record */
{
	ut_ad(block->frame == page_align(rec));
	ut_ad(mtr->is_named_space(index->table->space));
	ut_ad(page_rec_is_leaf(rec));

	ut_ad(!index->table->is_temporary());
	ut_ad(page_is_leaf(block->frame));

	dberr_t		err;
	lock_t*		lock;
	bool		inherit_in = *inherit;
	trx_t*		trx = thr_get_trx(thr);
	const rec_t*	next_rec = page_rec_get_next_const(rec);
	ulint		heap_no = page_rec_get_heap_no(next_rec);
	ut_ad(!rec_is_metadata(next_rec, *index));

	const page_id_t id{block->page.id()};

	lock_sys.mutex_lock();
	/* Because this code is invoked for a running transaction by
	the thread that is serving the transaction, it is not necessary
	to hold trx->mutex here. */

	/* When inserting a record into an index, the table must be at
	least IX-locked. When we are building an index, we would pass
	BTR_NO_LOCKING_FLAG and skip the locking altogether. */
	ut_ad(lock_table_has(trx, index->table, LOCK_IX));

	lock = lock_rec_get_first(&lock_sys.rec_hash, id, heap_no);

	if (lock == NULL) {
		/* We optimize CPU time usage in the simplest case */

		lock_sys.mutex_unlock();

		if (inherit_in && !dict_index_is_clust(index)) {
			/* Update the page max trx id field */
			page_update_max_trx_id(block,
					       buf_block_get_page_zip(block),
					       trx->id, mtr);
		}

		*inherit = false;

		return(DB_SUCCESS);
	}

	/* Spatial index does not use GAP lock protection. It uses
	"predicate lock" to protect the "range" */
	if (dict_index_is_spatial(index)) {
		return(DB_SUCCESS);
	}

	*inherit = true;

	/* If another transaction has an explicit lock request which locks
	the gap, waiting or granted, on the successor, the insert has to wait.

	An exception is the case where the lock by the another transaction
	is a gap type lock which it placed to wait for its turn to insert. We
	do not consider that kind of a lock conflicting with our insert. This
	eliminates an unnecessary deadlock which resulted when 2 transactions
	had to wait for their insert. Both had waiting gap type lock requests
	on the successor, which produced an unnecessary deadlock. */

	const unsigned	type_mode = LOCK_X | LOCK_GAP | LOCK_INSERT_INTENTION;

	if (lock_t* c_lock =
	    lock_rec_other_has_conflicting(type_mode, id, heap_no, trx)) {
		trx->mutex.wr_lock();

		err = lock_rec_enqueue_waiting(
			c_lock, type_mode, id, block->frame, heap_no, index,
			thr, nullptr);

		trx->mutex.wr_unlock();
	} else {
		err = DB_SUCCESS;
	}

	lock_sys.mutex_unlock();

	switch (err) {
	case DB_SUCCESS_LOCKED_REC:
		err = DB_SUCCESS;
		/* fall through */
	case DB_SUCCESS:
		if (!inherit_in || dict_index_is_clust(index)) {
			break;
		}

		/* Update the page max trx id field */
		page_update_max_trx_id(
			block, buf_block_get_page_zip(block), trx->id, mtr);
	default:
		/* We only care about the two return values. */
		break;
	}

#ifdef UNIV_DEBUG
	{
		mem_heap_t*	heap		= NULL;
		rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
		const rec_offs*	offsets;
		rec_offs_init(offsets_);

		offsets = rec_get_offsets(next_rec, index, offsets_, true,
					  ULINT_UNDEFINED, &heap);

		ut_ad(lock_rec_queue_validate(false, id, next_rec, index, offsets));

		if (heap != NULL) {
			mem_heap_free(heap);
		}
	}
#endif /* UNIV_DEBUG */

	return(err);
}

/*********************************************************************//**
Creates an explicit record lock for a running transaction that currently only
has an implicit lock on the record. The transaction instance must have a
reference count > 0 so that it can't be committed and freed before this
function has completed. */
static
void
lock_rec_convert_impl_to_expl_for_trx(
/*==================================*/
	const page_id_t		id,	/*!< in: page identifier */
	const rec_t*		rec,	/*!< in: user record on page */
	dict_index_t*		index,	/*!< in: index of record */
	trx_t*			trx,	/*!< in/out: active transaction */
	ulint			heap_no)/*!< in: rec heap number to lock */
{
  ut_ad(trx->is_referenced());
  ut_ad(page_rec_is_leaf(rec));
  ut_ad(!rec_is_metadata(rec, *index));

  DEBUG_SYNC_C("before_lock_rec_convert_impl_to_expl_for_trx");
  {
    LockMutexGuard g;
    trx->mutex.wr_lock();
    ut_ad(!trx_state_eq(trx, TRX_STATE_NOT_STARTED));

    if (!trx_state_eq(trx, TRX_STATE_COMMITTED_IN_MEMORY) &&
        !lock_rec_has_expl(LOCK_X | LOCK_REC_NOT_GAP, id, heap_no, trx))
      lock_rec_add_to_queue(LOCK_X | LOCK_REC_NOT_GAP, id, page_align(rec),
                            heap_no, index, trx, true);
  }

  trx->mutex.wr_unlock();
  trx->release_reference();

  DEBUG_SYNC_C("after_lock_rec_convert_impl_to_expl_for_trx");
}


#ifdef UNIV_DEBUG
struct lock_rec_other_trx_holds_expl_arg
{
  const ulint heap_no;
  const page_id_t id;
  const trx_t &impl_trx;
};


static my_bool lock_rec_other_trx_holds_expl_callback(
  rw_trx_hash_element_t *element,
  lock_rec_other_trx_holds_expl_arg *arg)
{
  mysql_mutex_lock(&element->mutex);
  if (element->trx)
  {
    element->trx->mutex.wr_lock();
    ut_ad(element->trx->state != TRX_STATE_NOT_STARTED);
    lock_t *expl_lock= element->trx->state == TRX_STATE_COMMITTED_IN_MEMORY
      ? nullptr
      : lock_rec_has_expl(LOCK_S | LOCK_REC_NOT_GAP,
                          arg->id, arg->heap_no, element->trx);
    /*
      An explicit lock is held by trx other than the trx holding the implicit
      lock.
    */
    ut_ad(!expl_lock || expl_lock->trx == &arg->impl_trx);
    element->trx->mutex.wr_unlock();
  }
  mysql_mutex_unlock(&element->mutex);
  return 0;
}


/**
  Checks if some transaction, other than given trx_id, has an explicit
  lock on the given rec.

  FIXME: if the current transaction holds implicit lock from INSERT, a
  subsequent locking read should not convert it to explicit. See also
  MDEV-11215.

  @param      caller_trx  trx of current thread
  @param[in]  trx         trx holding implicit lock on rec
  @param[in]  rec         user record
  @param[in]  id          page identifier
*/

static void lock_rec_other_trx_holds_expl(trx_t *caller_trx, trx_t *trx,
                                          const rec_t *rec,
                                          const page_id_t id)
{
  if (trx)
  {
    ut_ad(!page_rec_is_metadata(rec));
    LockMutexGuard g;
    ut_ad(trx->is_referenced());
    const trx_state_t state{trx->state};
    ut_ad(state != TRX_STATE_NOT_STARTED);
    if (state == TRX_STATE_COMMITTED_IN_MEMORY)
      /* The transaction was committed before we acquired LockMutexGuard. */
      return;

    lock_rec_other_trx_holds_expl_arg arg= { page_rec_get_heap_no(rec), id,
                                             *trx };
    trx_sys.rw_trx_hash.iterate(caller_trx,
                                lock_rec_other_trx_holds_expl_callback, &arg);
  }
}
#endif /* UNIV_DEBUG */


/** If an implicit x-lock exists on a record, convert it to an explicit one.

Often, this is called by a transaction that is about to enter a lock wait
due to the lock conflict. Two explicit locks would be created: first the
exclusive lock on behalf of the lock-holder transaction in this function,
and then a wait request on behalf of caller_trx, in the calling function.

This may also be called by the same transaction that is already holding
an implicit exclusive lock on the record. In this case, no explicit lock
should be created.

@param[in,out]	caller_trx	current transaction
@param[in]	id		index tree leaf page identifier
@param[in]	rec		record on the leaf page
@param[in]	index		the index of the record
@param[in]	offsets		rec_get_offsets(rec,index)
@return	whether caller_trx already holds an exclusive lock on rec */
static
bool
lock_rec_convert_impl_to_expl(
	trx_t*			caller_trx,
	page_id_t		id,
	const rec_t*		rec,
	dict_index_t*		index,
	const rec_offs*		offsets)
{
	trx_t*		trx;

	lock_sys.mutex_assert_unlocked();
	ut_ad(page_rec_is_user_rec(rec));
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(!page_rec_is_comp(rec) == !rec_offs_comp(offsets));
	ut_ad(page_rec_is_leaf(rec));
	ut_ad(!rec_is_metadata(rec, *index));

	if (dict_index_is_clust(index)) {
		trx_id_t	trx_id;

		trx_id = lock_clust_rec_some_has_impl(rec, index, offsets);

		if (trx_id == 0) {
			return false;
		}
		if (UNIV_UNLIKELY(trx_id == caller_trx->id)) {
			return true;
		}

		trx = trx_sys.find(caller_trx, trx_id);
	} else {
		ut_ad(!dict_index_is_online_ddl(index));

		trx = lock_sec_rec_some_has_impl(caller_trx, rec, index,
						 offsets);
		if (trx == caller_trx) {
			trx->release_reference();
			return true;
		}

		ut_d(lock_rec_other_trx_holds_expl(caller_trx, trx, rec, id));
	}

	if (trx) {
		ulint	heap_no = page_rec_get_heap_no(rec);

		ut_ad(trx->is_referenced());

		/* If the transaction is still active and has no
		explicit x-lock set on the record, set one for it.
		trx cannot be committed until the ref count is zero. */

		lock_rec_convert_impl_to_expl_for_trx(
			id, rec, index, trx, heap_no);
	}

	return false;
}

/*********************************************************************//**
Checks if locks of other transactions prevent an immediate modify (update,
delete mark, or delete unmark) of a clustered index record. If they do,
first tests if the query thread should anyway be suspended for some
reason; if not, then puts the transaction and the query thread to the
lock wait state and inserts a waiting request for a record x-lock to the
lock queue.
@return DB_SUCCESS, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_clust_rec_modify_check_and_lock(
/*=================================*/
	const buf_block_t*	block,	/*!< in: buffer block of rec */
	const rec_t*		rec,	/*!< in: record which should be
					modified */
	dict_index_t*		index,	/*!< in: clustered index */
	const rec_offs*		offsets,/*!< in: rec_get_offsets(rec, index) */
	que_thr_t*		thr)	/*!< in: query thread */
{
	dberr_t	err;
	ulint	heap_no;

	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(page_rec_is_leaf(rec));
	ut_ad(dict_index_is_clust(index));
	ut_ad(block->frame == page_align(rec));

	ut_ad(!rec_is_metadata(rec, *index));
	ut_ad(!index->table->is_temporary());

	heap_no = rec_offs_comp(offsets)
		? rec_get_heap_no_new(rec)
		: rec_get_heap_no_old(rec);

	/* If a transaction has no explicit x-lock set on the record, set one
	for it */

	if (lock_rec_convert_impl_to_expl(thr_get_trx(thr), block->page.id(),
					  rec, index, offsets)) {
		/* We already hold an implicit exclusive lock. */
		return DB_SUCCESS;
	}

	err = lock_rec_lock(true, LOCK_X | LOCK_REC_NOT_GAP,
			    block, heap_no, index, thr);

	ut_ad(lock_rec_queue_validate(false, block->page.id(),
				      rec, index, offsets));

	if (err == DB_SUCCESS_LOCKED_REC) {
		err = DB_SUCCESS;
	}

	return(err);
}

/*********************************************************************//**
Checks if locks of other transactions prevent an immediate modify (delete
mark or delete unmark) of a secondary index record.
@return DB_SUCCESS, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_sec_rec_modify_check_and_lock(
/*===============================*/
	ulint		flags,	/*!< in: if BTR_NO_LOCKING_FLAG
				bit is set, does nothing */
	buf_block_t*	block,	/*!< in/out: buffer block of rec */
	const rec_t*	rec,	/*!< in: record which should be
				modified; NOTE: as this is a secondary
				index, we always have to modify the
				clustered index record first: see the
				comment below */
	dict_index_t*	index,	/*!< in: secondary index */
	que_thr_t*	thr,	/*!< in: query thread
				(can be NULL if BTR_NO_LOCKING_FLAG) */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	dberr_t	err;
	ulint	heap_no;

	ut_ad(!dict_index_is_clust(index));
	ut_ad(!dict_index_is_online_ddl(index) || (flags & BTR_CREATE_FLAG));
	ut_ad(block->frame == page_align(rec));
	ut_ad(mtr->is_named_space(index->table->space));
	ut_ad(page_rec_is_leaf(rec));
	ut_ad(!rec_is_metadata(rec, *index));

	if (flags & BTR_NO_LOCKING_FLAG) {

		return(DB_SUCCESS);
	}
	ut_ad(!index->table->is_temporary());

	heap_no = page_rec_get_heap_no(rec);

	/* Another transaction cannot have an implicit lock on the record,
	because when we come here, we already have modified the clustered
	index record, and this would not have been possible if another active
	transaction had modified this secondary index record. */

	err = lock_rec_lock(true, LOCK_X | LOCK_REC_NOT_GAP,
			    block, heap_no, index, thr);

#ifdef UNIV_DEBUG
	{
		mem_heap_t*	heap		= NULL;
		rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
		const rec_offs*	offsets;
		rec_offs_init(offsets_);

		offsets = rec_get_offsets(rec, index, offsets_, true,
					  ULINT_UNDEFINED, &heap);

		ut_ad(lock_rec_queue_validate(
			      false, block->page.id(), rec, index, offsets));

		if (heap != NULL) {
			mem_heap_free(heap);
		}
	}
#endif /* UNIV_DEBUG */

	if (err == DB_SUCCESS || err == DB_SUCCESS_LOCKED_REC) {
		/* Update the page max trx id field */
		/* It might not be necessary to do this if
		err == DB_SUCCESS (no new lock created),
		but it should not cost too much performance. */
		page_update_max_trx_id(block,
				       buf_block_get_page_zip(block),
				       thr_get_trx(thr)->id, mtr);
		err = DB_SUCCESS;
	}

	return(err);
}

/*********************************************************************//**
Like lock_clust_rec_read_check_and_lock(), but reads a
secondary index record.
@return DB_SUCCESS, DB_SUCCESS_LOCKED_REC, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_sec_rec_read_check_and_lock(
/*=============================*/
	ulint			flags,	/*!< in: if BTR_NO_LOCKING_FLAG
					bit is set, does nothing */
	const buf_block_t*	block,	/*!< in: buffer block of rec */
	const rec_t*		rec,	/*!< in: user record or page
					supremum record which should
					be read or passed over by a
					read cursor */
	dict_index_t*		index,	/*!< in: secondary index */
	const rec_offs*		offsets,/*!< in: rec_get_offsets(rec, index) */
	lock_mode		mode,	/*!< in: mode of the lock which
					the read cursor should set on
					records: LOCK_S or LOCK_X; the
					latter is possible in
					SELECT FOR UPDATE */
	unsigned		gap_mode,/*!< in: LOCK_ORDINARY, LOCK_GAP, or
					LOCK_REC_NOT_GAP */
	que_thr_t*		thr)	/*!< in: query thread */
{
	dberr_t	err;
	ulint	heap_no;

	ut_ad(!dict_index_is_clust(index));
	ut_ad(!dict_index_is_online_ddl(index));
	ut_ad(block->frame == page_align(rec));
	ut_ad(page_rec_is_user_rec(rec) || page_rec_is_supremum(rec));
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(page_rec_is_leaf(rec));
	ut_ad(mode == LOCK_X || mode == LOCK_S);

	if ((flags & BTR_NO_LOCKING_FLAG)
	    || srv_read_only_mode
	    || index->table->is_temporary()) {

		return(DB_SUCCESS);
	}

	const page_id_t id{block->page.id()};

	ut_ad(!rec_is_metadata(rec, *index));
	heap_no = page_rec_get_heap_no(rec);

	/* Some transaction may have an implicit x-lock on the record only
	if the max trx id for the page >= min trx id for the trx list or a
	database recovery is running. */

	if (!page_rec_is_supremum(rec)
	    && page_get_max_trx_id(block->frame) >= trx_sys.get_min_trx_id()
	    && lock_rec_convert_impl_to_expl(thr_get_trx(thr), id, rec,
					     index, offsets)) {
		/* We already hold an implicit exclusive lock. */
		return DB_SUCCESS;
	}

	err = lock_rec_lock(false, gap_mode | mode,
			    block, heap_no, index, thr);

	ut_ad(lock_rec_queue_validate(false, id, rec, index, offsets));

	return(err);
}

/*********************************************************************//**
Checks if locks of other transactions prevent an immediate read, or passing
over by a read cursor, of a clustered index record. If they do, first tests
if the query thread should anyway be suspended for some reason; if not, then
puts the transaction and the query thread to the lock wait state and inserts a
waiting request for a record lock to the lock queue. Sets the requested mode
lock on the record.
@return DB_SUCCESS, DB_SUCCESS_LOCKED_REC, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_clust_rec_read_check_and_lock(
/*===============================*/
	ulint			flags,	/*!< in: if BTR_NO_LOCKING_FLAG
					bit is set, does nothing */
	const buf_block_t*	block,	/*!< in: buffer block of rec */
	const rec_t*		rec,	/*!< in: user record or page
					supremum record which should
					be read or passed over by a
					read cursor */
	dict_index_t*		index,	/*!< in: clustered index */
	const rec_offs*		offsets,/*!< in: rec_get_offsets(rec, index) */
	lock_mode		mode,	/*!< in: mode of the lock which
					the read cursor should set on
					records: LOCK_S or LOCK_X; the
					latter is possible in
					SELECT FOR UPDATE */
	unsigned		gap_mode,/*!< in: LOCK_ORDINARY, LOCK_GAP, or
					LOCK_REC_NOT_GAP */
	que_thr_t*		thr)	/*!< in: query thread */
{
	dberr_t	err;
	ulint	heap_no;

	ut_ad(dict_index_is_clust(index));
	ut_ad(block->frame == page_align(rec));
	ut_ad(page_rec_is_user_rec(rec) || page_rec_is_supremum(rec));
	ut_ad(gap_mode == LOCK_ORDINARY || gap_mode == LOCK_GAP
	      || gap_mode == LOCK_REC_NOT_GAP);
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(page_rec_is_leaf(rec));
	ut_ad(!rec_is_metadata(rec, *index));

	if ((flags & BTR_NO_LOCKING_FLAG)
	    || srv_read_only_mode
	    || index->table->is_temporary()) {

		return(DB_SUCCESS);
	}

	const page_id_t id{block->page.id()};

	heap_no = page_rec_get_heap_no(rec);

	if (heap_no != PAGE_HEAP_NO_SUPREMUM
	    && lock_rec_convert_impl_to_expl(thr_get_trx(thr), id, rec,
					     index, offsets)) {
		/* We already hold an implicit exclusive lock. */
		return DB_SUCCESS;
	}

	err = lock_rec_lock(false, gap_mode | mode,
			    block, heap_no, index, thr);

	ut_ad(lock_rec_queue_validate(false, id, rec, index, offsets));

	DEBUG_SYNC_C("after_lock_clust_rec_read_check_and_lock");

	return(err);
}
/*********************************************************************//**
Checks if locks of other transactions prevent an immediate read, or passing
over by a read cursor, of a clustered index record. If they do, first tests
if the query thread should anyway be suspended for some reason; if not, then
puts the transaction and the query thread to the lock wait state and inserts a
waiting request for a record lock to the lock queue. Sets the requested mode
lock on the record. This is an alternative version of
lock_clust_rec_read_check_and_lock() that does not require the parameter
"offsets".
@return DB_SUCCESS, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_clust_rec_read_check_and_lock_alt(
/*===================================*/
	ulint			flags,	/*!< in: if BTR_NO_LOCKING_FLAG
					bit is set, does nothing */
	const buf_block_t*	block,	/*!< in: buffer block of rec */
	const rec_t*		rec,	/*!< in: user record or page
					supremum record which should
					be read or passed over by a
					read cursor */
	dict_index_t*		index,	/*!< in: clustered index */
	lock_mode		mode,	/*!< in: mode of the lock which
					the read cursor should set on
					records: LOCK_S or LOCK_X; the
					latter is possible in
					SELECT FOR UPDATE */
	unsigned		gap_mode,/*!< in: LOCK_ORDINARY, LOCK_GAP, or
					LOCK_REC_NOT_GAP */
	que_thr_t*		thr)	/*!< in: query thread */
{
	mem_heap_t*	tmp_heap	= NULL;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets		= offsets_;
	dberr_t		err;
	rec_offs_init(offsets_);

	ut_ad(page_rec_is_leaf(rec));
	offsets = rec_get_offsets(rec, index, offsets, true,
				  ULINT_UNDEFINED, &tmp_heap);
	err = lock_clust_rec_read_check_and_lock(flags, block, rec, index,
						 offsets, mode, gap_mode, thr);
	if (tmp_heap) {
		mem_heap_free(tmp_heap);
	}

	if (err == DB_SUCCESS_LOCKED_REC) {
		err = DB_SUCCESS;
	}

	return(err);
}

/*******************************************************************//**
Release the last lock from the transaction's autoinc locks. */
UNIV_INLINE
void
lock_release_autoinc_last_lock(
/*===========================*/
	ib_vector_t*	autoinc_locks)	/*!< in/out: vector of AUTOINC locks */
{
	lock_t*		lock;

	lock_sys.mutex_assert_locked();

	/* The lock to be release must be the last lock acquired. */
	ulint size = ib_vector_size(autoinc_locks);
	ut_a(size);
	lock = *static_cast<lock_t**>(ib_vector_get(autoinc_locks, size - 1));

	ut_ad(lock->type_mode == (LOCK_AUTO_INC | LOCK_TABLE));
	ut_ad(lock->un_member.tab_lock.table);

	/* This will remove the lock from the trx autoinc_locks too. */
	lock_table_dequeue(lock);

	/* Remove from the table vector too. */
	lock_trx_table_locks_remove(lock);
}

/*******************************************************************//**
Check if a transaction holds any autoinc locks.
@return TRUE if the transaction holds any AUTOINC locks. */
static
ibool
lock_trx_holds_autoinc_locks(
/*=========================*/
	const trx_t*	trx)		/*!< in: transaction */
{
	ut_a(trx->autoinc_locks != NULL);

	return(!ib_vector_is_empty(trx->autoinc_locks));
}

/*******************************************************************//**
Release all the transaction's autoinc locks. */
static
void
lock_release_autoinc_locks(
/*=======================*/
	trx_t*		trx)		/*!< in/out: transaction */
{
	lock_sys.mutex_assert_locked();
	mysql_mutex_assert_owner(&lock_sys.wait_mutex);
	/* If this is invoked for a running transaction by the thread
	that is serving the transaction, then it is not necessary to
	hold trx->mutex here. */

	ut_a(trx->autoinc_locks != NULL);

	/* We release the locks in the reverse order. This is to
	avoid searching the vector for the element to delete at
	the lower level. See (lock_table_remove_low()) for details. */
	while (!ib_vector_is_empty(trx->autoinc_locks)) {

		/* lock_table_remove_low() will also remove the lock from
		the transaction's autoinc_locks vector. */
		lock_release_autoinc_last_lock(trx->autoinc_locks);
	}

	/* Should release all locks. */
	ut_a(ib_vector_is_empty(trx->autoinc_locks));
}

/*******************************************************************//**
Gets the table on which the lock is.
@return table */
UNIV_INLINE
dict_table_t*
lock_get_table(
/*===========*/
	const lock_t*	lock)	/*!< in: lock */
{
  if (lock->is_table())
    return lock->un_member.tab_lock.table;

  ut_ad(lock->index->is_primary() || !dict_index_is_online_ddl(lock->index));
  return lock->index->table;
}

/*******************************************************************//**
Gets the id of the table on which the lock is.
@return id of the table */
table_id_t
lock_get_table_id(
/*==============*/
	const lock_t*	lock)	/*!< in: lock */
{
	dict_table_t* table = lock_get_table(lock);
	ut_ad(!table->is_temporary());
	return(table->id);
}

/** Determine which table a lock is associated with.
@param[in]	lock	the lock
@return name of the table */
const table_name_t&
lock_get_table_name(
	const lock_t*	lock)
{
	return(lock_get_table(lock)->name);
}

/*******************************************************************//**
For a record lock, gets the index on which the lock is.
@return index */
const dict_index_t*
lock_rec_get_index(
/*===============*/
	const lock_t*	lock)	/*!< in: lock */
{
  ut_ad(!lock->is_table());
  ut_ad(dict_index_is_clust(lock->index) ||
        !dict_index_is_online_ddl(lock->index));

  return lock->index;
}

/** Cancel a waiting lock request and release possibly waiting transactions */
void lock_cancel_waiting_and_release(lock_t *lock)
{
  lock_sys.mutex_assert_locked();
  mysql_mutex_assert_owner(&lock_sys.wait_mutex);
  trx_t *trx= lock->trx;
  ut_ad(trx->state == TRX_STATE_ACTIVE);

  trx->lock.cancel= true;

  if (!lock->is_table())
    lock_rec_dequeue_from_page(lock);
  else
  {
    if (trx->autoinc_locks)
      lock_release_autoinc_locks(trx);
    lock_table_dequeue(lock);
    /* Remove the lock from table lock vector too. */
    lock_trx_table_locks_remove(lock);
  }

  /* Reset the wait flag and the back pointer to lock in trx. */
  lock_reset_lock_and_trx_wait(lock);

  lock_wait_end(trx);

  trx->lock.cancel= false;
}

/*********************************************************************//**
Unlocks AUTO_INC type locks that were possibly reserved by a trx. This
function should be called at the the end of an SQL statement, by the
connection thread that owns the transaction (trx->mysql_thd). */
void
lock_unlock_table_autoinc(
/*======================*/
	trx_t*	trx)	/*!< in/out: transaction */
{
	lock_sys.mutex_assert_unlocked();
	ut_ad(!trx->lock.wait_lock);

	/* This can be invoked on NOT_STARTED, ACTIVE, PREPARED,
	but not COMMITTED transactions. */

	ut_ad(trx_state_eq(trx, TRX_STATE_NOT_STARTED)
	      || !trx_state_eq(trx, TRX_STATE_COMMITTED_IN_MEMORY));

	/* This function is invoked for a running transaction by the
	thread that is serving the transaction. Therefore it is not
	necessary to hold trx->mutex here. */

	if (lock_trx_holds_autoinc_locks(trx)) {
		lock_sys.mutex_lock();
		mysql_mutex_lock(&lock_sys.wait_mutex);

		lock_release_autoinc_locks(trx);

		lock_sys.mutex_unlock();
		mysql_mutex_unlock(&lock_sys.wait_mutex);
	}
}

static inline dberr_t lock_trx_handle_wait_low(trx_t* trx)
{
  lock_sys.mutex_assert_locked();
  mysql_mutex_assert_owner(&lock_sys.wait_mutex);

  if (trx->lock.was_chosen_as_deadlock_victim)
    return DB_DEADLOCK;
  if (!trx->lock.wait_lock)
    /* The lock was probably granted before we got here. */
    return DB_SUCCESS;

  lock_cancel_waiting_and_release(trx->lock.wait_lock);
  return DB_LOCK_WAIT;
}

/*********************************************************************//**
Check whether the transaction has already been rolled back because it
was selected as a deadlock victim, or if it has to wait then cancel
the wait lock.
@return DB_DEADLOCK, DB_LOCK_WAIT or DB_SUCCESS */
dberr_t
lock_trx_handle_wait(
/*=================*/
	trx_t*	trx)	/*!< in/out: trx lock state */
{
#ifdef WITH_WSREP
  if (UNIV_UNLIKELY(trx->lock.was_chosen_as_wsrep_victim))
    /* FIXME: we do not hold lock_sys.wait_mutex! */
    return lock_trx_handle_wait_low(trx);
#endif /* WITH_WSREP */
  dberr_t err;
  lock_sys.mutex_lock();
  mysql_mutex_lock(&lock_sys.wait_mutex);
  trx->mutex.wr_lock();
  err= lock_trx_handle_wait_low(trx);
  lock_sys.mutex_unlock();
  mysql_mutex_unlock(&lock_sys.wait_mutex);
  trx->mutex.wr_unlock();
  return err;
}

/*********************************************************************//**
Get the number of locks on a table.
@return number of locks */
ulint
lock_table_get_n_locks(
/*===================*/
	const dict_table_t*	table)	/*!< in: table */
{
	ulint		n_table_locks;

	lock_sys.mutex_lock();

	n_table_locks = UT_LIST_GET_LEN(table->locks);

	lock_sys.mutex_unlock();

	return(n_table_locks);
}

#ifdef UNIV_DEBUG
/**
  Do an exhaustive check for any locks (table or rec) against the table.

  @param[in]  table  check if there are any locks held on records in this table
                     or on the table itself
*/

static my_bool lock_table_locks_lookup(rw_trx_hash_element_t *element,
                                       const dict_table_t *table)
{
  lock_sys.mutex_assert_locked();
  mysql_mutex_lock(&element->mutex);
  if (element->trx)
  {
    element->trx->mutex.wr_lock();
    check_trx_state(element->trx);
    if (element->trx->state != TRX_STATE_COMMITTED_IN_MEMORY)
    {
      for (const lock_t *lock= UT_LIST_GET_FIRST(element->trx->lock.trx_locks);
           lock != NULL;
           lock= UT_LIST_GET_NEXT(trx_locks, lock))
      {
        ut_ad(lock->trx == element->trx);
        if (!lock->is_table())
        {
          ut_ad(lock->index->online_status != ONLINE_INDEX_CREATION ||
                lock->index->is_primary());
          ut_ad(lock->index->table != table);
        }
        else
          ut_ad(lock->un_member.tab_lock.table != table);
      }
    }
    element->trx->mutex.wr_unlock();
  }
  mysql_mutex_unlock(&element->mutex);
  return 0;
}
#endif /* UNIV_DEBUG */

/*******************************************************************//**
Check if there are any locks (table or rec) against table.
@return true if table has either table or record locks. */
bool
lock_table_has_locks(
/*=================*/
	const dict_table_t*	table)	/*!< in: check if there are any locks
					held on records in this table or on the
					table itself */
{
	ibool			has_locks;

	ut_ad(table != NULL);
	lock_sys.mutex_lock();

	has_locks = UT_LIST_GET_LEN(table->locks) > 0 || table->n_rec_locks > 0;

#ifdef UNIV_DEBUG
	if (!has_locks) {
		trx_sys.rw_trx_hash.iterate(lock_table_locks_lookup, table);
	}
#endif /* UNIV_DEBUG */

	lock_sys.mutex_unlock();

	return(has_locks);
}

/*******************************************************************//**
Initialise the table lock list. */
void
lock_table_lock_list_init(
/*======================*/
	table_lock_list_t*	lock_list)	/*!< List to initialise */
{
	UT_LIST_INIT(*lock_list, &lock_table_t::locks);
}

/*******************************************************************//**
Initialise the trx lock list. */
void
lock_trx_lock_list_init(
/*====================*/
	trx_lock_list_t*	lock_list)	/*!< List to initialise */
{
	UT_LIST_INIT(*lock_list, &lock_t::trx_locks);
}


#ifdef UNIV_DEBUG
/*******************************************************************//**
Check if the transaction holds any locks on the sys tables
or its records.
@return the strongest lock found on any sys table or 0 for none */
const lock_t*
lock_trx_has_sys_table_locks(
/*=========================*/
	const trx_t*	trx)	/*!< in: transaction to check */
{
	const lock_t*	strongest_lock = 0;
	lock_mode	strongest = LOCK_NONE;

	lock_sys.mutex_lock();

	const lock_list::const_iterator end = trx->lock.table_locks.end();
	lock_list::const_iterator it = trx->lock.table_locks.begin();

	/* Find a valid mode. Note: ib_vector_size() can be 0. */

	for (/* No op */; it != end; ++it) {
		const lock_t*	lock = *it;

		if (lock != NULL
		    && dict_is_sys_table(lock->un_member.tab_lock.table->id)) {

			strongest = lock->mode();
			ut_ad(strongest != LOCK_NONE);
			strongest_lock = lock;
			break;
		}
	}

	if (strongest == LOCK_NONE) {
		lock_sys.mutex_unlock();
		return(NULL);
	}

	for (/* No op */; it != end; ++it) {
		const lock_t*	lock = *it;

		if (lock == NULL) {
			continue;
		}

		ut_ad(trx == lock->trx);
		ut_ad(lock->is_table());
		ut_ad(lock->un_member.tab_lock.table);

		lock_mode mode = lock->mode();

		if (dict_is_sys_table(lock->un_member.tab_lock.table->id)
		    && lock_mode_stronger_or_eq(mode, strongest)) {

			strongest = mode;
			strongest_lock = lock;
		}
	}

	lock_sys.mutex_unlock();

	return(strongest_lock);
}

/** Check if the transaction holds an explicit exclusive lock on a record.
@param[in]	trx	transaction
@param[in]	table	table
@param[in]	id	leaf page identifier
@param[in]	heap_no	heap number identifying the record
@return whether an explicit X-lock is held */
bool lock_trx_has_expl_x_lock(const trx_t &trx, const dict_table_t &table,
                              page_id_t id, ulint heap_no)
{
  ut_ad(heap_no > PAGE_HEAP_NO_SUPREMUM);
  LockMutexGuard g;
  ut_ad(lock_table_has(&trx, &table, LOCK_IX));
  ut_ad(lock_table_has(&trx, &table, LOCK_X) ||
        lock_rec_has_expl(LOCK_X | LOCK_REC_NOT_GAP, id, heap_no, &trx));
  return true;
}
#endif /* UNIV_DEBUG */

namespace Deadlock
{
  /** rewind(3) the file used for storing the latest detected deadlock and
  print a heading message to stderr if printing of all deadlocks to stderr
  is enabled. */
  static void start_print()
  {
    lock_sys.mutex_assert_locked();

    rewind(lock_latest_err_file);
    ut_print_timestamp(lock_latest_err_file);

    if (srv_print_all_deadlocks)
      ib::info() << "Transactions deadlock detected,"
                    " dumping detailed information.";
  }

  /** Print a message to the deadlock file and possibly to stderr.
  @param msg message to print */
  static void print(const char *msg)
  {
    fputs(msg, lock_latest_err_file);
    if (srv_print_all_deadlocks)
      ib::info() << msg;
  }

  /** Print transaction data to the deadlock file and possibly to stderr.
  @param trx transaction */
  static void print(const trx_t &trx)
  {
    lock_sys.mutex_assert_locked();

    ulint n_rec_locks= trx.lock.n_rec_locks;
    ulint n_trx_locks= UT_LIST_GET_LEN(trx.lock.trx_locks);
    ulint heap_size= mem_heap_get_size(trx.lock.lock_heap);

    trx_print_low(lock_latest_err_file, &trx, 3000,
                  n_rec_locks, n_trx_locks, heap_size);

    if (srv_print_all_deadlocks)
      trx_print_low(stderr, &trx, 3000, n_rec_locks, n_trx_locks, heap_size);
  }

  /** Print lock data to the deadlock file and possibly to stderr.
  @param lock record or table type lock */
  static void print(const lock_t &lock)
  {
    lock_sys.mutex_assert_locked();

    if (!lock.is_table())
    {
      mtr_t mtr;
      lock_rec_print(lock_latest_err_file, &lock, mtr);

      if (srv_print_all_deadlocks)
        lock_rec_print(stderr, &lock, mtr);
    }
    else
    {
      lock_table_print(lock_latest_err_file, &lock);

      if (srv_print_all_deadlocks)
        lock_table_print(stderr, &lock);
    }
  }

  ATTRIBUTE_COLD
  /** Report a deadlock (cycle in the waits-for graph).
  @param trx    transaction waiting for a lock in this thread
  @return the transaction to be rolled back (unless one was committed already)
  @return nullptr if no deadlock */
  static trx_t *report(trx_t *const trx)
  {
    mysql_mutex_unlock(&lock_sys.wait_mutex);

    /* Normally, trx should be a direct part of the deadlock
    cycle. However, if innodb_deadlock_detect had been OFF in the
    past, our trx may be waiting for a lock that is held by a
    participant of a pre-existing deadlock, without being part of the
    deadlock itself. That is, the path to the deadlock may be P-shaped
    instead of O-shaped, with trx being at the foot of the P.

    We will process the entire path leading to a cycle, and we will
    choose the victim (to be aborted) among the cycle. */

    static const char rollback_msg[]= "*** WE ROLL BACK TRANSACTION (%u)\n";
    char buf[9 + sizeof rollback_msg];

    /* trx is owned by this thread; we can safely call these without
    holding any mutex */
    const undo_no_t trx_weight= TRX_WEIGHT(trx) |
      (trx->mysql_thd && thd_has_edited_nontrans_tables(trx->mysql_thd)
       ? 1ULL << 63 : 0);
    trx_t *victim= nullptr;
    undo_no_t victim_weight= ~0ULL;
    unsigned victim_pos= 0, trx_pos= 0;
    {
      unsigned l= 0;
      LockMutexGuard g;
      mysql_mutex_lock(&lock_sys.wait_mutex);
      /* Now that we are holding lock_sys.wait_mutex again, check
      whether a cycle still exists. */
      trx_t *cycle= find_cycle(trx);
      if (!cycle)
        return nullptr; /* One of the transactions was already aborted. */
      for (trx_t *next= cycle;;)
      {
        next= next->lock.wait_trx;
        const undo_no_t next_weight= TRX_WEIGHT(next) |
          (next->mysql_thd && thd_has_edited_nontrans_tables(next->mysql_thd)
           ? 1ULL << 63 : 0);
        if (next_weight < victim_weight)
        {
          victim_weight= next_weight;
          victim= next;
          victim_pos= l;
        }
        if (next == victim)
          trx_pos= l;
        if (next == cycle)
          break;
      }

      if (trx_pos && trx_weight == victim_weight)
      {
        victim= trx;
        victim_pos= trx_pos;
      }

      /* Finally, display the deadlock */
      switch (const auto r= static_cast<enum report>(innodb_deadlock_report)) {
      case REPORT_OFF:
        break;
      case REPORT_BASIC:
      case REPORT_FULL:
        start_print();
        l= 0;

        for (trx_t *next= cycle;;)
        {
          next= next->lock.wait_trx;
          ut_ad(next);
          ut_ad(next->state == TRX_STATE_ACTIVE);
          const lock_t *wait_lock= next->lock.wait_lock;
          ut_ad(wait_lock);
          snprintf(buf, sizeof buf, "\n*** (%u) TRANSACTION:\n", ++l);
          print(buf);
          print(*next);
          print("*** WAITING FOR THIS LOCK TO BE GRANTED:\n");
          print(*wait_lock);
	  if (r == REPORT_BASIC);
          else if (wait_lock->is_table())
          {
            if (const lock_t *lock=
                UT_LIST_GET_FIRST(wait_lock->un_member.tab_lock.table->locks))
            {
              ut_ad(!lock->is_waiting());
              print("*** CONFLICTING WITH:\n");
              do
                print(*lock);
              while ((lock= UT_LIST_GET_NEXT(un_member.tab_lock.locks, lock)) &&
                     !lock->is_waiting());
            }
            else
              ut_ad("no conflicting table lock found" == 0);
          }
          else if (const lock_t *lock=
                   lock_sys.get_first(wait_lock->type_mode & LOCK_PREDICATE
                                      ? lock_sys.prdt_hash : lock_sys.rec_hash,
                                      wait_lock->un_member.rec_lock.page_id))
          {
            const ulint heap_no= lock_rec_find_set_bit(wait_lock);
            if (!lock_rec_get_nth_bit(lock, heap_no))
              lock= lock_rec_get_next_const(heap_no, lock);
            ut_ad(!lock->is_waiting());
            print("*** CONFLICTING WITH:\n");
            do
              print(*lock);
            while ((lock= lock_rec_get_next_const(heap_no, lock)) &&
                   !lock->is_waiting());
          }
          else
            ut_ad("no conflicting record lock found" == 0);

          if (next == cycle)
            break;
        }
        snprintf(buf, sizeof buf, rollback_msg, victim_pos);
        print(buf);
      }

      ut_ad(victim->state == TRX_STATE_ACTIVE);

      if (victim != trx)
      {
        victim->mutex.wr_lock();
        victim->lock.was_chosen_as_deadlock_victim= true;
        lock_cancel_waiting_and_release(victim->lock.wait_lock);
        victim->mutex.wr_unlock();
      }
#ifdef WITH_WSREP
      if (victim->is_wsrep() && wsrep_thd_is_SR(victim->mysql_thd))
        wsrep_handle_SR_rollback(trx->mysql_thd, victim->mysql_thd);
#endif
    }

    return victim;
  }
}

/** Check if a lock request results in a deadlock.
Resolve a deadlock by choosing a transaction that will be rolled back.
@param trx    transaction requesting a lock
@return whether trx must report DB_DEADLOCK */
static bool Deadlock::check_and_resolve(trx_t *trx)
{
  ut_ad(trx->state == TRX_STATE_ACTIVE);
  ut_ad(!srv_read_only_mode);

  mysql_mutex_lock(&lock_sys.wait_mutex);

  if (!innodb_deadlock_detect)
    return false;

  if (UNIV_LIKELY(!find_cycle(trx)))
    return trx->lock.was_chosen_as_deadlock_victim;

  return report(trx) == trx || trx->lock.was_chosen_as_deadlock_victim;
}

/*************************************************************//**
Updates the lock table when a page is split and merged to
two pages. */
UNIV_INTERN
void
lock_update_split_and_merge(
	const buf_block_t* left_block,	/*!< in: left page to which merged */
	const rec_t* orig_pred,		/*!< in: original predecessor of
					supremum on the left page before merge*/
	const buf_block_t* right_block)	/*!< in: right page from which merged */
{
	const rec_t* left_next_rec;

	ut_ad(page_is_leaf(left_block->frame));
	ut_ad(page_is_leaf(right_block->frame));
	ut_ad(page_align(orig_pred) == left_block->frame);

	const page_id_t l{left_block->page.id()};
	const page_id_t r{right_block->page.id()};
	LockMutexGuard g;

	left_next_rec = page_rec_get_next_const(orig_pred);
	ut_ad(!page_rec_is_metadata(left_next_rec));

	/* Inherit the locks on the supremum of the left page to the
	first record which was moved from the right page */
	lock_rec_inherit_to_gap(
		*left_block, l,
		page_rec_get_heap_no(left_next_rec),
		PAGE_HEAP_NO_SUPREMUM);

	/* Reset the locks on the supremum of the left page,
	releasing waiting transactions */
	lock_rec_reset_and_release_wait(l, PAGE_HEAP_NO_SUPREMUM);

	/* Inherit the locks to the supremum of the left page from the
	successor of the infimum on the right page */
	lock_rec_inherit_to_gap(*left_block, r,
				PAGE_HEAP_NO_SUPREMUM,
				lock_get_min_heap_no(right_block));
}
