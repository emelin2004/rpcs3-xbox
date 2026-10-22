/* Lazily bound system functions. LLVM (and the code that only builds with it) imports desktop
 * Win32 functions that a UWP app on Xbox may not find, and one missing import stops the whole
 * app from loading. Each function below is defined here instead (with its __imp_ pointer, so the
 * import library's entry is never used). The first call looks the real function up in the DLL
 * that is already loaded; if the device doesn't have it, the call gets that API's failure value.
 * Arguments are forwarded untouched (integer and xmm0-3 registers, stack), so any signature works.
 * uwp_lazy_report() resolves them all up front and lists the missing ones for the log. */

#include <string.h>

typedef unsigned long long u64;
typedef unsigned short wchar_t16;

/* uwp_lazyresolve.c (kept apart: windows.h declares these functions with other types) */
void* uwp_lazy_lookup(const wchar_t16* dll, const char* name);

typedef struct lazy_desc
{
	const wchar_t16* dll;
	const char* name;
	void* fallback;
	void* volatile target;
} lazy_desc;

/* Failure values (x64: return value in rax, arguments ignored). */
static u64 lazy_ret_0(void) { return 0; }
static u64 lazy_ret_not_found(void) { return 2; /* ERROR_FILE_NOT_FOUND */ }
static u64 lazy_ret_e_fail(void) { return 0x80004005; }
static const wchar_t16 lazy_empty_cmdline[] = u"rpcs3_uwp.exe";
static u64 lazy_ret_cmdline(void) { return (u64)lazy_empty_cmdline; }

#define K32 u"kernel32.dll"
#define ADV u"advapi32.dll"
#define SH32 u"shell32.dll"

/* dll, name, failure value */
#define UWP_LAZY_LIST(X) \
	X(K32, AssignProcessToJobObject, lazy_ret_0) \
	X(K32, CreateJobObjectW, lazy_ret_0) \
	X(K32, CreateProcessW, lazy_ret_0) \
	X(K32, ExpandEnvironmentStringsW, lazy_ret_0) \
	X(K32, FlushInstructionCache, lazy_ret_0) \
	X(K32, FlushViewOfFile, lazy_ret_0) \
	X(K32, GetCommandLineW, lazy_ret_cmdline) \
	X(K32, GetDriveTypeW, lazy_ret_0) \
	X(K32, GetEnvironmentVariableW, lazy_ret_0) \
	X(K32, GetExitCodeProcess, lazy_ret_0) \
	X(K32, GetLongPathNameW, lazy_ret_0) \
	X(K32, GetProcessGroupAffinity, lazy_ret_0) \
	X(K32, GetSystemDirectoryW, lazy_ret_0) \
	X(K32, GetSystemTime, lazy_ret_0) \
	X(K32, GetThreadGroupAffinity, lazy_ret_0) \
	X(K32, GetVolumePathNameW, lazy_ret_0) \
	X(K32, HeapCompact, lazy_ret_0) \
	X(K32, K32EnumProcessModulesEx, lazy_ret_0) \
	X(K32, K32GetProcessMemoryInfo, lazy_ret_0) \
	X(K32, LoadLibraryExA, lazy_ret_0) \
	X(K32, LockFileEx, lazy_ret_0) \
	X(K32, OpenSemaphoreA, lazy_ret_0) \
	X(K32, RemoveVectoredExceptionHandler, lazy_ret_0) \
	X(K32, SearchPathW, lazy_ret_0) \
	X(K32, SetConsoleCtrlHandler, lazy_ret_0) \
	X(K32, SetConsoleMode, lazy_ret_0) \
	X(K32, SetErrorMode, lazy_ret_0) \
	X(K32, SetInformationJobObject, lazy_ret_0) \
	X(K32, SetThreadGroupAffinity, lazy_ret_0) \
	X(K32, SystemTimeToFileTime, lazy_ret_0) \
	X(K32, TerminateProcess, lazy_ret_0) \
	X(K32, UnlockFileEx, lazy_ret_0) \
	X(ADV, AdjustTokenPrivileges, lazy_ret_0) \
	X(ADV, CryptAcquireContextW, lazy_ret_0) \
	X(ADV, LookupPrivilegeValueA, lazy_ret_0) \
	X(ADV, OpenProcessToken, lazy_ret_0) \
	X(ADV, RegGetValueW, lazy_ret_not_found) \
	X(SH32, ILCreateFromPathW, lazy_ret_0) \
	X(SH32, ILFree, lazy_ret_0) \
	X(SH32, SHCreateItemFromIDList, lazy_ret_e_fail) \
	X(SH32, SHGetKnownFolderPath, lazy_ret_e_fail)

void* uwp_lazy_resolve(lazy_desc* d)
{
	void* fn = d->target;
	if (fn)
		return fn;

	fn = uwp_lazy_lookup(d->dll, d->name);
	d->target = fn ? fn : d->fallback;
	return d->target;
}

/* Thunk: jump to the bound function; on the first call save the argument registers and bind. */
#define UWP_LAZY_DEFINE(dll, fn, fb) \
	lazy_desc uwp_lazy_##fn = {dll, #fn, (void*)&fb, 0}; \
	extern void fn(void); \
	void* __imp_##fn = (void*)&fn; \
	__asm__(".text\n.globl " #fn "\n.p2align 4\n" #fn ":\n" \
		"\tmovq uwp_lazy_" #fn "+24(%rip), %rax\n\ttestq %rax, %rax\n\tjz 1f\n\tjmp *%rax\n" \
		"1:\tpushq %rcx\n\tpushq %rdx\n\tpushq %r8\n\tpushq %r9\n\tsubq $0x68, %rsp\n" \
		"\tmovdqu %xmm0, 0x20(%rsp)\n\tmovdqu %xmm1, 0x30(%rsp)\n\tmovdqu %xmm2, 0x40(%rsp)\n\tmovdqu %xmm3, 0x50(%rsp)\n" \
		"\tleaq uwp_lazy_" #fn "(%rip), %rcx\n\tcall uwp_lazy_resolve\n" \
		"\tmovdqu 0x20(%rsp), %xmm0\n\tmovdqu 0x30(%rsp), %xmm1\n\tmovdqu 0x40(%rsp), %xmm2\n\tmovdqu 0x50(%rsp), %xmm3\n" \
		"\taddq $0x68, %rsp\n\tpopq %r9\n\tpopq %r8\n\tpopq %rdx\n\tpopq %rcx\n\tjmp *%rax\n");

UWP_LAZY_LIST(UWP_LAZY_DEFINE)

#define UWP_LAZY_REF(dll, fn, fb) &uwp_lazy_##fn,
static lazy_desc* const lazy_all[] = {UWP_LAZY_LIST(UWP_LAZY_REF)};

/* Bind every function now; writes the names of the missing ones (comma separated) to out. */
void uwp_lazy_report(char* out, size_t size)
{
	size_t pos = 0;
	out[0] = 0;
	for (size_t i = 0; i < sizeof(lazy_all) / sizeof(lazy_all[0]); i++)
	{
		lazy_desc* d = lazy_all[i];
		if (uwp_lazy_resolve(d) != d->fallback)
			continue;
		const size_t n = strlen(d->name);
		if (pos + n + 3 >= size)
			break;
		if (pos)
		{
			out[pos++] = ',';
			out[pos++] = ' ';
		}
		memcpy(out + pos, d->name, n + 1);
		pos += n;
	}
}
