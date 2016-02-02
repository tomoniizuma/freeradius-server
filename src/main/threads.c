/*
 * threads.c	request threading support
 *
 * Version:	$Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2000,2006  The FreeRADIUS server project
 * Copyright 2000  Alan DeKok <aland@ox.org>
 */

RCSID("$Id$")
USES_APPLE_DEPRECATED_API	/* OpenSSL API has been deprecated by Apple */

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/process.h>
#include <freeradius-devel/heap.h>
#include <freeradius-devel/rad_assert.h>

/*
 *	Other OS's have sem_init, OS X doesn't.
 */
#ifdef HAVE_SEMAPHORE_H
#include <semaphore.h>
#endif

#ifdef __APPLE__
#  ifdef WITH_GCD
#    include <dispatch/dispatch.h>
#  endif
#  include <mach/task.h>
#  include <mach/mach_init.h>
#  include <mach/semaphore.h>

#  ifndef WITH_GCD
#    undef sem_t
#    define sem_t semaphore_t
#    undef sem_init
#    define sem_init(s,p,c) semaphore_create(mach_task_self(),s,SYNC_POLICY_FIFO,c)
#    undef sem_wait
#    define sem_wait(s) semaphore_wait(*s)
#    undef sem_post
#    define sem_post(s) semaphore_signal(*s)
#  endif	/* WITH_GCD */
#endif	/* __APPLE__ */

#ifdef HAVE_SYS_WAIT_H
#  include <sys/wait.h>
#endif

#ifdef HAVE_PTHREAD_H

#ifdef HAVE_OPENSSL_CRYPTO_H
#  include <openssl/crypto.h>
#endif
#  ifdef HAVE_OPENSSL_ERR_H
#    include <openssl/err.h>
#  endif
#ifdef HAVE_OPENSSL_EVP_H
#  include <openssl/evp.h>
#endif

#ifdef HAVE_GPERFTOOLS_PROFILER_H
#  include <gperftools/profiler.h>
#endif

#ifndef WITH_GCD
#  define SEMAPHORE_LOCKED	(0)

/*
 *	Threads start off in the idle list.
 *
 *	When a packet comes in, the first thread in the idle list is
 *	assigned the request, and is moved to the head of the active
 *	list.  When the thread is done processing the request, it
 *	removes itself from the active list, and adds itself to the
 *	HEAD of the idle list.  This ensures that "hot" threads
 *	continue to get scheduled, and "cold" threads age out of the
 *	CPU cache.
 *
 *	When the server is reaching overload, there are no threads in
 *	the idle queue.  In that case, the request is added to the
 *	idle_heap.  Any active threads will check the heap FIRST,
 *	before moving themselves to the idle list as described above.
 *	If there are requests in the heap, the thread stays in the
 *	active list, and processes the packet.
 *
 *	Once a second, a random one of the worker threads will manage
 *	the thread pool.  This work involves spawning more threads if
 *	needed, marking old "idle" threads as cancelled, etc.  That
 *	work is done with the mutex released (if at all possible).
 *	This practice minimizes contention on the mutex.
 */
#  define THREAD_IDLE		(1)
#  define THREAD_ACTIVE		(2)
#  define THREAD_CANCELLED	(3)
#  define THREAD_EXITED		(4)

/*
 *  A data structure which contains the information about
 *  the current thread.
 */
typedef struct THREAD_HANDLE {
	struct THREAD_HANDLE	*prev;		//!< Previous thread handle (in the linked list).
	struct THREAD_HANDLE	*next;		//!< Next thread handle (int the linked list).

	pthread_t		pthread_id;	//!< pthread_id.
	int			thread_num;	//!< Server thread number, 1...number of threads.
	int			status;		//!< Is the thread running or exited?
	unsigned int		request_count;	//!< The number of requests that this thread has handled.
	time_t			timestamp;	//!< When the thread started executing.
	REQUEST			*request;

	sem_t			semaphore;	//!< used to signal the thread when there are new requests
} THREAD_HANDLE;

#endif	/* WITH_GCD */

typedef struct thread_fork_t {
	pid_t		pid;
	int		status;
	int		exited;
} thread_fork_t;


#ifdef WITH_STATS
typedef struct fr_pps_t {
	uint32_t	pps_old;
	uint32_t	pps_now;
	uint32_t	pps;
	time_t		time_old;
} fr_pps_t;
#endif


/*
 *	A data structure to manage the thread pool.  There's no real
 *	need for a data structure, but it makes things conceptually
 *	easier.
 */
typedef struct THREAD_POOL {
	bool		spawn_workers;

#ifdef WNOHANG
	pthread_mutex_t	wait_mutex;
	fr_hash_table_t *waiters;
#endif

#ifdef WITH_GCD
	dispatch_queue_t	queue;
#else
	uint32_t	max_thread_num;
	uint32_t	start_threads;
	uint32_t	max_threads;
	uint32_t	min_spare_threads;
	uint32_t	max_spare_threads;
	uint32_t	max_requests_per_thread;
	uint32_t	request_count;
	time_t		time_last_spawned;
	uint32_t	cleanup_delay;
	bool		stop_flag;

#ifdef WITH_STATS
	fr_pps_t	pps_in, pps_out;
#ifdef WITH_ACCOUNTING
	bool		auto_limit_acct;
#endif
#endif

	char const	*queue_priority;

	/*
	 *	To ensure only one thread at a time touches the scheduler.
	 *
	 *	Threads start off on the idle list.  As packets come
	 *	in, the threads go to the active list.  If the idle
	 *	list is empty, packets go to the idle_heap.
	 */
	pthread_mutex_t	mutex;

	bool		spawning;
	time_t		managed;

	uint32_t	max_queue_size;
	uint32_t	num_queued;

	uint32_t        requests;

	fr_heap_cmp_t	heap_cmp;

	uint32_t	total_threads;
	uint32_t	active_threads;
	uint32_t	idle_threads;
	uint32_t	exited_threads;

	fr_heap_t	*idle_heap;

	THREAD_HANDLE	*idle_head;
	THREAD_HANDLE	*idle_tail;

	THREAD_HANDLE	*active_head;
	THREAD_HANDLE	*active_tail;

	THREAD_HANDLE	*exited_head;
	THREAD_HANDLE	*exited_tail;
#endif	/* WITH_GCD */
} THREAD_POOL;

static THREAD_POOL thread_pool;
static bool pool_initialized = false;

#ifndef WITH_GCD
static void thread_pool_manage(time_t now);
#endif

#ifndef WITH_GCD
/*
 *	A mapping of configuration file names to internal integers
 */
static const CONF_PARSER thread_config[] = {
	{ FR_CONF_POINTER("start_servers", PW_TYPE_INTEGER, &thread_pool.start_threads), .dflt = "5" },
	{ FR_CONF_POINTER("max_servers", PW_TYPE_INTEGER, &thread_pool.max_threads), .dflt = "32" },
	{ FR_CONF_POINTER("min_spare_servers", PW_TYPE_INTEGER, &thread_pool.min_spare_threads), .dflt = "3" },
	{ FR_CONF_POINTER("max_spare_servers", PW_TYPE_INTEGER, &thread_pool.max_spare_threads), .dflt = "10" },
	{ FR_CONF_POINTER("max_requests_per_server", PW_TYPE_INTEGER, &thread_pool.max_requests_per_thread), .dflt = "0" },
	{ FR_CONF_POINTER("cleanup_delay", PW_TYPE_INTEGER, &thread_pool.cleanup_delay), .dflt = "5" },
	{ FR_CONF_POINTER("max_queue_size", PW_TYPE_INTEGER, &thread_pool.max_queue_size), .dflt = "65536" },
	{ FR_CONF_POINTER("queue_priority", PW_TYPE_STRING, &thread_pool.queue_priority), .dflt = NULL },
#ifdef WITH_STATS
#ifdef WITH_ACCOUNTING
	{ FR_CONF_POINTER("auto_limit_acct", PW_TYPE_BOOLEAN, &thread_pool.auto_limit_acct) },
#endif
#endif
	CONF_PARSER_TERMINATOR
};
#endif

#ifdef WNOHANG
/*
 *	We don't want to catch SIGCHLD for a host of reasons.
 *
 *	- exec_wait means that someone, somewhere, somewhen, will
 *	call waitpid(), and catch the child.
 *
 *	- SIGCHLD is delivered to a random thread, not the one that
 *	forked.
 *
 *	- if another thread catches the child, we have to coordinate
 *	with the thread doing the waiting.
 *
 *	- if we don't waitpid() for non-wait children, they'll be zombies,
 *	and will hang around forever.
 *
 */
static void reap_children(void)
{
	pid_t pid;
	int status;
	thread_fork_t mytf, *tf;


	pthread_mutex_lock(&thread_pool.wait_mutex);

	do {
	retry:
		pid = waitpid(0, &status, WNOHANG);
		if (pid <= 0) break;

		mytf.pid = pid;
		tf = fr_hash_table_finddata(thread_pool.waiters, &mytf);
		if (!tf) goto retry;

		tf->status = status;
		tf->exited = 1;
	} while (fr_hash_table_num_elements(thread_pool.waiters) > 0);

	pthread_mutex_unlock(&thread_pool.wait_mutex);
}
#else
#  define reap_children()
#endif /* WNOHANG */

#ifndef WITH_GCD
static REQUEST *request_dequeue(void);

/*
 *	Add a request to the list of waiting requests.
 *	This function gets called ONLY from the main handler thread...
 *
 *	This function should never fail.
 */
int request_enqueue(REQUEST *request)
{
	THREAD_HANDLE *thread;

	request->component = "<core>";
	request->module = "<queue>";
	request->child_state = REQUEST_QUEUED;

	/*
	 *	Give the request to a thread, doing as little work as
	 *	possible in the contended region.
	 */
	pthread_mutex_lock(&thread_pool.mutex);

	/*
	 *	If we're too busy, don't do anything.
	 */
	if ((thread_pool.num_queued + 1) >= thread_pool.max_queue_size) {
		pthread_mutex_unlock(&thread_pool.mutex);

		/*
		 *	Mark the request as done.
		 */
		RATE_LIMIT(ERROR("Something is blocking the server.  There are %d packets in the queue, "
				 "waiting to be processed.  Ignoring the new request.", thread_pool.max_queue_size));

	done:
		request->module = "<done>";
		request->child_state = REQUEST_DONE;
		return 0;
	}

#ifdef WITH_STATS
#ifdef WITH_ACCOUNTING
	if (thread_pool.auto_limit_acct) {
		struct timeval now;

		/*
		 *	Throw away accounting requests if we're too
		 *	busy.  The NAS should retransmit these, and no
		 *	one should notice.
		 *
		 *	In contrast, we always try to process
		 *	authentication requests.  Those are more time
		 *	critical, and it's harder to determine which
		 *	we can throw away, and which we can keep.
		 *
		 *	We allow the queue to get half full before we
		 *	start worrying.  Even then, we still require
		 *	that the rate of input packets is higher than
		 *	the rate of outgoing packets.  i.e. the queue
		 *	is growing.
		 *
		 *	Once that happens, we roll a dice to see where
		 *	the barrier is for "keep" versus "toss".  If
		 *	the queue is smaller than the barrier, we
		 *	allow it.  If the queue is larger than the
		 *	barrier, we throw the packet away.  Otherwise,
		 *	we keep it.
		 *
		 *	i.e. the probability of throwing the packet
		 *	away increases from 0 (queue is half full), to
		 *	100 percent (queue is completely full).
		 *
		 *	A probabilistic approach allows us to process
		 *	SOME of the new accounting packets.
		 */
		if ((request->packet->code == PW_CODE_ACCOUNTING_REQUEST) &&
		    (thread_pool.num_queued > (thread_pool.max_queue_size / 2)) &&
		    (thread_pool.pps_in.pps_now > thread_pool.pps_out.pps_now)) {
			uint32_t prob;
			uint32_t keep;

			/*
			 *	Take a random value of how full we
			 *	want the queue to be.  It's OK to be
			 *	half full, but we get excited over
			 *	anything more than that.
			 */
			keep = (thread_pool.max_queue_size / 2);
			prob = fr_rand() & ((1 << 10) - 1);
			keep *= prob;
			keep >>= 10;
			keep += (thread_pool.max_queue_size / 2);

			/*
			 *	If the queue is larger than our dice
			 *	roll, we throw the packet away.
			 */
			if (thread_pool.num_queued > keep) {
				pthread_mutex_unlock(&thread_pool.mutex);
				goto done;
			}
		}

		gettimeofday(&now, NULL);

		/*
		 *	Calculate the instantaneous arrival rate into
		 *	the queue.
		 */
		thread_pool.pps_in.pps = rad_pps(&thread_pool.pps_in.pps_old,
						 &thread_pool.pps_in.pps_now,
						 &thread_pool.pps_in.time_old,
						 &now);

		thread_pool.pps_in.pps_now++;
	}
#endif	/* WITH_ACCOUNTING */
#endif

	/*
	 *	If there's a queue, OR no idle threads, put the
	 *	request into the queue, in priority order.
	 *
	 *	@fixme: warn of blocked threads here, instead of in
	 *	request_dequeue()
	 */
	if (thread_pool.num_queued || !thread_pool.idle_head) {
		if (!fr_heap_insert(thread_pool.idle_heap, request)) {
			pthread_mutex_unlock(&thread_pool.mutex);
			goto done;
		}

		thread_pool.num_queued++;

		if (!thread_pool.idle_head) {
			pthread_mutex_unlock(&thread_pool.mutex);
			return 1;
		}

		/*
		 *	Else there is an idle thread.  Pop a request
		 *	off of the top of the heap, and pass it to the
		 *	idle thread.
		 */
		thread = thread_pool.idle_head;
		request = request_dequeue();
		if (!request) {
			pthread_mutex_unlock(&thread_pool.mutex);
			return 1;
		}

	} else {
		/*
		 *	Grab the first idle thread.
		 */
		thread = thread_pool.idle_head;
		rad_assert(thread->status == THREAD_IDLE);
	}

	/*
	 *	Remove the thread from the head of the idle list.
	 */
	rad_assert(thread->prev == NULL);

	thread_pool.idle_head = thread->next;
	if (thread->next) {
		thread->next->prev = NULL;
	} else {
		rad_assert(thread_pool.idle_tail == thread);
		thread_pool.idle_tail = thread->prev;
		rad_assert(thread_pool.idle_threads == 1);
	}
	thread_pool.idle_threads--;

	/*
	 *	Move the thread to the head of the active list.
	 */
	thread->next = thread_pool.active_head;
	if (thread->next) {
		rad_assert(thread_pool.active_tail != NULL);
		thread->next->prev = thread;
	} else {
		rad_assert(thread_pool.active_tail == NULL);
		thread_pool.active_tail = thread;
	}
	thread_pool.active_head = thread;
	thread_pool.active_threads++;

	thread_pool.requests++;
	pthread_mutex_unlock(&thread_pool.mutex);

	thread->status = THREAD_ACTIVE;
	thread->request = request;

	/*
	 *	There's one more request in the queue.
	 *
	 *	Note that we're not touching the queue any more, so
	 *	the semaphore post is outside of the mutex.  This also
	 *	means that when the thread wakes up and tries to lock
	 *	the mutex, it will be unlocked, and there won't be
	 *	contention.
	 */
	sem_post(&thread->semaphore);

	return 1;
}

/*
 *	Remove a request from the queue.
 *
 *	Called with the thread mutex held.
 */
static REQUEST *request_dequeue(void)
{
	time_t blocked;
	static time_t last_complained = 0;
	static time_t total_blocked = 0;
	int num_blocked = 0;
	REQUEST *request = NULL;

retry:
	/*
	 *	Grab the first entry.
	 */
	request = fr_heap_peek(thread_pool.idle_heap);
	if (!request) {
		rad_assert(thread_pool.num_queued == 0);
		return NULL;
	}

	(void) fr_heap_extract(thread_pool.idle_heap, request);
	thread_pool.num_queued--;

	VERIFY_REQUEST(request);

	/*
	 *	Too late.  Mark it as done, and continue.
	 *
	 *	@fixme: with a heap, we can dynamically remove it from the heap!
	 *	@fixme: Is this memory leaked?  Probably...
	 */
	if (request->master_state == REQUEST_STOP_PROCESSING) {
		request->module = "<done>";
		request->child_state = REQUEST_DONE;
		goto retry;
	}

	rad_assert(request->magic == REQUEST_MAGIC);

	request->component = "<core>";
	request->module = NULL;
	request->child_state = REQUEST_RUNNING;

	blocked = time(NULL);
	if (!request->proxy && (blocked - request->timestamp.tv_sec) > 5) {
		total_blocked++;
		if (last_complained < blocked) {
			last_complained = blocked;
			blocked -= request->timestamp.tv_sec;
			num_blocked = total_blocked;
		} else {
			blocked = 0;
		}
	} else {
		total_blocked = 0;
		blocked = 0;
	}

	if (blocked) {
		ERROR("%d requests have been waiting in the processing queue for %d seconds.  Check that all databases are running properly!",
		      num_blocked, (int) blocked);
	}

	return request;
}


/*
 *	The main thread handler for requests.
 *
 *	Wait on the semaphore until we have it, and process the request.
 */
static void *request_handler_thread(void *arg)
{
	THREAD_HANDLE *thread = (THREAD_HANDLE *) arg;

	/*
	 *	Loop forever, until told to exit.
	 */
	while (true) {
		time_t now;
		REQUEST *request;

#  ifdef HAVE_GPERFTOOLS_PROFILER_H
		ProfilerRegisterThread();
#  endif

		/*
		 *	Wait to be signalled.
		 */
		DEBUG2("Thread %d waiting to be assigned a request",
		       thread->thread_num);

		while (sem_wait(&thread->semaphore) != 0) {
			/*
			 *	Interrupted system call.  Go back to
			 *	waiting, but DON'T print out any more
			 *	text.
			 */
			if (errno == EINTR) {
				DEBUG2("Re-wait %d", thread->thread_num);
				continue;
			}
			ERROR("Thread %d failed waiting for semaphore: %s: Exiting\n",
			      thread->thread_num, fr_syserror(errno));

			rad_assert(thread->status == THREAD_IDLE);

			thread->status = THREAD_CANCELLED;

			/*
			 *	Remove ourselves from the idle list
			 */
			if (thread->prev) {
				rad_assert(thread_pool.idle_head != thread);
				thread->prev->next = thread->next;

			} else {
				rad_assert(thread_pool.idle_head == thread);
				thread_pool.idle_head = thread->next;
			}

			if (thread->next) {
				rad_assert(thread_pool.idle_tail != thread);
				thread->next->prev = NULL;
			} else {
				rad_assert(thread_pool.idle_tail == thread);
				thread_pool.idle_tail = thread->prev;
				rad_assert(thread_pool.idle_threads == 1);
			}
			thread_pool.idle_threads--;

			/*
			 *	Add the thread to the tail of the exited list.
			 */
			if (thread_pool.exited_tail) {
				thread->prev = thread_pool.exited_tail;
				thread->prev->next = thread;
				thread_pool.exited_tail = thread;
			} else {
				rad_assert(thread_pool.exited_head == NULL);
				thread_pool.exited_head = thread;
				thread_pool.exited_tail = thread;
				thread->prev = NULL;
			}
			thread_pool.total_threads--;
			break;
		}

	process:
		/*
		 *	Maybe we've been idle for too long.
		 */
		if (thread->status == THREAD_CANCELLED) break;

		/*
		 *	The server is exiting.  Don't dequeue any
		 *	requests.
		 */
		if (thread_pool.stop_flag) break;

		rad_assert(thread->request != NULL);
		request = thread->request;

#ifdef WITH_ACCOUNTING
		if ((thread->request->packet->code == PW_CODE_ACCOUNTING_REQUEST) &&
		    thread_pool.auto_limit_acct) {
			VALUE_PAIR *vp;

			vp = radius_pair_create(request, &request->config,
					       181, VENDORPEC_FREERADIUS);
			if (vp) vp->vp_integer = thread_pool.pps_in.pps;

			vp = radius_pair_create(request, &request->config,
					       182, VENDORPEC_FREERADIUS);
			if (vp) vp->vp_integer = thread_pool.pps_in.pps;

			vp = radius_pair_create(request, &request->config,
					       183, VENDORPEC_FREERADIUS);
			if (vp) {
				vp->vp_integer = thread_pool.max_queue_size - thread_pool.num_queued;
				vp->vp_integer *= 100;
				vp->vp_integer /= thread_pool.max_queue_size;
			}
		}
#endif

		thread->request_count++;

		DEBUG2("Thread %d handling request %d, (%d handled so far)",
		       thread->thread_num, request->number,
		       thread->request_count);

		request->child_pid = thread->pthread_id;
		request->component = "<core>";
		request->module = NULL;
		request->child_state = REQUEST_RUNNING;
		request->log.unlang_indent = 0;

		request->process(thread->request, FR_ACTION_RUN);

		thread->request = NULL;

		/*
		 *	Clean up any children we exec'd.
		 */
		reap_children();

#  ifdef HAVE_OPENSSL_ERR_H
		/*
		 *	Clear the error queue for the current thread.
		 */
		ERR_clear_error();
#  endif

		pthread_mutex_lock(&thread_pool.mutex);

		/*
		 *	Manage the thread pool once a second.
		 *
		 *	This is done in a child thread to ensure that
		 *	the main socket thread(s) do as little work as
		 *	possible.
		 */
		now = time(NULL);
		if (thread_pool.managed < now) {
			thread_pool_manage(now);
		}

		/*
		 *	If there are requests waiting on the queue,
		 *	grab one and process it.
		 */
		if (thread_pool.num_queued) {
			request = request_dequeue();
			if (request) {
				pthread_mutex_unlock(&thread_pool.mutex);
				thread->request = request;
				goto process;
			}

			/*
			 *	Else there was an old request which was discard,
			 *	we're now idle.
			 */
			rad_assert(thread_pool.num_queued == 0);
		}

		/*
		 *	Remove the thread from the active list.
		 */
		rad_assert(thread->status == THREAD_ACTIVE);

		if (thread->prev) {
			rad_assert(thread_pool.active_head != thread);
			thread->prev->next = thread->next;

		} else {
			rad_assert(thread_pool.active_head == thread);
			thread_pool.active_head = thread->next;
		}

		if (thread->next) {
			rad_assert(thread_pool.active_tail != thread);
			thread->next->prev = thread->prev;
		} else {
			rad_assert(thread_pool.active_tail == thread);
			thread_pool.active_tail = thread->prev;
		}
		thread_pool.active_threads--;

		/*
		 *	Insert it into the head of the idle list.
		 */
		thread->prev = NULL;
		thread->next = thread_pool.idle_head;
		if (thread->next) {
			rad_assert(thread_pool.idle_tail != NULL);
			thread->next->prev = thread;
		} else {
			rad_assert(thread_pool.idle_tail == NULL);
			thread_pool.idle_tail = thread;
		}
		thread_pool.idle_head = thread;
		thread_pool.idle_threads++;

		thread->status = THREAD_IDLE;
		pthread_mutex_unlock(&thread_pool.mutex);
	}

	DEBUG2("Thread %d exiting...", thread->thread_num);

#  ifdef HAVE_OPENSSL_ERR_H
	/*
	 *	If we linked with OpenSSL, the application
	 *	must remove the thread's error queue before
	 *	exiting to prevent memory leaks.
	 */
	ERR_remove_state(0);
#  endif

	trigger_exec(NULL, NULL, "server.thread.stop", true, NULL);
	thread->status = THREAD_EXITED;

	return NULL;
}

/*
 *	Spawn a new thread, and place it in the thread pool.
 *	Called with the thread mutex locked...
 *
 *	The thread is started initially in the blocked state, waiting
 *	for the semaphore.
 */
static THREAD_HANDLE *spawn_thread(time_t now, int do_trigger)
{
	int rcode;
	THREAD_HANDLE *thread;

	/*
	 *	Allocate a new thread handle.
	 */
	thread = (THREAD_HANDLE *) rad_malloc(sizeof(THREAD_HANDLE));
	memset(thread, 0, sizeof(THREAD_HANDLE));

	thread->thread_num = thread_pool.max_thread_num++;
	thread->request_count = 0;
	thread->status = THREAD_IDLE;
	thread->timestamp = now;

	memset(&thread->semaphore, 0, sizeof(thread->semaphore));
	rcode = sem_init(&thread->semaphore, 0, SEMAPHORE_LOCKED);
	if (rcode != 0) {
		ERROR("Failed to initialize semaphore: %s", fr_syserror(errno));
		free(thread);
		return NULL;
	}

	/*
	 *	Create the thread joinable, so that it can be cleaned up
	 *	using pthread_join().
	 *
	 *	Note that the function returns non-zero on error, NOT
	 *	-1.  The return code is the error, and errno isn't set.
	 */
	rcode = pthread_create(&thread->pthread_id, 0, request_handler_thread, thread);
	if (rcode != 0) {
		free(thread);
		ERROR("Thread create failed: %s",
		       fr_syserror(rcode));
		return NULL;
	}

	DEBUG2("Thread spawned new child %d. Total threads in pool: %d",
	       thread->thread_num, thread_pool.total_threads + 1);
	if (do_trigger) trigger_exec(NULL, NULL, "server.thread.start", true, NULL);

	return thread;
}
#endif	/* WITH_GCD */


#ifdef WNOHANG
static uint32_t pid_hash(void const *data)
{
	thread_fork_t const *tf = data;

	return fr_hash(&tf->pid, sizeof(tf->pid));
}

static int pid_cmp(void const *one, void const *two)
{
	thread_fork_t const *a = one;
	thread_fork_t const *b = two;

	return (a->pid - b->pid);
}
#endif

static int timestamp_cmp(void const *one, void const *two)
{
	REQUEST const *a = one;
	REQUEST const *b = two;

	if (timercmp(&a->timestamp, &b->timestamp, < )) return -1;
	if (timercmp(&a->timestamp, &b->timestamp, > )) return +1;

	return 0;
}

/*
 *	Smaller entries go to the top of the heap.
 *	Larger ones to the bottom of the heap.
 */
static int default_cmp(void const *one, void const *two)
{
	REQUEST const *a = one;
	REQUEST const *b = two;

	if (a->priority < b->priority) return -1;
	if (a->priority > b->priority) return +1;

	return timestamp_cmp(one, two);
}


/*
 *	Prioritize by how far along the EAP session is.
 */
static int state_cmp(void const *one, void const *two)
{
	REQUEST const *a = one;
	REQUEST const *b = two;

	/*
	 *	Rounds which are further along go higher in the heap.
	 */
	if (a->packet->rounds > b->packet->rounds) return -1;
	if (a->packet->rounds < b->packet->rounds) return +1;

	return default_cmp(one, two);
}


/** Parse the configuration for the thread pool
 *
 */
int thread_pool_bootstrap(CONF_SECTION *cs, bool *spawn_workers)
{
	CONF_SECTION	*pool_cf;

	rad_assert(spawn_workers != NULL);
	rad_assert(pool_initialized == false); /* not called on HUP */

	/*
	 *	Initialize the thread pool to some reasonable values.
	 */
	memset(&thread_pool, 0, sizeof(THREAD_POOL));
#ifndef WITH_GCD
	thread_pool.total_threads = 0;
	thread_pool.max_thread_num = 1;
	thread_pool.cleanup_delay = 5;
	thread_pool.stop_flag = false;
#endif
	thread_pool.spawn_workers = *spawn_workers;

	pool_cf = cf_subsection_find_next(cs, NULL, "thread");
#ifdef WITH_GCD
	if (pool_cf) WARN("Built with Grand Central Dispatch.  Ignoring 'thread' subsection");
#else
	if (!pool_cf) {
		*spawn_workers = false;
		return 0;
	}
#endif

#ifndef WITH_GCD
	if (cf_section_parse(pool_cf, NULL, thread_config) < 0) return -1;

	/*
	 *	Catch corner cases.
	 */
	if (thread_pool.min_spare_threads < 1)
		thread_pool.min_spare_threads = 1;
	if (thread_pool.max_spare_threads < 1)
		thread_pool.max_spare_threads = 1;
	if (thread_pool.max_spare_threads < thread_pool.min_spare_threads)
		thread_pool.max_spare_threads = thread_pool.min_spare_threads;
	if (thread_pool.max_threads == 0)
		thread_pool.max_threads = 256;
	if ((thread_pool.max_queue_size < 2) || (thread_pool.max_queue_size > 1024*1024)) {
		ERROR("FATAL: max_queue_size value must be in range 2-1048576");
		return -1;
	}

	if (thread_pool.start_threads > thread_pool.max_threads) {
		ERROR("FATAL: start_servers (%i) must be <= max_servers (%i)",
		      thread_pool.start_threads, thread_pool.max_threads);
		return -1;
	}

	if (!thread_pool.queue_priority ||
	    (strcmp(thread_pool.queue_priority, "default") == 0)) {
		thread_pool.heap_cmp = default_cmp;

	} else if (strcmp(thread_pool.queue_priority, "eap") == 0) {
		thread_pool.heap_cmp = state_cmp;

	} else if (strcmp(thread_pool.queue_priority, "time") == 0) {
		thread_pool.heap_cmp = timestamp_cmp;

	} else {
		ERROR("FATAL: Invalid queue_priority '%s'", thread_pool.queue_priority);
		return -1;
	}

#endif	/* WITH_GCD */
	return 0;
}


/*
 *	Allocate the thread pool, and seed it with an initial number
 *	of threads.
 */
int thread_pool_init(void)
{
#ifndef WITH_GCD
	uint32_t	i;
	int		rcode;
#endif
	time_t		now;

	now = time(NULL);

	/*
	 *	Don't bother initializing the mutexes or
	 *	creating the hash tables.  They won't be used.
	 */
	if (!thread_pool.spawn_workers) return 0;

	/*
	 *	The pool has already been initialized.  Don't spawn
	 *	new threads, and don't forget about forked children.
	 */
	if (pool_initialized) return 0;

#ifdef WNOHANG
	if ((pthread_mutex_init(&thread_pool.wait_mutex,NULL) != 0)) {
		ERROR("FATAL: Failed to initialize wait mutex: %s",
		       fr_syserror(errno));
		return -1;
	}

	/*
	 *	Create the hash table of child PID's
	 */
	thread_pool.waiters = fr_hash_table_create(NULL, pid_hash, pid_cmp, free);
	if (!thread_pool.waiters) {
		ERROR("FATAL: Failed to set up wait hash");
		return -1;
	}
#endif


#ifndef WITH_GCD
	rcode = pthread_mutex_init(&thread_pool.mutex, NULL);
	if (rcode != 0) {
		ERROR("FATAL: Failed to initialize thread pool mutex: %s",
		       fr_syserror(errno));
		return -1;
	}

	thread_pool.idle_heap = fr_heap_create(thread_pool.heap_cmp, offsetof(REQUEST, heap_id));
	if (!thread_pool.idle_heap) {
		ERROR("FATAL: Failed to initialize the incoming queue.");
		return -1;
	}

	/*
	 *	Create a number of waiting threads.  Note we don't
	 *	need to lock the mutex, as nothing is sending
	 *	requests.
	 *
	 *	FIXE: If we fail while creating them, do something intelligent.
	 */
	for (i = 0; i < thread_pool.start_threads; i++) {
		THREAD_HANDLE *thread;

		thread = spawn_thread(now, 0);
		if (!thread) return -1;

		thread->prev = NULL;
		thread->next = thread_pool.idle_head;
		if (thread->next) {
			rad_assert(thread_pool.idle_tail != NULL);
			thread->next->prev = thread;
		} else {
			rad_assert(thread_pool.idle_tail == NULL);
			thread_pool.idle_tail = thread;
		}
		thread_pool.idle_head = thread;
		thread_pool.idle_threads++;

		thread_pool.total_threads++;
	}
#else
	thread_pool.queue = dispatch_queue_create("org.freeradius.threads", NULL);
	if (!thread_pool.queue) {
		ERROR("Failed creating dispatch queue: %s\n",
		       fr_syserror(errno));
		fr_exit(1);
	}
#endif

	DEBUG2("Thread pool initialized");
	pool_initialized = true;
	return 0;
}


/*
 *	Stop all threads in the pool.
 */
void thread_pool_stop(void)
{
#ifndef WITH_GCD
	THREAD_HANDLE *thread;
	THREAD_HANDLE *next;

	if (!pool_initialized) return;

	/*
	 *	Set pool stop flag.
	 */
	thread_pool.stop_flag = true;


	/*
	 *	Join and free all threads.
	 */
	for (thread = thread_pool.exited_head; thread; thread = next) {
		next = thread->next;

		pthread_join(thread->pthread_id, NULL);
		free(thread);
	}

	for (thread = thread_pool.idle_head; thread; thread = next) {
		next = thread->next;

		thread->status = THREAD_CANCELLED;
		sem_post(&thread->semaphore);

		pthread_join(thread->pthread_id, NULL);
		free(thread);
	}

	for (thread = thread_pool.active_head; thread; thread = next) {
		next = thread->next;

		thread->status = THREAD_CANCELLED;
		sem_post(&thread->semaphore);

		pthread_join(thread->pthread_id, NULL);
		free(thread);
	}

	fr_heap_delete(thread_pool.idle_heap);

#  ifdef WNOHANG
	fr_hash_table_free(thread_pool.waiters);
#  endif
#endif
}


#ifdef WITH_GCD
int request_enqueue(REQUEST *request)
{
	dispatch_block_t block;

	block = ^{
		request->process(request, FR_ACTION_RUN);
	};

	dispatch_async(thread_pool.queue, block);

	return 1;
}
#endif

#ifndef WITH_GCD
/*
 *	Check the min_spare_threads and max_spare_threads.
 *
 *	If there are too many or too few threads waiting, then we
 *	either create some more, or delete some.
 *
 *	This is called only from request_enqueue(), with the
 *	mutex held.
 */
static void thread_pool_manage(time_t now)
{
	THREAD_HANDLE *thread;

	thread_pool.managed = now;

	/*
	 *	Delete one exited thread.
	 */
	if (thread_pool.exited_head &&
	    (thread_pool.exited_head->status == THREAD_EXITED)) {
		thread = thread_pool.exited_head;

		/*
		 *	Unlink it from the exited list.
		 *
		 *	It's already been removed from
		 *	"total_threads", as we don't count threads
		 *	which are doing nothing.
		 */
		thread_pool.exited_head = thread->next;
		if (thread->next) {
			thread->next->prev = NULL;
		} else {
			thread_pool.exited_tail = NULL;
		}

		/*
		 *	Deleting old threads can take time, so we join
		 *	it with the mutex unlocked.
		 */
		pthread_mutex_unlock(&thread_pool.mutex);
		pthread_join(thread->pthread_id, NULL);
		free(thread);
		pthread_mutex_lock(&thread_pool.mutex);
	}

	/*
	 *	If there are too few spare threads.  Go create some more.
	 */
	if (!thread_pool.spawning &&
	    (thread_pool.total_threads < thread_pool.max_threads) &&
	    (thread_pool.idle_threads < thread_pool.min_spare_threads)) {
		uint32_t i, total;

		total = thread_pool.min_spare_threads - thread_pool.idle_threads;

		if ((total + thread_pool.total_threads) > thread_pool.max_threads) {
			total = thread_pool.max_threads - thread_pool.total_threads;
		}

		/*
		 *	Return if we don't need to create any new spares.
		 */
		if (total == 0) return;

		/*
		 *	Create a number of spare threads, and insert
		 *	them into the idle queue.
		 */
		for (i = 0; i < total; i++) {
			thread_pool.spawning = true;

			pthread_mutex_unlock(&thread_pool.mutex);
			thread = spawn_thread(now, 1);
			pthread_mutex_lock(&thread_pool.mutex);

			thread_pool.spawning = false;

			/*
			 *	Insert it into the head of the idle list.
			 */
			thread->prev = NULL;
			thread->next = thread_pool.idle_head;
			if (thread->next) {
				thread->next->prev = thread;
			} else {
				rad_assert(thread_pool.idle_tail == NULL);
				thread_pool.idle_tail = thread;
			}
			thread_pool.idle_head = thread;
			thread_pool.idle_threads++;
			thread_pool.total_threads++;
		}

		return;
	}

	/*
	 *	Only delete the spare threads if sufficient time has
	 *	passed since we last created one.  This helps to minimize
	 *	the amount of create/delete cycles.
	 */
	if ((now - thread_pool.time_last_spawned) < (int)thread_pool.cleanup_delay) {
		return;
	}

	/*
	 *	If there are too many spare threads, delete one.
	 *
	 *	Note that we only delete ONE at a time, instead of
	 *	wiping out many.  This allows the excess threads to be
	 *	slowly reaped, which is better than suddenly nuking a
	 *	bunch of them.
	 */
	if (thread_pool.idle_threads > thread_pool.max_spare_threads) {
		DEBUG2("Threads: deleting 1 spare out of %d spares",
		       thread_pool.idle_threads - thread_pool.max_spare_threads);

		rad_assert(thread_pool.idle_tail != NULL);

		/*
		 *	Remove the thread from the tail of the idle list.
		 */
		thread = thread_pool.idle_tail;
		rad_assert(thread->next == NULL);

		thread_pool.idle_tail = thread->prev;
		if (thread->prev) {
			thread->prev->next = NULL;
		} else {
			thread_pool.idle_head = NULL;
			rad_assert(thread_pool.idle_threads == 1);
		}
		thread_pool.idle_threads--;

		/*
		 *	Add the thread to the tail of the exited list.
		 */
		if (thread_pool.exited_tail) {
			thread->prev = thread_pool.exited_tail;
			thread->prev->next = thread;
			thread_pool.exited_tail = thread;
		} else {
			thread->prev = NULL;
			rad_assert(thread_pool.exited_head == NULL);
			thread_pool.exited_head = thread;
			thread_pool.exited_tail = thread;
		}
		thread_pool.total_threads--;

		rad_assert(thread->status == THREAD_IDLE);
		thread->status = THREAD_CANCELLED;

		/*
		 *	Post an extra semaphore, as a
		 *	signal to wake up, and exit.
		 */
		sem_post(&thread->semaphore);
	}

	/*
	 *	Otherwise everything's kosher.  There are not too few,
	 *	or too many spare threads.
	 */
	return;
}
#endif	/* WITH_GCD */

#ifdef WNOHANG
/*
 *	Thread wrapper for fork().
 */
pid_t rad_fork(void)
{
	pid_t child_pid;

	if (!pool_initialized) return fork();

	reap_children();	/* be nice to non-wait thingies */

	if (fr_hash_table_num_elements(thread_pool.waiters) >= 1024) {
		return -1;
	}

	/*
	 *	Fork & save the PID for later reaping.
	 */
	child_pid = fork();
	if (child_pid > 0) {
		int rcode;
		thread_fork_t *tf;

		tf = rad_malloc(sizeof(*tf));
		memset(tf, 0, sizeof(*tf));

		tf->pid = child_pid;

		pthread_mutex_lock(&thread_pool.wait_mutex);
		rcode = fr_hash_table_insert(thread_pool.waiters, tf);
		pthread_mutex_unlock(&thread_pool.wait_mutex);

		if (!rcode) {
			ERROR("Failed to store PID, creating what will be a zombie process %d",
			       (int) child_pid);
			free(tf);
		}
	}

	/*
	 *	Return whatever we were told.
	 */
	return child_pid;
}


/*
 *	Wait 10 seconds at most for a child to exit, then give up.
 */
pid_t rad_waitpid(pid_t pid, int *status)
{
	int i;
	thread_fork_t mytf, *tf;

	if (!pool_initialized) return waitpid(pid, status, 0);

	if (pid <= 0) return -1;

	mytf.pid = pid;

	pthread_mutex_lock(&thread_pool.wait_mutex);
	tf = fr_hash_table_finddata(thread_pool.waiters, &mytf);
	pthread_mutex_unlock(&thread_pool.wait_mutex);

	if (!tf) return -1;

	for (i = 0; i < 100; i++) {
		reap_children();

		if (tf->exited) {
			*status = tf->status;

			pthread_mutex_lock(&thread_pool.wait_mutex);
			fr_hash_table_delete(thread_pool.waiters, &mytf);
			pthread_mutex_unlock(&thread_pool.wait_mutex);
			return pid;
		}
		usleep(100000);	/* sleep for 1/10 of a second */
	}

	/*
	 *	10 seconds have passed, give up on the child.
	 */
	pthread_mutex_lock(&thread_pool.wait_mutex);
	fr_hash_table_delete(thread_pool.waiters, &mytf);
	pthread_mutex_unlock(&thread_pool.wait_mutex);

	return 0;
}
#else
/*
 *	No rad_fork or rad_waitpid
 */
#endif

void thread_pool_queue_stats(int array[RAD_LISTEN_MAX], int pps[2])
{
	int i;

#ifndef WITH_GCD
	if (pool_initialized) {
		struct timeval now;

		/*
		 *	@fixme: the list of listeners is no longer
		 *	fixed in size.
		 */
		memset(array, 0, sizeof(array[0]) * RAD_LISTEN_MAX);
		array[0] = fr_heap_num_elements(thread_pool.idle_heap);

		gettimeofday(&now, NULL);

		pps[0] = rad_pps(&thread_pool.pps_in.pps_old,
				 &thread_pool.pps_in.pps_now,
				 &thread_pool.pps_in.time_old,
				 &now);
		pps[1] = rad_pps(&thread_pool.pps_out.pps_old,
				 &thread_pool.pps_out.pps_now,
				 &thread_pool.pps_out.time_old,
				 &now);

	} else
#endif	/* WITH_GCD */
	{
		for (i = 0; i < RAD_LISTEN_MAX; i++) {
			array[i] = 0;
		}

		pps[0] = pps[1] = 0;
	}
}

/** Return the maximum number of threads that can run concurrently
 *
 */
uint32_t thread_pool_max_threads(void)
{
	return thread_pool.max_threads;
}
#endif /* HAVE_PTHREAD_H */
