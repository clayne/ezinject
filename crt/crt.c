/*
 * Copyright (C) 2021 Stefano Moioli <smxdev4@gmail.com>
 * This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held liable for any damages arising from the use of this software.
 * Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following restrictions:
 *  1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
 *  2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
 *  3. This notice may not be removed or altered from any source distribution.
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <sched.h>
#include <pthread.h>
#include <unistd.h>

#ifdef EZ_TARGET_LINUX
#include <sys/prctl.h>
#endif
#ifdef EZ_TARGET_POSIX
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#endif
#ifdef EZ_TARGET_FREEBSD
#include <sys/sysproto.h>
#endif
#ifdef EZ_TARGET_LINUX
#include <asm/unistd.h>
#endif

#ifdef EZ_TARGET_WINDOWS
#include <windows.h>
#endif

#include <sys/stat.h>
#include <fcntl.h>

#include <pthread.h>

#include "ezinject.h"
#include "ezinject_common.h"
#include "ezinject_injcode.h"
#include "ezinject_compat.h"
#include "ezinject_module.h"

#include "log.h"

#ifdef DEBUG
#include "ezinject_util.h"
#endif

#ifndef MODULE_NAME
#define MODULE_NAME "userlib"
#endif

#define UNUSED(x) (void)(x)

#ifdef UCLIBC_OLD
#include "crt_uclibc.c"
#endif

#include "crt.h"

void *crt_userlib_handle;

extern int crt_userinit(struct injcode_bearing *br);
WINAPI void* crt_user_entry(void *arg);

#if 0
#ifdef EZ_TARGET_WINDOWS
static void _init_stdout(){
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	AllocConsole();
	freopen("CONIN$", "r", stdin);
	freopen("CONOUT$", "w", stderr);
	freopen("CONOUT$", "w", stdout);

	HANDLE hConOut = CreateFile(
		TEXT("CONOUT$"),
		GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, NULL);

	SetStdHandle(STD_OUTPUT_HANDLE, hConOut);
	SetStdHandle(STD_ERROR_HANDLE, hConOut);
}
#endif
#endif

static const char *crt_get_log_filepath(struct injcode_bearing *br){
	const char *log_path = BR_STRTBL(br)[EZSTR_LOG_FILEPATH].str;
	if(strlen(log_path) > 0){
		return log_path;
	}
	return NULL;
}

static void crt_loginit(struct injcode_bearing *br, log_config_t *cfg_out){
	FILE *log_handle = stdout;
	const char *log_file_path = crt_get_log_filepath(br);
	if(log_file_path){
		FILE *new_log_handle = fopen(log_file_path, "a");
		if(new_log_handle){
			log_handle = new_log_handle;
		} else {
			fprintf(stderr, "fopen '%s' failed\n", log_file_path);
		}
	}
	log_config_t log_cfg = {
		.verbosity = V_DBG,
		.log_leave_open = log_handle == stdout,
		.log_output = log_handle,
		.buffered = false
	};
	memcpy(cfg_out, &log_cfg, sizeof(log_cfg));
}

DLLEXPORT int crt_init(struct injcode_bearing *br){
	log_config_t crt_log_cfg;
	crt_loginit(br, &crt_log_cfg);
	log_init(&crt_log_cfg);

	INFO("library loaded!");

	log_config_t log_cfg;
	memcpy(&log_cfg, &crt_log_cfg, sizeof(crt_log_cfg));

	INFO("calling lib_loginit");
	if(lib_loginit(&log_cfg) == 0){
		log_init(&log_cfg);
	}

	INFO("library loaded! (after lib_loginit)");
	INFO("initializing");

	// workaround for old uClibc (see http://lists.busybox.net/pipermail/uclibc/2009-October/043122.html)
	// https://github.com/kraj/uClibc/commit/cfa1d49e87eae4d46e0f0d568627b210383534f3
	#ifdef UCLIBC_OLD
	uclibc_fixup_pthread();
	#endif

	DBG("crt_thread_create");
	if(crt_thread_create(br, crt_user_entry) < 0){
		ERR("crt_thread_create failed");
		return -1;
	}
	return 0;
}

/**
 * User thread: runs on a new stack created by pthread_create
 **/
WINAPI void *crt_user_entry(void *arg) {
	struct injcode_bearing *br = arg;

	crt_userlib_handle = br->userlib;

	// prepare argv
	char **dynPtr = &br->argv[0];

	struct ezinj_str *stbl = &BR_STRTBL(br)[EZSTR_ARGV0];
	for(int i=0; i<br->argc; i++, stbl++){
		*(dynPtr++) = (char *)stbl->str;
	}

#ifdef DEBUG
	hexdump(br, SIZEOF_BR(*br));
#endif

	int result = crt_userinit(br);

	br->thread_exit_code = result;

	DBG("crt_thread_notify");
	if(crt_thread_notify(br) < 0){
		ERR("crt_thread_notify failed");
	}
	DBG("ret");
	log_fini();
	return NULL;
}
