struct injcode_bearing
{
	void (*libc_dlopen_mode)(const char *name, int mode);
	long (*libc_syscall)(long number, ...);
	char libname[128];
};

extern __attribute__((naked)) void injected_code(void);
extern __attribute__((naked)) void injected_code_end(void);
