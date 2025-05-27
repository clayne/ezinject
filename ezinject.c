/*
 * Copyright (C) 2021 Stefano Moioli <smxdev4@gmail.com>
 * This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held liable for any damages arising from the use of this software.
 * Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following restrictions:
 *  1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
 *  2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
 *  3. This notice may not be removed or altered from any source distribution.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>
#include <stdint.h>
#include <limits.h>
#include <unistd.h>
#include <inttypes.h>
#include <ctype.h>

#include "config.h"

#include <fcntl.h>
#include <sched.h>

#ifdef EZ_TARGET_POSIX
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#endif

#include <sys/stat.h>

#include "dlfcn_compat.h"
#include "ezinject_util.h"
#include "ezinject.h"
#include "ezinject_common.h"
#include "ezinject_arch.h"
#include "ezinject_injcode.h"

#ifdef EZ_TARGET_WINDOWS
#include "os/windows/util.h"
#endif

static struct ezinj_ctx ctx; // only to be used for sigint handler

ez_region region_pl_code = {
	.start = (void *)&__start_payload,
	.end = (void *)&__stop_payload
};

#ifdef HAVE_SYSCALLS
uintptr_t get_wrapper_address(struct ezinj_ctx *ctx);
#endif

size_t create_layout(struct ezinj_ctx *ctx, size_t dyn_total_size, struct ezinj_pl *layout);

/**
 * Prepares the target process for a call invocation with syscall convention
 * NOTE: this function can be used to call anything, not just system calls
 *
 * @param[in]  orig_ctx	 the current process context
 * @param[out] new_ctx   the new process context
 * @param[in]  call      call arguments and options
 **/
intptr_t setregs_syscall(
	struct ezinj_ctx *ctx,
	regs_t *orig_ctx,
	regs_t *new_ctx,
	struct call_req *call
){
	REG(*new_ctx, REG_PC) = call->insn_addr;

	// this will be passed on the stack
	struct injcode_call *rcall = &call->rcall;

	uintptr_t target_sp = 0;
	if(call->stack_addr != 0){
		target_sp = call->stack_addr;
	} else {
		target_sp = REG(*orig_ctx, REG_SP);
	}
	// allocate remote call on the stack
	uintptr_t r_call_args = target_sp - sizeof(*rcall);

	rcall->argc = 0;
	rcall->result = 0;
	// copy call arguments
	memcpy(&rcall->argv, &call->argv, sizeof(call->argv));

#ifdef EZ_TARGET_POSIX
	rcall->libc_syscall = (void *)ctx->libc_syscall.remote;
#endif
#ifdef EZ_TARGET_LINUX
	if(ctx->force_mmap_syscall){
		rcall->libc_mmap = NULL;
	} else {
		rcall->libc_mmap = (void *)ctx->libc_mmap.remote;
	}
	rcall->libc_open = (void *)ctx->libc_open.remote;
	rcall->libc_read = (void *)ctx->libc_read.remote;
#endif
#ifdef EZ_TARGET_WINDOWS
	rcall->VirtualAlloc = (void *)ctx->virtual_alloc.remote;
	rcall->VirtualFree = (void *)ctx->virtual_free.remote;
	rcall->SuspendThread = (void *)ctx->suspend_thread.remote;
	rcall->GetCurrentThread = (void *)ctx->get_current_thread.remote;
#endif

#define PLAPI_USE(fn) rcall->plapi.fn = (void *)ctx->plapi.fn;
	PLAPI_USE(inj_memset);
	PLAPI_USE(inj_puts);
	PLAPI_USE(inj_dchar);
	PLAPI_USE(inj_dbgptr);
	PLAPI_USE(inj_fetchsym);
#undef PLAPI_USE

	// set the call structure as argument for the function being called
	rcall->trampoline.fn_arg = r_call_args;

	// count syscall arguments excluding syscall number
	for(int i=1; i<SC_MAX_ARGS; i++){
		if(CALL_HAS_ARG(*call, i)){
			rcall->argc++;
		}
	}

	#ifdef HAVE_SYSCALLS
	/**
	 * this target supports true system calls
	 * use a wrapper in-between to avoid stack corruption
	 * in some scenarios
	 *
	 * trampoline -> wrapper -> syscall
	 **/
	rcall->trampoline.fn_addr = get_wrapper_address(ctx);
	if(call->syscall_mode){
		/**
		 * set the branch target
		 * (based on the number of syscall arguments)
		 **/
		if(remote_call_prepare(ctx, rcall) < 0){
			ERR("remote_call_prepare failed");
			return -1;
		}
	} else {
		// call the user supplied target through the wrapper
		rcall->wrapper.target = (intptr_t (*)(volatile struct injcode_call *))ctx->branch_target.remote;
	}
	#else
	// call the user supplied target through the trampoline
	rcall->trampoline.fn_addr = ctx->branch_target.remote;
	#endif

	if(ctx->rcall_handler_pre != NULL){
		if(ctx->rcall_handler_pre(ctx, &call->rcall) < 0){
			ERR("rcall_handler_pre failed");
			return -1;
		}
	}

	// backup the stack area being overwritten
	ssize_t backupSize = (ssize_t)WORDALIGN(sizeof(*rcall));
	uint8_t *saved_stack = calloc(backupSize, 1);
	if(remote_read(ctx, saved_stack, r_call_args, backupSize) != backupSize){
		ERR("failed to backup stack");
		free(saved_stack);
		return -1;
	}

	// write the remote call onto the stack
	if(remote_write(
		ctx,
		r_call_args,
		rcall, sizeof(*rcall)
	) != sizeof(*rcall)){
		ERR("failed to write remote call");
		free(saved_stack);
		return -1;
	}

	// save original info for later
	call->backup_addr = r_call_args;
	call->backup_data = saved_stack;
	call->backup_size = backupSize;

	// update stack pointer
	REG(*new_ctx, REG_SP) = target_sp - sizeof(struct injcode_trampoline);

#ifdef DEBUG
	DBGPTR((void *)REG(*new_ctx, REG_SP));
#endif

#if defined(EZ_ARCH_ARM) && defined(HAVE_PSR_T_BIT)
	// set the ARM/Thumb flag for the shellcode invocation
	#ifdef USE_ARM_THUMB
	REG(*new_ctx, ARM_cpsr) = REG(*new_ctx, ARM_cpsr) | PSR_T_BIT;
	#else
	REG(*new_ctx, ARM_cpsr) = REG(*new_ctx, ARM_cpsr) & ~PSR_T_BIT;
	#endif
#endif

	return 0;
}


intptr_t remote_call_setup(struct ezinj_ctx *ctx, struct call_req *call, regs_t *orig_ctx, regs_t *new_ctx){
	memset(orig_ctx, 0x00, sizeof(*orig_ctx));

	if(remote_getregs(ctx, orig_ctx) < 0){
		PERROR("remote_getregs failed (initial)");
		return -1;
	}
	memcpy(new_ctx, orig_ctx, sizeof(*orig_ctx));

	if(setregs_syscall(ctx, orig_ctx, new_ctx, call) < 0){
		ERR("setregs_syscall failed");
		return -1;
	}
	if(remote_setregs(ctx, new_ctx) < 0){
		PERROR("remote_setregs failed");
		return -1;
	}

	return 0;
}

intptr_t remote_call_common(struct ezinj_ctx *ctx, struct call_req *call){
	regs_t orig_ctx, new_ctx;
	if(remote_call_setup(ctx, call, &orig_ctx, &new_ctx) < 0){
		ERR("remote_call_setup failed");
		return -1;
	}

	if(ctx->pl_debug) {
		if(remote_detach(ctx) != 0){
			PERROR("ptrace");
			return -1;
		}
	} else if(remote_continue(ctx, 0) != 0){
		PERROR("ptrace");
		return -1;
	}

	bool wait = false;
	do {
		#ifdef EZ_TARGET_WINDOWS
		// hack
		ctx->r_ezstate_addr = RCALL_FIELD_ADDR(&call->rcall, ezstate);
		#endif

		intptr_t status = remote_wait(ctx, 0);
		if(status < 0){
			ERR("remote_wait failed");
			return -1;
		}

	#ifdef EZ_TARGET_POSIX
	// this may be defined to a runtime call (!!)
	// in that case, it will return the *current* SIGRTMIN, which is not what we want
	#undef SIGRTMIN
	#define SIGRTMIN 32
		#define IS_IGNORED_SIG(x) ((x) == SIGCHLD || (x) == SIGUSR1 || (x) == SIGUSR2 || (x) >= SIGRTMIN)

		wait = false;

		int signal = WSTOPSIG(status);
		DBG("signal: %d", signal);
		/**
		 * some glibc versions use SIGRTMIN for thread management
		 * we need to forward those signals so that
		 * `pthread_create` and `pthread_join`
		 * can work correctly
		 */
		if(!call->syscall_mode && IS_IGNORED_SIG(signal)){
			INFO("forwarding signal %d", signal);
			remote_continue(ctx, signal);
			wait = true;
		}
	#endif
	} while(wait);

	if(ctx->pl_debug){
		return -1;
	}


	if(ctx->rcall_handler_post != NULL){
		if(ctx->rcall_handler_post(ctx, &call->rcall) < 0){
			ERR("rcall_handler_post failed");
			return -1;
		}
	}

	// read the rcall result from the stack
	remote_read(ctx,
		&call->rcall.result,
		RCALL_FIELD_ADDR(&call->rcall, result),
		sizeof(uintptr_t)
	);

#ifdef DEBUG
	DBG("[RET] = %"PRIdPTR, call->rcall.result);
#endif

#ifndef HAVE_REMOTING

	if(remote_getregs(ctx, &new_ctx) < 0){
		ERR("remote_getregs failed (restore)");
		return -1;
	}

	/**
	  * the payload is expected to use its own stack
	  * so we don't restore stack in that case
	  * because the stack could be unmapped
	  */
	if(call->syscall_mode){
		DBG("restoring stack data");
		if(remote_write(ctx,
			call->backup_addr,
			call->backup_data,
			call->backup_size
		) != call->backup_size){
			ERR("failed to restore saved stack data");
		}
	}

	if(remote_setregs(ctx, &orig_ctx)){
		PERROR("remote_setregs failed");
	}

#ifdef DEBUG
	DBG("SP: %p", (void *)((uintptr_t)REG(new_ctx, REG_SP)));
	DBG("PC: %p => %p",
		(void *)call->insn_addr,
		(void *)((uintptr_t)REG(new_ctx, REG_PC)));
#endif
#endif

	return call->rcall.result;
}

intptr_t remote_call(
	struct ezinj_ctx *ctx,
	unsigned int argmask, ...
){
	struct call_req call = {
		.insn_addr = ctx->entry_insn.remote,
		.stack_addr = ctx->pl_stack.remote,
		.syscall_mode = ctx->syscall_mode,
		.argmask = argmask
	};
	DBG("=====================");

	va_list ap;
	va_start(ap, argmask);

	for(int i=0; i<CALL_MAX_ARGS; i++){
		call.argv[i] = (CALL_HAS_ARG(call, i)) ? va_arg(ap, uintptr_t) : 0;
	}

	return remote_call_common(ctx, &call);
}

struct ezinj_str ezstr_new(enum ezinj_str_id id, char *str, size_t *pSize){
	if(!str){
		str = "";
	}

	struct ezinj_str bstr = {
		.id = (enum ezinj_str_id)id,
		.str = str
	};
	/**
	 * align the size of the string entry
	 * since some architectures (e.g. ARMv4)
	 * don't like doing unaligned accesses
	 * and will corrupt memory
	 */
	*pSize = (size_t)WORDALIGN(STRSZ(str));
	return bstr;
}

#ifdef EZ_TARGET_LINUX
#define LIBC_SEARCH "libc"
#else
#define LIBC_SEARCH C_LIBRARY_NAME
#endif

/**
 * @brief default implementation of libc lookup
 *
 * @param ctx
 */
static int libc_init_default(struct ezinj_ctx *ctx){
	char *ignores[] = {"ld-", NULL};

	/**
	 * $FIXME: this code does not belong here.
	 * it should be refactored together with `libc_init_default`
	 */
#ifdef EZ_TARGET_WINDOWS
	OSVERSIONINFO osvi = {
		.dwOSVersionInfoSize = sizeof(osvi)
	};
	if(!GetVersionEx(&osvi)) {
		PERROR("GetVersionEx");
		return 1;
	}
	if(osvi.dwPlatformId != VER_PLATFORM_WIN32_NT){
		// win32, stop here
		return 0;
	}
#endif

	INFO("Looking up " C_LIBRARY_NAME);

	void *h_libc = LIB_OPEN(C_LIBRARY_NAME);
	if(!h_libc){
		ERR("dlopen("C_LIBRARY_NAME") failed: %s", LIB_ERROR());
		return 1;
	}

	ez_addr libc = {
		.local  = (uintptr_t) get_base(getpid(), LIBC_SEARCH, ignores),
		.remote = (uintptr_t) get_base(ctx->target, LIBC_SEARCH, ignores)
	};

	DBGPTR(libc.remote);
	DBGPTR(libc.local);

	if(!libc.local || !libc.remote) {
		ERR("Failed to get libc base");
		return 1;
	}
	ctx->libc = libc;

	{
		void *h_libdl = LIB_OPEN(DL_LIBRARY_NAME);
		if(!h_libdl){
			ERR("dlopen("DL_LIBRARY_NAME") failed: %s", LIB_ERROR());
			return 1;
		}

		/**
		 * $FIXME:
		 * this is platform dependent and is left here as a default. it should be moved to the posix target
		 * this code will try to lookup dlopen/dlsym from libdl (the most common scenario)
		 * there are some platforms where this doesn't work (e.g. Windows), but this lookup isn't fatal
		 **/

		ez_addr libdl = {
			.local = (uintptr_t)get_base(getpid(), "libdl", NULL),
			.remote = (uintptr_t)get_base(ctx->target, "libdl", NULL)
		};
		ctx->libdl = libdl;

		void *dlopen_local = LIB_GETSYM(h_libdl, "dlopen");
		off_t dlopen_offset = (off_t)PTRDIFF(dlopen_local, libdl.local);
		ctx->dlopen_offset = dlopen_offset;

		void *dlclose_local = LIB_GETSYM(h_libdl, "dlclose");
		off_t dlclose_offset = (off_t)PTRDIFF(dlclose_local, libdl.local);
		ctx->dlclose_offset = dlclose_offset;

		void *dlsym_local = LIB_GETSYM(h_libdl, "dlsym");
		off_t dlsym_offset = (off_t)PTRDIFF(dlsym_local, libdl.local);
		ctx->dlsym_offset = dlsym_offset;

		LIB_CLOSE(h_libdl);
	}

	#define USE_LIBC_SYM(name) do { \
	ctx->libc_##name = sym_addr(h_libc, #name, ctx->libc); \
	DBGPTR(ctx->libc_##name.local); \
	DBGPTR(ctx->libc_##name.remote); \
} while(0)

	USE_LIBC_SYM(syscall);
#undef USE_LIBC_SYM

	LIB_CLOSE(h_libc);
	return 0;
}

int libc_init(struct ezinj_ctx *ctx){
	if(libc_init_default(ctx) != 0){
		return 1;
	}

	if(resolve_libc_symbols(ctx) != 0){
		return 1;
	}

	return 0;
}

int push_string(
	enum ezinj_str_id str_id,
	struct ezinj_str **pArgs,
	unsigned *num_strings, unsigned *capacity,
	size_t *dyn_str_size,
	char *str
){
	unsigned index = MAX(str_id, *num_strings);
	if(index >= *capacity){
		// insert index + 128 new items
		*capacity = index + 128;
		*pArgs = realloc(*pArgs, sizeof(struct ezinj_str) * *capacity);
		if(*pArgs == NULL){
			PERROR("realloc");
			return -1;
		}
	}
	size_t str_size = 0;
	(*pArgs)[str_id] = ezstr_new(str_id, str, &str_size);
	*dyn_str_size += str_size;
	return 0;
}

struct injcode_bearing *prepare_bearing(struct ezinj_ctx *ctx, int argc, char *argv[]){
	size_t dyn_ptr_size = argc * sizeof(char *);
	size_t dyn_str_size = 0;

	unsigned argsLim = 128;
	struct ezinj_str *args = calloc(argsLim, sizeof(struct ezinj_str));
	if(!args){
		PERROR("calloc");
		return NULL;
	}

	unsigned num_strings = EZSTR_MAX_DEFAULT;

#define PUSH_STRING(id, str) do { \
	if(push_string(id, &args, &num_strings, &argsLim, &dyn_str_size, str) < 0) { \
		return NULL; \
	} \
} while(0)

#ifdef EZ_TARGET_LINUX
	off_t pl_filename_offset = dyn_str_size;
	/**
	 * construct tempory payload filename
	 * yes, we use tempnam as we don't know
	 * the system temporary directory
	 **/
	char *pl_filename = tempnam(NULL, "ezpl");
	if(pl_filename == NULL){
		PERROR("tmpnam");
		return NULL;
	}

	PUSH_STRING(EZSTR_PL_FILENAME, pl_filename);
#endif

	// library to load
	char libName[PATH_MAX];
#if defined(EZ_TARGET_POSIX)
	if(!realpath(argv[0], libName)) {
		ERR("realpath: %s", libName);
		PERROR("realpath");
		return NULL;
	}
#elif defined(EZ_TARGET_WINDOWS)
	{
		GetFullPathNameA(argv[0], sizeof(libName), libName, NULL);
	}
#endif

	// libdl.so name (without path)
	PUSH_STRING(EZSTR_API_LIBDL, DL_LIBRARY_NAME);
	// libpthread.so name (without path)
	PUSH_STRING(EZSTR_API_LIBPTHREAD, PTHREAD_LIBRARY_NAME);

#if defined(EZ_TARGET_POSIX)
	PUSH_STRING(EZSTR_API_DLERROR, "dlerror");
	PUSH_STRING(EZSTR_API_PTHREAD_MUTEX_INIT, "pthread_mutex_init");
	PUSH_STRING(EZSTR_API_PTHREAD_MUTEX_LOCK, "pthread_mutex_lock");
	PUSH_STRING(EZSTR_API_PTHREAD_MUTEX_UNLOCK, "pthread_mutex_unlock");
	PUSH_STRING(EZSTR_API_COND_INIT, "pthread_cond_init");
	PUSH_STRING(EZSTR_API_COND_WAIT, "pthread_cond_wait");
#elif defined(EZ_TARGET_WINDOWS)
	PUSH_STRING(EZSTR_API_CREATE_EVENT, "CreateEventA");
	PUSH_STRING(EZSTR_API_CREATE_THREAD, "CreateThread");
	PUSH_STRING(EZSTR_API_CLOSE_HANDLE, "CloseHandle");
	PUSH_STRING(EZSTR_API_WAIT_FOR_SINGLE_OBJECT, "WaitForSingleObject");
	PUSH_STRING(EZSTR_API_GET_EXIT_CODE_THREAD, "GetExitCodeThread");
#endif

char logPath[PATH_MAX];
if(ctx->module_logfile && strlen(ctx->module_logfile) > 0){
#if defined(EZ_TARGET_POSIX)
	if(!realpath(ctx->module_logfile, logPath)) {
		ERR("realpath: %s", logPath);
		PERROR("realpath");
		return NULL;
	}
#elif defined(EZ_TARGET_WINDOWS)
	{
		GetFullPathNameA(ctx->module_logfile, sizeof(logPath), logPath, NULL);
	}
#endif
} else {
	strncpy(logPath, "", sizeof(logPath));
}

	PUSH_STRING(EZSTR_API_CRT_INIT, "crt_init");
	PUSH_STRING(EZSTR_LOG_FILEPATH, logPath);

	// argv0
	PUSH_STRING(EZSTR_ARGV0, libName);

	// user arguments
	for(int i=1; i < argc; i++){
		PUSH_STRING(EZSTR_ARGV0+i, argv[i]);
		++num_strings;
	}


#undef PUSH_STRING

	size_t dyn_entries_size = num_strings * sizeof(struct ezinj_str);
	size_t dyn_total_size = dyn_ptr_size + dyn_entries_size + dyn_str_size;
	size_t mapping_size = create_layout(ctx, dyn_total_size, &ctx->pl);

	struct injcode_bearing *br = (struct injcode_bearing *)ctx->mapped_mem.local;
	memset(br, 0x00, sizeof(*br));

	if(!br){
		free(args);
		PERROR("malloc");
		return NULL;
	}
	br->mapping_size = mapping_size;

	br->pl_debug = ctx->pl_debug;

	br->libdl_handle = (void *)ctx->libdl.remote;
#if defined(HAVE_DL_LOAD_SHARED_LIBRARY)
	br->uclibc_sym_tables = (void *)ctx->uclibc_sym_tables.remote;
	br->uclibc_dl_fixup = (void *)ctx->uclibc_dl_fixup.remote;
	br->uclibc_loaded_modules = (void *)ctx->uclibc_loaded_modules.remote;
#ifdef EZ_ARCH_MIPS
	br->uclibc_mips_got_reloc = (void *)ctx->uclibc_mips_got_reloc.remote;
#endif
#endif

#ifdef EZ_TARGET_DARWIN
	br->pthread_create = (void *)ctx->pthread_create.remote;
	br->pthread_join = (void *)ctx->pthread_join.remote;
	br->pthread_create_from_mach_thread = (void *)ctx->pthread_create_from_mach_thread.remote;
	br->pthread_detach = (void *)ctx->pthread_detach.remote;
	br->pthread_self = (void *)ctx->pthread_self.remote;
	br->mach_thread_self = (void *)ctx->mach_thread_self.remote;
	br->thread_terminate = (void *)ctx->thread_terminate.remote;
	br->mach_port_allocate = (void *)ctx->mach_port_allocate.remote;
	br->task_self_trap = (void *)ctx->task_self_trap.remote;
#endif

#ifdef EZ_TARGET_WINDOWS
	br->CreateFileA = (void *)ctx->create_file.remote;
	br->WriteFile = (void *)ctx->write_file.remote;
	br->CloseHandle = (void *)ctx->close_handle.remote;
	br->LdrRegisterDllNotification = (void *)ctx->nt_register_dll_noti.remote;
	br->LdrUnregisterDllNotification = (void *)ctx->nt_unregister_dll_noti.remote;
	br->kernel32_base = ctx->libdl.remote;
#endif

	br->dlopen_offset = ctx->dlopen_offset;
	br->dlclose_offset = ctx->dlclose_offset;
	br->dlsym_offset = ctx->dlsym_offset;

#define USE_LIBC_SYM(name) do { \
	br->libc_##name = (void *)ctx->libc_##name.remote; \
	DBGPTR(br->libc_##name); \
} while(0)

#ifdef EZ_TARGET_POSIX
	USE_LIBC_SYM(dlopen);
	USE_LIBC_SYM(syscall);
#endif

#undef USE_LIBC_SYM

	br->argc = argc;
	br->dyn_total_size = dyn_total_size;
	br->num_strings = num_strings;
#ifdef EZ_TARGET_LINUX
	br->pl_filename_offset = pl_filename_offset;
#endif

	char *stringEntries = (char *)br + sizeof(*br) + dyn_ptr_size;
	char *stringData = stringEntries + dyn_entries_size;

	struct ezinj_str *pEntries = (struct ezinj_str *)stringEntries;
	size_t strtbl_off = 0;
	for(unsigned i=0; i<num_strings; i++){
		pEntries->id = args[i].id;
		// store the current string offset. it will rebased by injcode
		pEntries->str = (char *)strtbl_off;
		++pEntries;

		if(!args[i].str){
			continue;
		}
		size_t str_sz = STRSZ(args[i].str);
		memcpy(stringData, args[i].str, str_sz);
		stringData += str_sz;
		strtbl_off += str_sz;
	}

#ifdef EZ_TARGET_LINUX
	free(pl_filename);
#endif

	// copy code
	memcpy(ctx->pl.code_start, region_pl_code.start, REGION_LENGTH(region_pl_code));
	free(args);
	return br;
}

size_t create_layout(struct ezinj_ctx *ctx, size_t dyn_total_size, struct ezinj_pl *layout){
	// br + argv
	size_t br_size = (size_t)WORDALIGN(sizeof(struct injcode_bearing) + dyn_total_size);
	// size of code payload
	size_t code_size = (size_t)WORDALIGN(REGION_LENGTH(region_pl_code));

	size_t stack_offset = br_size + code_size;
	size_t mapping_size = stack_offset + PL_STACK_SIZE;
	#ifdef EZ_TARGET_WINDOWS
	{
		SYSTEM_INFO sysInfo;
		GetSystemInfo(&sysInfo);
		mapping_size = (size_t)ALIGN(mapping_size, sysInfo.dwPageSize);
	}
	#else
	mapping_size = (size_t)PAGEALIGN(mapping_size);
	#endif

	DBG("br_size=%zu", br_size);
	DBG("code_size=%zu", code_size);
	DBG("stack_offset=%zu", stack_offset);
	DBG("mapping_size=%zu", mapping_size);

	void *mapped_mem = calloc(1, mapping_size);

	ctx->mapped_mem.local = (uintptr_t)mapped_mem;

	/** prepare payload layout **/

	uint8_t *pMem = (uint8_t *)ctx->mapped_mem.local;
	layout->br_start = pMem;
	pMem += br_size;

	layout->code_start = pMem;

	// stack is located at the end of the memory map
	layout->stack_top = (uint8_t *)ctx->mapped_mem.local + mapping_size;

	/** align stack **/

	#if defined(EZ_ARCH_AMD64) || defined(EZ_ARCH_ARM64)
	// x64 requires a 16 bytes aligned stack for movaps
	// force stack to snap to the lowest 16 bytes, or it will crash on x64
	layout->stack_top = (uint8_t *)((uintptr_t)layout->stack_top & ~ALIGNMSK(16));
	#else
	layout->stack_top = (uint8_t *)((uintptr_t)layout->stack_top & ~ALIGNMSK(sizeof(void *)));
	#endif

	return mapping_size;
}

void cleanup_mem(struct ezinj_ctx *ctx){
	free((void *)ctx->mapped_mem.local);
}

void sigint_handler(int signum){
	UNUSED(signum);
	cleanup_mem(&ctx);
}

#if defined(EZ_TARGET_LINUX)
void print_maps(){
	pid_t pid = syscall(__NR_getpid);
	char *path;
	asprintf(&path, "/proc/%u/maps", pid);
	do {
		FILE *fh = fopen(path, "r");
		if(!fh){
			return;
		}

		char line[256];
		while(!feof(fh)){
			fgets(line, sizeof(line), fh);
			fputs(line, stdout);
		}
		fclose(fh);
	} while(0);
	free(path);
}
#else
void print_maps(){}
#endif

static void ezinject_log_init(){
	log_config_t log = {
		.log_leave_open = 1,
		.log_output = stdout,
		.verbosity = V_INFO
	};
	log_init(&log);
}

int ezinject_main(
	struct ezinj_ctx *ctx,
	int argc, char *argv[]
){
	print_maps();

	signal(SIGINT, sigint_handler);

	// prepare bearing
	struct injcode_bearing *br = prepare_bearing(ctx, argc, argv);
	if(br == NULL){
		return -1;
	}


	uintptr_t r_sc_elf = 0;
	uintptr_t r_sc_vmem = 0;

	#if defined(HAVE_SHELLCODE)
	// allocate initial shellcode on the ELF header
	INFO("target: allocating sc");
	if(remote_sc_alloc(ctx, SC_ALLOC_ELFHDR, &r_sc_elf) != 0){
		ERR("remote_sc_alloc: failed to overwrite ELF header");
		return -1;
	}
	remote_sc_set(ctx, r_sc_elf);

	// wait for a single syscall
	ctx->syscall_mode = true;

	/* Verify that remote_call works correctly */
	if(remote_sc_check(ctx) != 0){
		ERR("remote_sc_check failed");
		return -1;
	}
	#endif

	intptr_t err = -1;
	do {
		// creates the new payload area with mmap (invoked from EXEHDR)
		INFO("target: allocating %zu bytes", br->mapping_size);
		uintptr_t remote_shm_ptr = remote_pl_alloc(ctx, br->mapping_size);
		#if defined(EZ_TARGET_LINUX)
		if(remote_shm_ptr == 0){
			// mmap(3) failed. try with mmap(2)
			ctx->force_mmap_syscall = 1;
			WARN("mmap(3) failed, trying mmap(2)");
			remote_shm_ptr = remote_pl_alloc(ctx, br->mapping_size);
		}
		#endif

		if(remote_shm_ptr == 0){
			#if defined(EZ_TARGET_WINDOWS)
			PERROR("VirtualAllocEx failed");
			#else
			ERR("Remote alloc failed: %p", (void *)remote_shm_ptr);
			#endif
		}
		INFO("target: payload base: %p", (void *)remote_shm_ptr);

		ctx->mapped_mem.remote = remote_shm_ptr;

		struct ezinj_pl *pl = &ctx->pl;

		#if defined(EZ_TARGET_LINUX)
		INFO("target: copying payload (using files)");
		if(remote_pl_copy(ctx) != 0){
			ERR("remote_pl_copy failed");
			break;
		}
		#else
		INFO("target: copying payload (using debugger)");
		if(remote_write(ctx, ctx->mapped_mem.remote, (void *)ctx->mapped_mem.local, br->mapping_size) != br->mapping_size){
			PERROR("remote_write failed");
		}
		#endif

		#if !defined(HAVE_REMOTING) && defined(HAVE_SHELLCODE)
		// allocate new shellcode on a new memory map
		// the current shellcode is used for the allocation
		// this must be done before switching to payload mode
		INFO("target: relocating sc");
		if(remote_sc_alloc(ctx, SC_ALLOC_MMAP, &r_sc_vmem) != 0){
			ERR("remote_sc_alloc: mmap failed");
			return -1;
		}
		remote_sc_set(ctx, r_sc_vmem);

		// restore the ELF header
		if(remote_sc_free(ctx, SC_ALLOC_ELFHDR, r_sc_elf) != 0){
			ERR("remote_sc_free: ELF header restore failed");
			return -1;
		}
		#endif

		// switch to SIGSTOP wait mode
		ctx->syscall_mode = false;

		/**
		 * now that PL is available and mapped, we can use it
		 * for stack and entry point
		 **/

		// switch to stack on PL
		uintptr_t *target_sp = (uintptr_t *)pl->stack_top;
		ctx->pl_stack.remote = (uintptr_t)PL_REMOTE(ctx, target_sp);

		// use trampoline on PL
		ctx->entry_insn.remote = PL_REMOTE_CODE(ctx, &trampoline_entry);
		// tell the trampoline to call the main injcode
		ctx->branch_target.remote = PL_REMOTE_CODE(ctx, &injected_fn);

		// init plapi
		#define PLAPI_SET(ctx, fn) ctx->plapi.fn = PL_REMOTE_CODE(ctx, &fn)
		PLAPI_SET(ctx, inj_memset);
		PLAPI_SET(ctx, inj_puts);
		PLAPI_SET(ctx, inj_dchar);
		PLAPI_SET(ctx, inj_dbgptr);
		PLAPI_SET(ctx, inj_fetchsym);
		#undef PLAPI_SET

		// when syscall_mode = false, SC is skipped
		INFO("target: calling payload at %p", pl->br_start);
		err = CHECK(RSCALL0(ctx, PL_REMOTE(ctx, pl->br_start)));

		/**
		 * if payload debugging is on, skip any cleanup
		 **/
		if(ctx->pl_debug){
			return -1;
		}

		// restore syscall behavior (to call munmap, if needed by the target)
		ctx->syscall_mode = true;
		ctx->pl_stack.remote = 0;
		INFO("target: freeing payload memory");
		remote_pl_free(ctx, remote_shm_ptr);

	#if !defined(HAVE_REMOTING) && defined(HAVE_SHELLCODE)
		// switch back to the ELF header, to free vmem
		if(remote_sc_alloc(ctx, SC_ALLOC_ELFHDR, &r_sc_elf) != 0){
			ERR("remote_sc_alloc: failed to overwrite ELF header");
			return -1;
		}
		remote_sc_set(ctx, r_sc_elf);

		// free memory mapped sc
		if(remote_sc_free(ctx, SC_ALLOC_MMAP, r_sc_vmem) != 0){
			ERR("remote_sc_free: failed to free memory map");
			return -1;
		}
	#endif

		// now free the ELF header once more (no syscalls allowed after this point)
		if(remote_sc_free(ctx, SC_ALLOC_ELFHDR, r_sc_elf) != 0){
			ERR("remote_sc_free: ELF header restore failed");
			return -1;
		}
	} while(0);

	return err;
}

int main(int argc, char *argv[]){
	ezinject_log_init();

#ifdef DEBUG
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
#endif

	memset(&ctx, 0x00, sizeof(ctx));

	const char *module_logfile = NULL;

	{
		int c;
		while ((c = getopt (argc, argv, "hdl:v:")) != -1){
			switch(c){
				case 'd':
					WARN("payload debugging enabled, the target **WILL** freeze");
					ctx.pl_debug = 1;
					break;
				case 'l':;
					module_logfile = strdup(optarg);
					break;
				case 'v':;
					switch(toupper(*optarg)){
						case 'D':
							log_set_verbosity(V_DBG);
							break;
					}
					break;
				case 'h':
					goto usage;
			}
		}
	}

	if(argc < 3) {
		usage:
		ERR("Usage: %s [-h|-d|-l <log_path>|-v <verbosity>] <pid> <library.so> [args...]", argv[0]);
		return 1;
	}

	const char *argPid = argv[optind++];
	ctx.target = strtoul(argPid, NULL, 10);
	ctx.module_logfile = (char *)module_logfile;

	if(os_api_init(&ctx) != 0){
		ERR("os_api_init() failed");
		return 1;
	}

	if(libc_init(&ctx) != 0){
		ERR("libc_init() failed");
		return 1;
	}
	ctx.r_xpage_base = (uintptr_t)get_base(ctx.target, NULL, NULL);

	INFO("Attaching to %"PRIuMAX, (uintmax_t)ctx.target);

	if(remote_attach(&ctx) < 0){
		PERROR("remote_attach failed");
		return 1;
	}

	INFO("waiting for target to stop...");

	int err = 0;
#ifndef EZ_TARGET_WINDOWS
	if(remote_wait(&ctx, 0) < 0){
		ERR("remote_wait");
		return 1;
	}
#endif

	err = ezinject_main(&ctx, argc - optind, &argv[optind]);
	/**
	 * due to an eglibc bug, libdl loading will fail even tho it actually worked
	 * if we're targeting linux, try again
	 */
	#ifdef EZ_TARGET_LINUX
	if(err == INJ_ERR_LIBDL){
		cleanup_mem(&ctx);
		if(libc_init(&ctx) != 0){
			return 1;
		}
		err = ezinject_main(&ctx, argc - optind, &argv[optind]);
	}
	#endif


	INFO("detaching...");
	if(remote_detach(&ctx) < 0){
		PERROR("remote_detach failed");
	}

	/**
	 * skip IPC cleanup if we encountered any error
	 * (payload debugging counts as failure)
	 **/
	if(err != 0){
		if(ctx.pl_debug){
			INFO("You may now attach with gdb for payload debugging");
			INFO("Press Enter to quit");
			getchar();
		}
	}

	cleanup_mem(&ctx);
	return (err == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
