#include <assert.h>
#include <stdio.h>
#include <sys/types.h>
#include <ucontext.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

#include "kfc.h"
#include "valgrind.h"

#include "queue.h"
#include "test.h"

static int inited = 0;

int ids_so_far = 0;
int id = 0;


/**
 * FOR NEXT TIME: "Instead, build, modify, and write contexts using a pointer directly to where you will be storing them"
 * 	And do the while loop around our scheduler 
 */

/**
 * "slingo" is what I call an id while working with the queue
 */
queue_t queue;

ucontext_t scheduler;

struct thread_info {
	int id;
	ucontext_t ctx;
	void *rv; // return value
	int finished; // 0 for not finished, 1 for finished
	queue_t wait_queue;
};

struct thread_info threads[KFC_MAX_THREADS];

void scheduler_function(void *args) {

	DPRINTF("We're getting to the scheduler!\n");

	// 1. retrieve the next thread to run's id from the top of the queue
	struct thread_info *next_thread = (struct thread_info *)queue_dequeue(&queue);
	id = next_thread->id;

	setcontext(&threads[id].ctx);
}

void trampoline(void *(* start_func)(void *), void *arg) {
	kfc_exit(start_func(arg));
}

/**
 * Initializes the kfc library.  Programs are required to call this function
 * before they may use anything else in the library's public interface.
 *
 * @param kthreads    Number of kernel threads (pthreads) to allocate
 * @param quantum_us  Preemption timeslice in microseconds, or 0 for cooperative
 *                   &curr_ctx scheduling
 *
 * @return 0 if successful, nonzero on failure
 */
int
kfc_init(int kthreads, int quantum_us)
{
	assert(!inited);

	queue_init(&queue);

	getcontext(&threads[0].ctx);
	
	// initialize all wait queues
	for (int i = 0; i < KFC_MAX_THREADS; i++) {
		queue_init(&threads[i].wait_queue);
	}

	// initialize the scheduler ucontext_t
	getcontext(&scheduler);
	scheduler.uc_stack.ss_sp = malloc(KFC_DEF_STACK_SIZE);
	scheduler.uc_stack.ss_size = KFC_DEF_STACK_SIZE;
	scheduler.uc_stack.ss_flags = 0;
	VALGRIND_STACK_REGISTER(scheduler.uc_stack.ss_sp, scheduler.uc_stack.ss_sp + scheduler.uc_stack.ss_size);

	scheduler.uc_link = NULL;

	makecontext(&scheduler, (void (*)(void)) scheduler_function, 0);

	inited = 1;
	return 0;
}
/**
 * Cleans up any resources which were allocated by kfc_init.  You may assume
 * that this function is called only from the main thread, that any other
 * threads have terminated and been joined, and that threading will not be
 * needed again.  (In other words, just clean up and don't worry about the
 * consequences.)
 *
 * I won't be testing this function specifically, but it is provided as a
 * convenience to you if you are using Valgrind to check your code, which I
 * always encourage.
 */
void kfc_teardown(void)
{
	assert(inited);

	queue_destroy(&queue);
	for (int i = 0; i < KFC_MAX_THREADS; i++) {
		queue_destroy(&threads[i].wait_queue);
	}

	inited = 0;
}

/**
 * Creates a new user thread which executes the provided function concurrently.
 * It is left up to the implementation to decide whether the calling thread
 * continues to execute or the new thread takes over immediately.
 *
 * @param ptid[out]   Pointer to a tid_t variable in which to store the new
 *                    thread's ID
 * @param start_func  Thread main function
 * @param tid_t		  Argument to be passed to the thread main function
 * @param stack_base  Location of the thread's stack if already allocated, or
 *                    NULL if requesting that the library allocate it
 *                    dynamically
 * @param stack_size  Size (in bytes) of the thread's stack, or 0 to use the
 *                    default thread stack size KFC_DEF_STACK_SIZE
 *
 * @return 0 if successful, nonzero on failure
 */
int
kfc_create(tid_t *ptid, void *(*start_func)(void *), void *arg,
		caddr_t stack_base, size_t stack_size)
{
	assert(inited);

	if (stack_base == NULL) {
		if (stack_size == 0)
			stack_size = KFC_DEF_STACK_SIZE;
		stack_base = malloc(stack_size);
		if (stack_base == NULL) {
			perror("kfc_create stack_base malloc error: ");
			return -1;
		}
		VALGRIND_STACK_REGISTER(stack_base, stack_base + stack_size);
	}

	int current_id = id;
	*ptid = id = ++ids_so_far;

	getcontext(&threads[id].ctx);
	threads[id].ctx.uc_stack.ss_sp = stack_base;
	threads[id].ctx.uc_stack.ss_size = stack_size;
	threads[id].ctx.uc_stack.ss_flags = 0;
	
	threads[id].ctx.uc_link = &scheduler; // set this to the scheduler context

	threads[id].id = id;

	//makecontext(&threads[id].ctx, (void (*)(void)) start_func, 1, arg);
	makecontext(&threads[id].ctx, (void (*)(void))trampoline, 2, start_func, arg);

	queue_enqueue(&queue, &threads[id]);

	id = current_id;

	return 0;
}

/**
 * Exits the calling thread.  This should be the same thing that happens when
 * the thread's start_func returns.
 *
 * @param ret  Return value from the thread
 */
void
kfc_exit(void *ret)
{

	while (queue_peek(&threads[kfc_self()].wait_queue) != NULL) {
		queue_enqueue(&queue, queue_dequeue(&threads[kfc_self()].wait_queue));
	}

	threads[kfc_self()].rv = ret;
	threads[kfc_self()].finished = 1;

	setcontext(&scheduler);

	assert(inited);
}

/**
 * IN FUNCTION JOIN() OF JOINER
 * 1. Joiner thread calls join on thread N
 * 2. Joiner thread puts itself on the waiting/blocked queue of thread N
 * 3. Without enqueueing itself to the ready equeue, swap to the scheduler context (so now it's not on the ready queue anymore)
 * IN FUNCTION EXIT() OF THREAD N
 * 4. Eventually, thread N will get to the point that it exits.
 * 5. Thread N puts everything in its waiting queue onto the ready queue
 * 6. Thread N puts its return value somewhere that the joiner thread can access it
 * IN FUNCTION JOIN() OF JOINER
 * 7. Joiner thread will eventually get its chance to run again
 * 8. Joiner thread will look in the place where thread N stored its return value
 * 9. Joiner thread will return that value through an out parameter
 */

/**
 * Waits for the thread specified by tid to terminate, retrieving that threads
 * return value.  Returns immediately if the target thread has already
 * terminated, otherwise blocks.  Attempting to join a thread which already has
 * another thread waiting to join it, or attempting to join a thread which has
 * already been joined, results in undefined behavior.
 *
 * @param pret[out]  Pointer to a void * in which the thread's return value from
 *                   kfc_exit should be stored, or NULL if the caller does not
 *                   care.
 *
 * @return 0 if successful, nonzero on failure
 */
int
kfc_join(tid_t tid, void **pret)
{
	assert(inited);

	if (!threads[tid].finished) {
		// 2. Joiner thread puts itself on the waiting/blocked queue of thread N
		queue_enqueue(&threads[tid].wait_queue, &threads[kfc_self()]);

		// 3. Without enqueueing itself to the ready equeue, swap to the scheduler context (so now it's not on the ready queue anymore)
		//		BUT ONLY IF THREAD N HAS NOT ALREADY FINISHED
		swapcontext(&threads[kfc_self()].ctx, &scheduler);
	}

	// 7. Joiner thread will eventually get its chance to run again
	// 8. Joiner thread will look in the place where thread N stored its return value
	// 9. Joiner thread will return that value through an out parameter
	*pret = threads[tid].rv;

	return 0;
}

/**
 * Returns a small integer which identifies the calling thread.
 *
 * @return Thread ID of the currently executing thread
 */
tid_t
kfc_self(void)
{
	assert(inited);

	return id;
}

/**
 * Causes the calling thread to yield the processor voluntarily.  This may
 * result in another thread being scheduled, but it does not preclude the
 * possibility of the same thread continuing if re-chosen by the scheduling
 * algorithm.
 */
void
kfc_yield(void)
{
	assert(inited);

	/*int *slingo = malloc(sizeof(int));
	*slingo = id;
	queue_enqueue(&queue, slingo);*/

	queue_enqueue(&queue, &threads[id]);

	swapcontext(&threads[id].ctx, &scheduler);
}

/**
 * Initializes a user-level counting semaphore with a specific value.
 *
 * @param sem    Pointer to the semaphore to be initialized
 * @param value  Initial value for the semaphore's counter
 *
 * @return 0 if successful, nonzero on failure
 */
int
kfc_sem_init(kfc_sem_t *sem, int value)
{
	assert(inited);
	return 0;
}

/**
 * Increments the value of the semaphore.  This operation is also known as
 * up, signal, release, and V (Dutch verhoog, "increase").
 *
 * @param sem  Pointer to the semaphore which the thread is releasing
 *
 * @return 0 if successful, nonzero on failure
 */
int
kfc_sem_post(kfc_sem_t *sem)
{
	assert(inited);
	return 0;
}

/**
 * Attempts to decrement the value of the semaphore.  This operation is also
 * known as down, acquire, and P (Dutch probeer, "try").  This operation should
 * block when the counter is not above 0.
 *
 * @param sem  Pointer to the semaphore which the thread wishes to acquire
 *
 * @return 0 if successful, nonzero on failure
 */
int
kfc_sem_wait(kfc_sem_t *sem)
{
	assert(inited);
	return 0;
}

/**
 * Frees any resources associated with a semaphore.  Destroying a semaphore on
 * which threads are waiting results in undefined behavior.
 *
 * @param sem  Pointer to the semaphore to be destroyed
 */
void
kfc_sem_destroy(kfc_sem_t *sem)
{
	assert(inited);
}
