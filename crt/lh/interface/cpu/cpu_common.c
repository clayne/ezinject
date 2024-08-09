/*
 * Copyright (C) 2021 Stefano Moioli <smxdev4@gmail.com>
 * This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held liable for any damages arising from the use of this software.
 * Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following restrictions:
 *  1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
 *  2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
 *  3. This notice may not be removed or altered from any source distribution.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "ezinject_util.h"

#include "interface/if_hook.h"
#include "interface/if_cpu.h"

#include "ezinject_common.h"
#include "config.h"

#ifdef EZ_TARGET_POSIX
#include <sys/mman.h>
#endif

size_t inj_getjmp_size(){
	#ifdef LH_JUMP_ABS
		return inj_absjmp_opcode_bytes();
	#else
		return inj_reljmp_opcode_bytes();
	#endif
}

uint8_t *inj_build_jump(void *dstAddr, void *srcAddr, size_t *jumpSzPtr){
	size_t jumpSz = inj_getjmp_size(dstAddr);
	uint8_t *buffer = calloc(jumpSz, 1);
	if(!buffer)
		return NULL;
	#ifdef LH_JUMP_ABS
		if(inj_build_abs_jump(buffer, dstAddr, srcAddr) != 0)
			goto error;
	#else
		if(inj_build_rel_jump(buffer, dstAddr, srcAddr) != 0)
			goto error;
	#endif
	if(jumpSzPtr)
		*jumpSzPtr = jumpSz;

	if(verbosity > 3){
		INFO("jump");
		hexdump(buffer, jumpSz);
	}
	return buffer;
	error:
		free(buffer);
		return NULL;
}

#ifdef USE_CAPSTONE
int inj_getinsn_count(void *buf, size_t sz, unsigned int *validbytes){
	csh handle;
	cs_insn *insn;
	#if defined EZ_ARCH_I386
	if (cs_open(CS_ARCH_X86, CS_MODE_32, &handle) != CS_ERR_OK)
	#elif defined EZ_ARCH_AMD64
	if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) != CS_ERR_OK)
	#elif defined EZ_ARCH_ARM
	if (cs_open(CS_ARCH_ARM, CS_MODE_LITTLE_ENDIAN, &handle) != CS_ERR_OK)
	#endif
		goto err_open;

	size_t count, i;
	count = cs_disasm(handle, buf, sz, 0x0, 0, &insn);
	if((ssize_t)count < 0)
		goto err_disasm;

	if(validbytes == NULL)
		goto ret;

	*validbytes = 0;
	for(i=0; i<count; i++){
		*validbytes += insn[i].size;
	}

	ret:
		cs_free(insn, count);
		return count;

	err_open:
		ERR("cs_open failed!");
		return -1;
	err_disasm:
		ERR("cs_disasm failed!");
		cs_close(&handle);
		return -1;
}
#endif

int inj_getbackup_size(void *codePtr, unsigned int payloadSz){
#ifdef USE_CAPSTONE
	unsigned int totalBytes = 0;
	int total_insn = inj_getinsn_count(codePtr, payloadSz, &totalBytes);
	if(total_insn <= 0 || totalBytes == 0)
		return -1;
	unsigned int dasmSize = payloadSz;
	while(totalBytes < payloadSz){
		total_insn += inj_getinsn_count(codePtr, ++dasmSize, &totalBytes);
		DBG("VALID: %u  REQUIRED: %u", totalBytes, payloadSz);
	}
	DBG("Instruction Count: %d (size: %u)", total_insn, totalBytes);
	return totalBytes;
#else
	unsigned i = 0;
	int opSz;
	if((opSz = inj_opcode_bytes()) > 0){ //fixed opcode size
		while(i < payloadSz)
			i += opSz;
		return i;
	} else { //dynamic opcode size
		UNUSED(codePtr);
		return -1;
	}
#endif
}

/*
 * Relocates code pointed by codePtr from sourcePC to destPC
 */
#if !defined(__i386__) && !defined(__x86_64__)
int inj_relocate_code(void *codePtr, unsigned int codeSz, void *sourcePC, void *destPC){
	/* Not yet implemented for other arches */
	UNUSED(codePtr);
	UNUSED(codeSz);
	UNUSED(sourcePC);
	UNUSED(destPC);
	return 0;
}
#endif

#ifdef EZ_ARCH_ARM
void *inj_code_addr(void *func_addr){
	return (void *)((uintptr_t)func_addr & ~1);
}
#else
void *inj_code_addr(void *func_addr){
	return func_addr;
}
#endif

/*
 * Same as needle variant, but we don't need to copy data back and forth
 */
void *inj_backup_function(void *original_code, size_t *num_saved_bytes, int opcode_bytes_to_restore){
	if(original_code == NULL){
		ERR("ERROR: Code Address not specified");
		return NULL;
	}

	void *code_addr = inj_code_addr(original_code);

	int num_opcode_bytes;
	if(opcode_bytes_to_restore > -1){
		// User specified bytes to save manually
		num_opcode_bytes = opcode_bytes_to_restore;
	} else {
		// Calculate amount of bytes to save (important for Intel, variable opcode size)
		num_opcode_bytes = inj_getbackup_size(code_addr, inj_getjmp_size());
	}

	if(num_opcode_bytes < 0){
		ERR("Cannot determine number of opcode bytes to save");
		WARN("Code size of %d bytes (LHM_NF_COPY_BYTES) may be too small", LHM_FN_COPY_BYTES);
		num_opcode_bytes = LHM_FN_COPY_BYTES;
	}
	INFO("Opcode bytes to save: %d", num_opcode_bytes);

	void *jump_origin = (void *)(UPTR(original_code) + num_opcode_bytes);

	size_t jumpSz;
	uint8_t *jump_back;			//custom -> original
	// JUMP from Replacement back to Original code (skip the original bytes that have been replaced to avoid loop)
	void *jump_target = (void *)PTRADD(original_code, num_opcode_bytes);
	if(!(jump_back = inj_build_jump(jump_target, jump_origin, &jumpSz))){
		ERR("Cannot build jump to %p", jump_target);
		return NULL;
	}

	// Allocate space for the payload (code size + jump back)
	// Unlike needle variant, we call mmap here, as we're in the user process
	size_t payloadSz = num_opcode_bytes + jumpSz;

	void *pMem = NULL;
#if defined(EZ_TARGET_POSIX)
	pMem = mmap(0, payloadSz, PROT_EXEC | PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if(pMem == MAP_FAILED){
		PERROR("mmap");
		return NULL;
	}
#elif defined(EZ_TARGET_WINDOWS)
	pMem = calloc(1, payloadSz);
	DWORD oldProtect = 0;
	if(!VirtualProtect(pMem, payloadSz, PAGE_EXECUTE_READWRITE, &oldProtect)){
		ERR("VirtualProtect failed");
		return NULL;
	}
#else
#error "Unsupported Target"
#endif
	if(pMem == NULL){
		ERR("malloc failed");
		return NULL;
	}
	uint8_t *remote_code = (uint8_t *)pMem;

	memcpy(remote_code, code_addr, num_opcode_bytes);
	// Make sure code doesn't contain any PC-relative operation once moved to the new location
	inj_relocate_code(remote_code, num_opcode_bytes, original_code, pMem);
	memcpy(remote_code + num_opcode_bytes, jump_back, jumpSz);

	if(num_saved_bytes){
		*num_saved_bytes = num_opcode_bytes;
	}

	if(code_addr != original_code){
		// assume ARM thumb
		return (void *)(UPTR(pMem) | 1);
	}

	return pMem;
}
