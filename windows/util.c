#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include <windows.h>
#include <psapi.h>
#include <shlwapi.h>

#include "log.h"

BOOL win32_errstr(DWORD dwErrorCode, LPTSTR pBuffer, DWORD cchBufferLength){
	if(cchBufferLength == 0){
		return FALSE;
	}

	DWORD cchMsg = FormatMessage(
		FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL,
		dwErrorCode,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		pBuffer,
		cchBufferLength,
		NULL
	);

	if(cchMsg > 0){
		char *nl = strrchr(pBuffer, '\n');
		if(nl){
			*nl = '\0';
			if(*(nl-1) == '\r'){
				*(nl - 1) = '\0';
			}
		}
		return TRUE;
	}

	return FALSE;
}

int strncasecmp(const char *s1, const char *s2, size_t n) {
	if (n != 0) {
		const unsigned char
				*us1 = (const unsigned char *)s1,
				*us2 = (const unsigned char *)s2;

		do {
			if (tolower(*us1) != tolower(*us2++))
				return (tolower(*us1) - tolower(*--us2));
			if (*us1++ == '\0')
				break;
		} while (--n != 0);
	}
	return (0);
}

char *strcasestr(const char *s, const char *find) {
	char c, sc;
	size_t len;

	if ((c = *find++) != 0) {
		c = tolower((unsigned char)c);
		len = strlen(find);
		do {
			do {
				if ((sc = *s++) == 0)
					return (NULL);
			} while ((char)tolower((unsigned char)sc) != c);
		} while (strncasecmp(s, find, len) != 0);
		s--;
	}
	return ((char *)s);
}

static bool is_same_file(const char *pathA, const char *pathB){
	HANDLE fileA = CreateFileA(pathA, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if(fileA == INVALID_HANDLE_VALUE){
		ERR("CreateFileA failed");
		return false;
	}

	HANDLE fileB = CreateFileA(pathB, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if(fileB == INVALID_HANDLE_VALUE){
		CloseHandle(fileA);
		ERR("CreateFileA failed");
		return false;
	}

	BY_HANDLE_FILE_INFORMATION infoA, infoB;
	GetFileInformationByHandle(fileA, &infoA);
	GetFileInformationByHandle(fileB, &infoB);

	bool identical = (
		infoA.dwVolumeSerialNumber == infoB.dwVolumeSerialNumber &&
		infoA.nFileIndexLow == infoB.nFileIndexLow &&
		infoA.nFileIndexHigh == infoB.nFileIndexHigh
	);

	CloseHandle(fileA);
	CloseHandle(fileB);

	return identical;
}

void *get_base(pid_t pid, char *substr, char **ignores) {
	HANDLE hProcess;

	hProcess = OpenProcess( PROCESS_QUERY_INFORMATION |
                            PROCESS_VM_READ,
                            FALSE, pid );
	if (NULL == hProcess){
		ERR("OpenProcess() failed");
		return NULL;
	}					

	bool found = false;
	void *base = NULL;

	do {
		HMODULE *modules = NULL;
		TCHAR imageFileName[MAX_PATH];
		unsigned pathSize = _countof(imageFileName);
		if(!QueryFullProcessImageNameA(hProcess, 0, imageFileName, &pathSize)){
			DBG("QueryFullProcessImageNameA failed");
			break;
		}

		DBG("imageFileName: %s", imageFileName);

		int numModules = 0;
		{
			uint32_t bytesNeeded = 0;
			EnumProcessModules(hProcess, NULL, 0, &bytesNeeded);
			
			modules = calloc(1, bytesNeeded);
			EnumProcessModules(hProcess, modules, bytesNeeded, &bytesNeeded);

			numModules = bytesNeeded / sizeof(HMODULE);
		}

		for(int i=0; i<numModules; i++){
			TCHAR modName[MAX_PATH];
			if(!GetModuleFileNameEx(hProcess, modules[i], modName, _countof(modName))){
				continue;
			}

			DBG("%p -> %s", modules[i], modName);

			if(
				(substr == NULL && !_stricmp(imageFileName, modName)) ||
				(substr != NULL && strcasestr(modName, substr) != NULL)
			){
				base = (void *)modules[i];
				break;
			}
		}
		free(modules);
	} while(0);
	CloseHandle(hProcess);

	return base;
}