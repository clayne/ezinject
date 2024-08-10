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
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/syslimits.h>

#include "ezinject.h"
#include "ezinject_common.h"
#include "log.h"

EZAPI os_api_init(struct ezinj_ctx *ctx){
	return 0;
}

static bool str_empty(char *str){
	int l = strlen(str);
	for(int i=0;i<l;i++){
		char ch = str[i];
		if(!isspace(ch) && !iscntrl(ch) && isprint(ch)){
			return false;
		}
	}
	return true;
}

static bool parse_line(
	char *line,
	char **pOutRegionName, void **pOutRegionStart, void **pOutRegionEnd, char **pOutProt
){
	void *regionStart = NULL;
	void *regionEnd = NULL;
	if(sscanf(line, "%*s %lx-%lx",
		(unsigned long *)&regionStart,
		(unsigned long *)&regionEnd
	) != 2){
		return false;
	}

	//__TEXT   7fff203ab000-7fff203e6000 [  236K   228K     0K     0K] r-x/r-x SM=COW   /usr/lib/system/libdyld.dylib
	char *regionName = NULL;
	char *prot = NULL;
	{
		char *tmp = strchr(line, '[');
		if(tmp == NULL){
			return false;
		}
		tmp = strchr(tmp, ']');
		if(tmp == NULL){
			return false;
		}
		// skip '] '
		tmp += 2;
		prot = tmp;
	}
	{
		char *tmp = strrchr(line, ' ');
		if(tmp++ == NULL){
			return false;
		}
		if(!str_empty(tmp)){
			char *nl = strchr(tmp, '\n');
			if(nl != NULL){
				*nl = '\0';
			}
			regionName = tmp;
		}
	}

	// prot is in the format "rwx/rwx", as in cur/max protections
	// truncate the string to keep the current protection bits only
	prot[3] = '\0';

	*pOutRegionName = regionName;
	*pOutRegionStart = regionStart;
	*pOutRegionEnd = regionEnd;
	*pOutProt = prot;
	return true;
}

void *get_base(pid_t pid, char *substr, char **ignores) {
	UNUSED(ignores);

	/**
	 * this is dirty, but doing it properly seems to require using the
	 * undocumented PrivateFramework "Symbolication"
	 * using mach_vm_region_recurse will *NOT* contain all information
	 **/
	char cmd[128];
	snprintf(cmd, sizeof(cmd), "vmmap -w -noCoalesce -noMalloc -interleaved -excludePersonalInfo %u", pid);

	void *h = popen(cmd, "r");
	if(!h){
		// failure to spawn vmmap
		return NULL;
	}

	void *base = NULL;
	char line[256];
	bool inside = false;
	while(!feof(h)){
		memset(line, 0x00, sizeof(line));
		fgets(line, sizeof(line), h);

		if(!inside){
			if(strstr(line, "==== regions for process") == NULL){
				continue;
			}
			inside = true;

			// this should never happen
			if(feof(h)){
				break;
			}
			// throw away the header
			fgets(line, sizeof(line), h);
			continue;
		}

		char *regionName = NULL;
		void *regionStart = NULL;
		void *regionEnd = NULL;
		char *prot = NULL;
		if(parse_line(line, &regionName, &regionStart, &regionEnd, &prot) == false){
			continue;
		}

		if(substr == NULL){
			if(strstr(prot, "x") != NULL){
				base = regionStart;
				break;
			}
		} else if(regionName != NULL && strstr(regionName, substr) != NULL){
			base = regionStart;
			break;
		}

		/** use the first empty line as a marker to stop **/
		if(str_empty(line)){
			break;
		}
	}
	pclose(h);

	return base;
}
