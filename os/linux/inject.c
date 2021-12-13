/*
 * Copyright (C) 2021 Stefano Moioli <smxdev4@gmail.com>
 * This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held liable for any damages arising from the use of this software.
 * Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following restrictions:
 *  1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
 *  2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
 *  3. This notice may not be removed or altered from any source distribution.
 */
#include "config.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/ptrace.h>

#include "ezinject_compat.h"
#include "ezinject.h"

#include "log.h"

uintptr_t remote_pl_alloc(struct ezinj_ctx *ctx, size_t mapping_size){
	uintptr_t result = RSCALL6(ctx, __NR_mmap2,
		NULL, mapping_size,
		PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0
	);
	if(result == (uintptr_t)MAP_FAILED){
		return 0;
	}
	return result;
}

static EZAPI _export_pl(struct ezinj_ctx *ctx){
	struct injcode_bearing *br = (struct injcode_bearing *)ctx->mapped_mem.local;

	if(br->pl_filename_offset == 0){
		ERR("pl_filename_offset is not set!");
		return -1;
	}

	char *stbl = BR_STRTBL(br) + br->pl_filename_offset;
	char *pl_filename = STR_DATA(stbl);

	INFO("exporting payload to %s", pl_filename);

	unlink(pl_filename);
	int fd = open(pl_filename, O_WRONLY | O_SYNC | O_CREAT, (mode_t)0666);
	if(fd <= 0){
		PERROR("open");
		return -1;
	}

	if(write(fd, (void *)ctx->mapped_mem.local, br->mapping_size) != br->mapping_size){
		PERROR("write");
		return -1;
	}

	close(fd);
	return 0;
}

EZAPI remote_pl_copy(struct ezinj_ctx *ctx){
	struct injcode_bearing *br = (struct injcode_bearing *)ctx->mapped_mem.local;

	// write payload to file
	if(_export_pl(ctx) != 0){
		ERR("_export_pl failed");
		return -1;
	}

	/**
	 * Copy the string table entry for the remote filename
	 * [entry size][pl filename (NULL terminated)]
	 **/

	char *stbl_entry = BR_STRTBL(br) + br->pl_filename_offset;
	// remote_write always writes in word units
	uintptr_t stbl_entry_size = (uintptr_t)WORDALIGN(STR_ENTSIZE(stbl_entry));
	uintptr_t r_stbl_entry = PL_REMOTE(ctx, stbl_entry);

	/** write the payload filename string table entry */
	size_t written = remote_write(ctx, r_stbl_entry, stbl_entry, stbl_entry_size);
	if(written != stbl_entry_size){
		ERR("remote_write: failed to write pl_filename (%zu != %zu)", written, stbl_entry_size);
		return -1;
	}

	char *pl_filename = STR_DATA(stbl_entry);

	// address of the stbl data entry we just wrote
	uintptr_t r_pl_filename = PL_REMOTE(ctx, pl_filename);

	intptr_t rc = -1;
	do {
		int r_fd = -1;
		#if defined(__NR_open)
		r_fd = RSCALL2(ctx, __NR_open, r_pl_filename, O_RDONLY);
		#elif defined(__NR_openat)
		r_fd = RSCALL3(ctx, __NR_openat, AT_FDCWD, r_pl_filename, O_RDONLY);
		#else
		#error "Unsupported platform"
		#endif

		if(r_fd <= 0){
			ERR("remote open(2) failed");
			break;
		}
		DBG("remote fd: %d", r_fd);

		if((ssize_t)RSCALL3(ctx, __NR_read, r_fd, ctx->mapped_mem.remote, br->mapping_size) != br->mapping_size){
			ERR("remote read(2) failed");
			break;
		}

		if(RSCALL1(ctx, __NR_close, r_fd) < 0){
			ERR("remote close(2) failed");
			break;
		}
		rc = 0;
	} while(0);

	unlink(pl_filename);
	return rc;
}

EZAPI remote_pl_free(struct ezinj_ctx *ctx, uintptr_t remote_shmaddr){
	UNUSED(remote_shmaddr);

	struct injcode_bearing *br = (struct injcode_bearing *)ctx->mapped_mem.local;
	return (intptr_t) CHECK(RSCALL2(ctx, __NR_munmap, ctx->mapped_mem.remote, br->mapping_size));
}

EZAPI remote_sc_check(struct ezinj_ctx *ctx){
	pid_t remote_pid = (pid_t)RSCALL0(ctx, __NR_getpid);
	if(remote_pid != ctx->target){
		ERR("Remote syscall returned incorrect result!");
		ERR("Expected: %u, actual: %u", ctx->target, remote_pid);
		return -1;
	}
	return 0;
}
