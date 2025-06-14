/*
 * Copyright (C) 2021 Stefano Moioli <smxdev4@gmail.com>
 * This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held liable for any damages arising from the use of this software.
 * Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following restrictions:
 *  1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
 *  2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
 *  3. This notice may not be removed or altered from any source distribution.
 */

intptr_t SCAPI injected_sc6(volatile struct injcode_call *sc){
	return CALL_FPTR(sc->libc_syscall,
		sc->argv[0], sc->argv[1],
		sc->argv[2], sc->argv[3],
		sc->argv[4], sc->argv[5],
		sc->argv[6]
	);
}

INLINE void inj_thread_stop(struct injcode_ctx *ctx, int signal){
	// awake ptrace
	// success: SIGSTOP
	// failure: anything else
	struct injcode_bearing *br = ctx->br;
	CALL_FPTR(br->libc_syscall,
		__NR_kill, CALL_FPTR(br->libc_syscall,
			__NR_getpid), signal);
	while(1);
}

INLINE void *inj_dlopen(struct injcode_ctx *ctx, const char *filename, unsigned flags){
	return CALL_FPTR(ctx->libdl.dlopen,
		filename, flags);
}

INLINE void inj_thread_init(
	struct injcode_bearing *br,
	struct thread_api *api
){
	CALL_FPTR(api->pthread_mutex_init,
		&br->mutex, 0);
	CALL_FPTR(api->pthread_cond_init,
		&br->cond, 0);
}

INLINE intptr_t inj_thread_wait(
	struct injcode_ctx *ctx,
	intptr_t *pExitStatus
){
	struct injcode_bearing *br = ctx->br;
	struct thread_api *api = &ctx->libthread;

	// wait for user thread to die
	PCALL(ctx, inj_dchar, 'j');

	CALL_FPTR(api->pthread_mutex_lock,
		&br->mutex);
	while(!br->loaded_signal){
		CALL_FPTR(api->pthread_cond_wait,
			&br->cond, &br->mutex);
	}
	CALL_FPTR(api->pthread_mutex_unlock,
		&br->mutex);

	*pExitStatus = br->thread_exit_code;
	return 0;
}

INLINE intptr_t _inj_init_libdl(struct injcode_ctx *ctx){
	PCALL(ctx, inj_puts, ctx->libdl_name);

	// just to make sure it's really loaded
	ctx->h_libdl = CALL_FPTR(ctx->libdl.dlopen,
		ctx->libdl_name, RTLD_NOLOAD);
	if(ctx->h_libdl == NULL){
		ctx->h_libdl = CALL_FPTR(ctx->libdl.dlopen,
			ctx->libdl_name, RTLD_NOW | RTLD_GLOBAL);
	}
	PCALL(ctx, inj_dbgptr, ctx->h_libdl);

	if(ctx->h_libdl == NULL){
		return -1;
	}

#ifdef EZ_TARGET_DARWIN
	void *h_self = CALL_FPTR(ctx->libdl.dlopen,
		NULL, RTLD_LAZY);
	if(h_self == NULL){
		return -1;
	}
	intptr_t res = PCALL(ctx, inj_fetchsym, EZSTR_API_DLERROR, h_self, (void **)&ctx->libdl.dlerror);
	CALL_FPTR(ctx->libdl.dlclose,
		h_self);
	return res;
#else
	return PCALL(ctx, inj_fetchsym, EZSTR_API_DLERROR, ctx->h_libdl, (void **)&ctx->libdl.dlerror);
#endif
}

INLINE intptr_t inj_api_init(struct injcode_ctx *ctx){
	intptr_t result = 0;

	if(_inj_init_libdl(ctx) != 0){
		return -1;
	}
	result += PCALL(ctx, inj_fetchsym, EZSTR_API_PTHREAD_MUTEX_INIT, ctx->h_libthread, (void **)&ctx->libthread.pthread_mutex_init);
	result += PCALL(ctx, inj_fetchsym, EZSTR_API_PTHREAD_MUTEX_LOCK, ctx->h_libthread, (void **)&ctx->libthread.pthread_mutex_lock);
	result += PCALL(ctx, inj_fetchsym, EZSTR_API_PTHREAD_MUTEX_UNLOCK, ctx->h_libthread, (void **)&ctx->libthread.pthread_mutex_unlock);
	result += PCALL(ctx, inj_fetchsym, EZSTR_API_COND_INIT, ctx->h_libthread, (void **)&ctx->libthread.pthread_cond_init);
	result += PCALL(ctx, inj_fetchsym, EZSTR_API_COND_WAIT, ctx->h_libthread, (void **)&ctx->libthread.pthread_cond_wait);
	if(result != 0){
		return -1;
	}

	ctx->libthread.pthread_mutex_init.self = VPTR(PTRADD(ctx, offsetof(struct injcode_ctx, libthread.pthread_mutex_init)));
	ctx->libthread.pthread_mutex_lock.self = VPTR(PTRADD(ctx, offsetof(struct injcode_ctx, libthread.pthread_mutex_lock)));
	ctx->libthread.pthread_mutex_unlock.self = VPTR(PTRADD(ctx, offsetof(struct injcode_ctx, libthread.pthread_mutex_unlock)));
	ctx->libthread.pthread_cond_init.self = VPTR(PTRADD(ctx, offsetof(struct injcode_ctx, libthread.pthread_cond_init)));
	ctx->libthread.pthread_cond_wait.self = VPTR(PTRADD(ctx, offsetof(struct injcode_ctx, libthread.pthread_cond_wait)));
	return 0;
}

INLINE intptr_t inj_load_prepare(struct injcode_ctx *ctx){
	UNUSED(ctx);
	return 0;
}

#if defined(__NR_open)
#define SYSCALL_OPEN(file, mode) __NR_open, (file), (mode)
#elif defined( __NR_openat)
#define SYSCALL_OPEN(file, mode) __NR_openat, AT_FDCWD, (file), (mode)
#else
#error "Unsupported platform"
#endif

INLINE intptr_t inj_loginit(struct injcode_ctx *ctx){
	struct injcode_bearing *br = ctx->br;
	const char *log_filename = BR_STRTBL(br)[EZSTR_LOG_FILEPATH].str;

	int log_handle = STDOUT_FILENO;

	if(inj_strlen(log_filename) > 0){
		int new_log_handle = CALL_FPTR(br->libc_syscall,
			SYSCALL_OPEN(log_filename, O_WRONLY));
		if(new_log_handle >= 0){
			log_handle = new_log_handle;
		}
	}
	ctx->log_handle = log_handle;
	return 0;
}

INLINE intptr_t inj_logfini(struct injcode_ctx *ctx){
	if(ctx->log_handle != STDOUT_FILENO){
		CALL_FPTR(ctx->br->libc_syscall,
			__NR_close, ctx->log_handle);
	}
	return 0;
}
