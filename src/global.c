/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#include "common.h"
#include "global.h"
#include "hash.h"
#include "fileops.h"
#include "git2/threads.h"
#include "thread-utils.h"


git_mutex git__mwindow_mutex;

#define MAX_SHUTDOWN_CB 8

git_global_shutdown_fn git__shutdown_callbacks[MAX_SHUTDOWN_CB];
git_atomic git__n_shutdown_callbacks;
git_atomic git__n_inits;
git_atomic git__initialized;

void git__on_shutdown(git_global_shutdown_fn callback)
{
	int count = git_atomic_inc(&git__n_shutdown_callbacks);
	assert(count <= MAX_SHUTDOWN_CB);
	git__shutdown_callbacks[count - 1] = callback;
}

static void git__shutdown(void)
{
	int pos;

	while ((pos = git_atomic_dec(&git__n_shutdown_callbacks)) >= 0) {
		if (git__shutdown_callbacks[pos])
			git__shutdown_callbacks[pos]();
	}
}

/**
 * Handle the global state with TLS
 *
 * If libgit2 is built with GIT_THREADS enabled,
 * the `git_threads_init()` function must be called
 * before calling any other function of the library.
 *
 * This function allocates a TLS index (using pthreads
 * or the native Win32 API) to store the global state
 * on a per-thread basis.
 *
 * Any internal method that requires global state will
 * then call `git__global_state()` which returns a pointer
 * to the global state structure; this pointer is lazily
 * allocated on each thread.
 *
 * Before shutting down the library, the
 * `git_threads_shutdown` method must be called to free
 * the previously reserved TLS index.
 *
 * If libgit2 is built without threading support, the
 * `git__global_statestate()` call returns a pointer to a single,
 * statically allocated global state. The `git_thread_`
 * functions are not available in that case.
 */

/*
 * `git_threads_init()` allows subsystems to perform global setup,
 * which may take place in the global scope.  An explicit memory
 * fence exists at the exit of `git_threads_init()`.  Without this,
 * CPU cores are free to reorder cache invalidation of `_tls_init`
 * before cache invalidation of the subsystems' newly written global
 * state.
 */
#if defined(GIT_THREADS) && defined(GIT_WIN32)

static DWORD _tls_index;

int git_threads_init(void)
{
	int error;

	if (git_atomic_inc(&git__n_inits) > 1)
		return 0;

	_tls_index = TlsAlloc();
	if (git_mutex_init(&git__mwindow_mutex))
		return -1;

	/* Initialize any other subsystems that have global state */
	if (git__initialized.val == 0 &&
	    (error = git_hash_global_init()) >= 0 &&
	    (error = git_futils_dirs_global_init()) >= 0)
		git_atomic_inc(&git__initialized);

	GIT_MEMORY_BARRIER;

	win32_pthread_initialize();

	return error;
}

void git_threads_shutdown(void)
{
	if (git_atomic_dec(&git__n_inits) > 0) return;

	/* Shut down any subsystems that have global state */
	git__shutdown();

	TlsFree(_tls_index);

	git_mutex_free(&git__mwindow_mutex);
	git_atomic_dec(&git__initialized);
}

git_global_st *git__global_state(void)
{
	void *ptr;

	assert(git__n_inits.val);

	if ((ptr = TlsGetValue(_tls_index)) != NULL)
		return ptr;

	ptr = git__malloc(sizeof(git_global_st));
	if (!ptr)
		return NULL;

	memset(ptr, 0x0, sizeof(git_global_st));
	TlsSetValue(_tls_index, ptr);
	return ptr;
}

#elif defined(GIT_THREADS) && defined(_POSIX_THREADS)

static pthread_key_t _tls_key;

static void cb__free_status(void *st)
{
	git__free(st);
}

int git_threads_init(void)
{
	int error = 0;

	if (git_atomic_inc(&git__n_inits) > 1)
		return 0;

	if (git_mutex_init(&git__mwindow_mutex))
		return -1;
	pthread_key_create(&_tls_key, &cb__free_status);

	/* Initialize any other subsystems that have global state */
	if (git__initialized.val == 0 &&
	    (error = git_hash_global_init()) >= 0 &&
	    (error = git_futils_dirs_global_init()) >= 0)
		git_atomic_inc(&git__initialized);

	GIT_MEMORY_BARRIER;

	return error;
}

void git_threads_shutdown(void)
{
	if (git_atomic_dec(&git__n_inits) > 0) return;

	/* Shut down any subsystems that have global state */
	git__shutdown();

	void *ptr = pthread_getspecific(_tls_key);
	pthread_setspecific(_tls_key, NULL);
	git__free(ptr);

	pthread_key_delete(_tls_key);
	git_mutex_free(&git__mwindow_mutex);
	git_atomic_dec(&git__initialized);
}

git_global_st *git__global_state(void)
{
	void *ptr;

	assert(git__initialized.val);

	if ((ptr = pthread_getspecific(_tls_key)) != NULL)
		return ptr;

	ptr = git__malloc(sizeof(git_global_st));
	if (!ptr)
		return NULL;

	memset(ptr, 0x0, sizeof(git_global_st));
	pthread_setspecific(_tls_key, ptr);
	return ptr;
}

#else

static git_global_st __state;

int git_threads_init(void)
{
	/* noop */
	return 0;
}

void git_threads_shutdown(void)
{
	/* Shut down any subsystems that have global state */
	git__shutdown();
}

git_global_st *git__global_state(void)
{
	return &__state;
}

#endif /* GIT_THREADS */
