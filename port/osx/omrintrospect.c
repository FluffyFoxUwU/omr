/*******************************************************************************
 * Copyright IBM Corp. and others 2016
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] https://openjdk.org/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

/**
 * @file
 * @ingroup Port
 * @brief process introspection support
 */

#include <fcntl.h>
#include <mach/mach.h>
#include <poll.h>
#include <pthread.h>
#include <sys/time.h>
#define _XOPEN_SOURCE
#include <ucontext.h>

#include "omrintrospect.h"
#include "omrintrospect_common.h"
#include "omrport.h"
#include "omrsignal_context.h"

#define SUSPEND_SIG SIGUSR1

typedef struct PlatformWalkData {
	/* Thread to filter out of the platform iterator */
	thread_port_t filterThread;
	/* Array of mach port thread identifiers */
	thread_act_port_array_t threadList;
	/* Total number of threads in the process, including calling thread */
	mach_msg_type_number_t threadCount;
	/* Suspended threads unharvested */
	mach_msg_type_number_t threadIndex;
	/* Old signal handler for the suspend signal */
	struct sigaction oldHandler;
	/* Old mask */
	sigset_t oldMask;
	/* Records whether we need to clean up in resume */
	BOOLEAN cleanupRequired;
	/* Backpointer to encapsulating state */
	J9ThreadWalkState *state;
} PlatformWalkData;

/* unable to pass data into signal handler so use global ptr */
static J9ThreadWalkState *stateForBacktrace;
static int pipeFileDescriptor[2];

static void freeThread(J9ThreadWalkState *state, J9PlatformThread *thread);
static int32_t getThreadContext(J9ThreadWalkState *state);
static void resumeAllPreempted(PlatformWalkData *data);
static int32_t setupNativeThread(J9ThreadWalkState *state, thread_context *sigContext);
static int32_t suspendAllPreemptive(PlatformWalkData *data);
static void upcallHandler(int signal, siginfo_t *siginfo, void *context);
static int32_t timedWait(int32_t seconds);
static int32_t timeout(int64_t deadline);

static void
resumeAllPreempted(PlatformWalkData *data)
{
	if (data->cleanupRequired) {
		/* Restore the old signal handler. */
		if ((0 == (SA_SIGINFO & data->oldHandler.sa_flags)) && (SIG_DFL == data->oldHandler.sa_handler)) {
			/* If there wasn't an old signal handler then set this to ignore. There shouldn't be any signals,
			 * but better safe than sorry.
			 */
			data->oldHandler.sa_handler = SIG_IGN;
		}

		sigaction(SUSPEND_SIG, &data->oldHandler, NULL);

		/* Restore the old signal mask. */
		sigprocmask(SIG_SETMASK, &data->oldMask, NULL);
	}

	/* Resume each thread. */
	for (; data->threadIndex < data->threadCount; data->threadIndex += 1) {
		if (data->filterThread != data->threadList[data->threadIndex]) {
			thread_resume(data->threadList[data->threadIndex]);
		}
	}

	close(pipeFileDescriptor[0]);
	close(pipeFileDescriptor[1]);

	data->state->portLibrary->heap_free(data->state->portLibrary, data->state->heap, data);
	data->state->platform_data = NULL;
}

/* Obtain the backtrace and send an arbitrary byte to indicate completion.
 * Pipes are async-signal-safe. stateForBacktrace should be as well
 * since it is only used carefully after waiting on the pipe. Receiving
 * unexpected SUSPEND_SIG signals while iterating threads would cause
 * any implementation to misbehave.
 */
static void
upcallHandler(int signal, siginfo_t *siginfo, void *context)
{
	char *data = "A";

	stateForBacktrace->portLibrary->introspect_backtrace_thread(stateForBacktrace->portLibrary, stateForBacktrace->current_thread, stateForBacktrace->heap, NULL);

	if (OMR_ARE_NO_BITS_SET(stateForBacktrace->options, OMR_INTROSPECT_NO_SYMBOLS)) {
		stateForBacktrace->portLibrary->introspect_backtrace_symbols_ex(stateForBacktrace->portLibrary, stateForBacktrace->current_thread, stateForBacktrace->heap, 0);
	}

	write(pipeFileDescriptor[1], data, 1);
}

static int32_t
suspendAllPreemptive(PlatformWalkData *data)
{
	mach_port_t task = mach_task_self();
	struct sigaction upcallAction;
	int32_t rc = 0;
	mach_msg_type_number_t threadCount = 0;
	data->threadCount = 0;

	/* Install a signal handler to get thread callstack info from the handler. */
	upcallAction.sa_sigaction = upcallHandler;
	upcallAction.sa_flags = SA_SIGINFO | SA_RESTART;

	if (-1 == sigaction(SUSPEND_SIG, &upcallAction, &data->oldHandler)) {
		RECORD_ERROR(data->state, SIGNAL_SETUP_ERROR, -1);
		rc = -1;
	} else if (data->oldHandler.sa_sigaction == upcallHandler) {
		/* Handler's already installed so already iterating threads. We mustn't uninstall the handler
		 * while cleaning as the thread that installed the initial handler will do so.
		 */
		memset(&data->oldHandler, 0, sizeof(struct sigaction));
		RECORD_ERROR(data->state, CONCURRENT_COLLECTION, -1);
		rc = -1;
	}

	if (0 == rc) {
		sigset_t set;
		mach_msg_type_number_t filterThreadIndex = 0;
		mach_port_t temp = 0;

		/* After this point it's safe to go through the full cleanup. */
		data->cleanupRequired = TRUE;

		/* Unblock SUSPEND_SIG for this process as abort() blocks all signals except SIGABRT. */
		sigemptyset(&set);
		sigaddset(&set, SUSPEND_SIG);
		if (0 != sigprocmask(SIG_UNBLOCK, &set, &data->oldMask)) {
			RECORD_ERROR(data->state, SIGNAL_SETUP_ERROR, -2);
			rc = -1;
		}

		/* Suspend all threads until there are no new threads. */
		do {
			mach_msg_type_number_t i = 0;

			/* Get a list of the threads within the process. */
			data->threadCount = threadCount;
			if (KERN_SUCCESS != task_threads(task, &data->threadList, &threadCount)) {
				RECORD_ERROR(data->state, THREAD_COUNT_FAILURE, -1);
				rc = -1;
				break;
			}

			/* Suspend each thread except this one. */
			for (i = data->threadCount; i < threadCount; i += 1) {
				if (data->filterThread != data->threadList[i]) {
					if (KERN_SUCCESS != thread_suspend(data->threadList[i])) {
						data->threadCount = i;
						rc = -1;
						break;
					}
				} else {
					filterThreadIndex = i;
				}
			}
		} while ((threadCount > data->threadCount) && (0 == rc));

		/* Swap the filterthread/current thread with first thread so that the current thread
		 * is always the first thread iterated.
		 */
		temp = data->threadList[0];
		data->threadList[0] = data->threadList[filterThreadIndex];
		data->threadList[filterThreadIndex] = temp;
	}

	return rc;
}

/*
 * Frees anything and everything to do with the scope of the specified thread.
 */
static void
freeThread(J9ThreadWalkState *state, J9PlatformThread *thread)
{
	J9PlatformStackFrame *frame = NULL;

	if (NULL == thread) {
		return;
	}

	frame = thread->callstack;
	while (NULL != frame) {
		J9PlatformStackFrame *tmp = frame;
		frame = frame->parent_frame;

		if (NULL != tmp->symbol) {
			state->portLibrary->heap_free(state->portLibrary, state->heap, tmp->symbol);
			tmp->symbol = NULL;
		}

		state->portLibrary->heap_free(state->portLibrary, state->heap, tmp);
	}

	state->portLibrary->heap_free(state->portLibrary, state->heap, thread);

	if (state->current_thread == thread) {
		state->current_thread = NULL;
	}
}

/*
 * Sets up the native thread structures including the backtrace. If a context is specified it is used instead of grabbing
 * the context for the thread pointed to by state->current_thread.
 *
 * @param state - state variable for controlling the thread walk
 * @param sigContext - context to use in place of live thread context
 *
 * @return - 0 on success, non-zero otherwise.
 */
static int32_t
setupNativeThread(J9ThreadWalkState *state, thread_context *sigContext)
{
	PlatformWalkData *data = (PlatformWalkData *)state->platform_data;
	int32_t rc = 0;

	/* Allocate the thread container. */
	state->current_thread = (J9PlatformThread *)state->portLibrary->heap_allocate(state->portLibrary, state->heap, sizeof(J9PlatformThread));
	if (NULL == state->current_thread) {
		RECORD_ERROR(state, ALLOCATION_FAILURE, 2);
		rc = -1;
	}
	if (0 == rc) {
		memset(state->current_thread, 0, sizeof(J9PlatformThread));
	}

	if (0 == rc) {
		pthread_t pt = pthread_from_mach_thread_np(data->threadList[data->threadIndex]);
		if (NULL == pt) {
			rc = -1;
		} else {
			uint64_t tid = 0;
			if (0 == pthread_threadid_np(pt, &tid)) {
				state->current_thread->thread_id = tid;
				state->current_thread->process_id = getpid();

				if (data->filterThread != data->threadList[data->threadIndex]) {
					/* Generate the callstack by installing a signal handler and raising a signal. None was provided.
					 * Backtrace functions will be called from the signal handler.
					 */
					rc = getThreadContext(state);
				}
			} else {
				rc = -1;
			}
		}
	}

	if (0 == rc) {
		/* Populate backtraces if not present. Should only happen for the filter thread as it was
		 * the only thread that did not execute the signal handler.
		 */
		if (data->filterThread == data->threadList[data->threadIndex]) {
			if (NULL == state->current_thread->callstack) {
				SPECULATE_ERROR(state, FAULT_DURING_BACKTRACE, 2);
				state->portLibrary->introspect_backtrace_thread(state->portLibrary, state->current_thread, state->heap, sigContext);
				CLEAR_ERROR(state);
			}

			if (OMR_ARE_ANY_BITS_SET(state->options, OMR_INTROSPECT_NO_SYMBOLS)) {
				/* The caller asked us not to resolve symbols, so we don't expect to have a symbol for the top frame. */
			} else if ((NULL != state->current_thread->callstack) && (NULL == state->current_thread->callstack->symbol)) {
				SPECULATE_ERROR(state, FAULT_DURING_BACKTRACE, 3);
				state->portLibrary->introspect_backtrace_symbols_ex(state->portLibrary, state->current_thread, state->heap, 0);
				CLEAR_ERROR(state);
			}
		}

		if (0 != state->current_thread->error) {
			RECORD_ERROR(state, state->current_thread->error, 1);
		}
	}

	return rc;
}

static int32_t
getThreadContext(J9ThreadWalkState *state)
{
	int32_t ret = 0;
	PlatformWalkData *data = (PlatformWalkData *)state->platform_data;
	thread_port_t thread = data->threadList[data->threadIndex];

	pthread_t pthread = pthread_from_mach_thread_np(thread);
	if (NULL != pthread) {
		ret = pthread_kill(pthread, SUSPEND_SIG);
	} else {
		ret = -1;
	}

	if (0 == ret) {
		/* Resume the thread, with the signal pending. */
		if (KERN_SUCCESS != thread_resume(thread)) {
			ret = -1;
		}
	}

	if (0 == ret) {
		/* Wait for the signal handler to complete. */
		ret = timedWait(timeout(state->deadline1));
		if (TIMEOUT == ret) {
			RECORD_ERROR(state, TIMEOUT, 0);
		}
	}

	return ret;
}

/**
 * Utility function to calculate remaining time before the deadline elapses.
 *
 * @param deadline system time by which processing should have completed
 *
 * @return number of seconds before timeout.
 */
static int32_t
timeout(int64_t deadline)
{
	int32_t secs = 0;
	struct timeval tv;
	if ((0 == gettimeofday(&tv, NULL)) && (deadline > tv.tv_sec)) {
		secs = (int32_t)(deadline - tv.tv_sec);
	}

	return secs;
}

/**
 * This function waits for data on a pipe and will block for at most `seconds` seconds.
 *
 * @param seconds the maximum number of seconds to block for, must be positive
 *
 * @return 0 if success, -1 if error, TIMEOUT if timeout occurred.
 */
static int32_t
timedWait(int32_t seconds)
{
	int32_t ret = 0;

	if (seconds > 0) {
		struct pollfd fds;

		fds.fd = pipeFileDescriptor[0];
		fds.events = POLLIN;

		ret = poll(&fds, 1, seconds * 1000);

		if ((-1 == ret) || (0 != (fds.revents & (POLLHUP | POLLERR | POLLNVAL)))) {
			/* error from poll() */
			ret = -1;
		} else if (0 == ret) {
			/* timeout */
			ret = TIMEOUT;
		} else {
			char buffer;

			if (1 == read(fds.fd, &buffer, 1)) {
				/* success */
				ret = 0;
			} else {
				ret = -1;
			}
		}
	} else {
		/* already timed out */
		ret = TIMEOUT;
	}

	return ret;
}

/*
 * Creates an iterator for the native threads present within the process, either all native threads
 * or a subset depending on platform (see platform specific documentation for detail).
 * This function will suspend all of the native threads that will be presented through the iterator
 * and they will remain suspended until this function or nextDo return NULL. The context for the
 * calling thread is always presented first.
 *
 * @param heap used to contain the stack frame representations and symbol strings. No
 * 			memory is allocated outside of the heap provided. Must not be NULL.
 * @param state semi-opaque structure used as a cursor for iteration. Must not be NULL User visible fields
 *			are:
 * 			deadline - the system time in seconds at which to abort introspection and resume
 * 			error - numeric error description, 0 on success
 * 			error_detail - detail for the specific error
 * 			error_string - string description of error
 * @param signal_info a signal context to use. This will be used to remove signal handler frames from the
 * 			callstack.
 *
 * @return NULL if there is a problem suspending threads, gathering thread contexts or if the heap
 * is too small to contain even the context without stack trace. Errors are reported in the error,
 * and error_detail fields of the state structure. A brief textual description is in error_string.
 */
J9PlatformThread *
omrintrospect_threads_startDo_with_signal(struct OMRPortLibrary *portLibrary, J9Heap *heap, J9ThreadWalkState *state, void *signal_info)
{
	int32_t result = 0;
	PlatformWalkData *data = NULL;
	int flag = 0;

	/* Construct the walk state. */
	state->heap = heap;
	state->portLibrary = portLibrary;
	data = (PlatformWalkData *)portLibrary->heap_allocate(portLibrary, heap, sizeof(PlatformWalkData));
	state->platform_data = data;
	state->current_thread = NULL;
	stateForBacktrace = state;

	if (NULL == data) {
		RECORD_ERROR(state, ALLOCATION_FAILURE, 0);
		return NULL;
	}

	memset(data, 0, sizeof(PlatformWalkData));
	data->state = state;
	data->filterThread = mach_thread_self();

	/* Create a pipe to allow the resumed thread to report it has completed
	 * sending its callstack info.
	 */
	result = pipe(pipeFileDescriptor);
	if (0 != result) {
		RECORD_ERROR(state, INITIALIZATION_ERROR, result);
		goto cleanup;
	}
	/* Initialize pipe to be non-blocking as poll() ensures read() does not block. */
	flag = fcntl(pipeFileDescriptor[0], F_GETFL);
	fcntl(pipeFileDescriptor[0], F_SETFL, flag | O_NONBLOCK);

	/* Suspend all threads bar this one. */
	result = suspendAllPreemptive(state->platform_data);
	if (0 != result) {
		RECORD_ERROR(state, SUSPEND_FAILURE, result);
		goto cleanup;
	}

	result = setupNativeThread(state, signal_info);
	if (0 != result) {
		RECORD_ERROR(state, COLLECTION_FAILURE, result);
		goto cleanup;
	}

	return state->current_thread;

cleanup:
	resumeAllPreempted(data);
	return NULL;
}

/* This function is identical to j9introspect_threads_startDo_with_signal but omits the signal argument
 * and instead uses a live context for the calling thread.
 */
J9PlatformThread *
omrintrospect_threads_startDo(struct OMRPortLibrary *portLibrary, J9Heap *heap, J9ThreadWalkState *state)
{
	return omrintrospect_threads_startDo_with_signal(portLibrary, heap, state, NULL);
}

/* This function is called repeatedly to get subsequent threads in the iteration. The only way to
 * resume suspended threads is to continue calling this function until it returns NULL.
 *
 * @param state state structure initialized by a call to one of the startDo functions.
 *
 * @return NULL if there is an error or if no more threads are available. Sets the error fields as
 * listed detailed for j9introspect_threads_startDo_with_signal.
 */
J9PlatformThread *
omrintrospect_threads_nextDo(J9ThreadWalkState *state)
{
	int32_t result = 0;
	PlatformWalkData *data = state->platform_data;

	if (NULL == data) {
		/* State is invalid due to fatal error from startDo. */
		RECORD_ERROR(state, INVALID_STATE, 0);
		return NULL;
	}

	/* Cleanup the previous threads. */
	freeThread(state, state->current_thread);

	data->threadIndex += 1;
	if (data->threadIndex == data->threadCount) {
		/* Finished processing threads. */
		goto cleanup;
	}

	result = setupNativeThread(state, NULL);
	if (0 != result) {
		if (NULL == state->current_thread) {
			/* Allocation failure, can only return NULL which terminates iteration. */
			goto cleanup;
		} else if (TIMEOUT == result) {
			/* Timeout, don't walk rest of threads. Wait for thread in upcallHandler to complete before
			 * freeing thread.
			 */
			timedWait(timeout(state->deadline2));
			freeThread(state, state->current_thread);
			goto cleanup;
		}
		/* Other failures, non-fatal. */
		RECORD_ERROR(state, COLLECTION_FAILURE, result);
	}

	return state->current_thread;

cleanup:
	resumeAllPreempted(data);
	return NULL;
}

int32_t
omrintrospect_set_suspend_signal_offset(struct OMRPortLibrary *portLibrary, int32_t signalOffset)
{
	return OMRPORT_ERROR_NOT_SUPPORTED_ON_THIS_PLATFORM;
}

int32_t
omrintrospect_startup(struct OMRPortLibrary *portLibrary)
{
	return 0;
}

void
omrintrospect_shutdown(struct OMRPortLibrary *portLibrary)
{
	return;
}
