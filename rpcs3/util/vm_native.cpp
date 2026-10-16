#include "stdafx.h"
#include "util/vm.hpp"
#include "util/asm.hpp"
#ifdef _WIN32
#include "Utilities/File.h"
#include "util/dyn_lib.hpp"
#include "util/logs.hpp"
#include "Utilities/lockless.h"
#include <Windows.h>
#include <span>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#endif

#if defined(__FreeBSD__)
#include <sys/sysctl.h>
#include <vm/vm_param.h>
#endif

#ifdef __linux__
#include <sys/syscall.h>
#include <linux/memfd.h>

#ifdef __NR_memfd_create
#elif __x86_64__
#define __NR_memfd_create 319
#elif ARCH_ARM64
#define __NR_memfd_create 279
#endif

static int memfd_create_(const char *name, uint flags)
{
	return syscall(__NR_memfd_create, name, flags);
}
#elif defined(__FreeBSD__)
# if __FreeBSD__ < 13
// XXX Drop after FreeBSD 12.* reaches EOL on 2024-06-30
#define MFD_CLOEXEC O_CLOEXEC
#define memfd_create_(name, flags) shm_open(SHM_ANON, O_RDWR | flags, 0600)
# else
#define memfd_create_ memfd_create
# endif
#endif

namespace utils
{
#ifdef MAP_NORESERVE
	constexpr int c_map_noreserve = MAP_NORESERVE;
#else
	[[maybe_unused]] constexpr int c_map_noreserve = 0;
#endif

#ifdef MADV_FREE
	[[maybe_unused]] constexpr int c_madv_free = MADV_FREE;
#elif defined(MADV_DONTNEED)
	[[maybe_unused]] constexpr int c_madv_free = MADV_DONTNEED;
#else
	[[maybe_unused]] constexpr int c_madv_free = 0;
#endif

#ifdef MADV_HUGEPAGE
	constexpr int c_madv_hugepage = MADV_HUGEPAGE;
#else
	[[maybe_unused]] constexpr int c_madv_hugepage = 0;
#endif

#if defined(MADV_DONTDUMP) && defined(MADV_DODUMP)
	constexpr int c_madv_no_dump = MADV_DONTDUMP;
	constexpr int c_madv_dump = MADV_DODUMP;
#elif defined(MADV_NOCORE) && defined(MADV_CORE)
	constexpr int c_madv_no_dump = MADV_NOCORE;
	constexpr int c_madv_dump = MADV_CORE;
#else
	[[maybe_unused]] constexpr int c_madv_no_dump = 0;
	[[maybe_unused]] constexpr int c_madv_dump = 0;
#endif

#if defined(MFD_HUGETLB) && defined(MFD_HUGE_2MB)
	constexpr int c_mfd_huge_2mb = MFD_HUGETLB | MFD_HUGE_2MB;
#elif defined(__linux__) || defined(__FreeBSD__)
	constexpr int c_mfd_huge_2mb = 0;
#endif

#if defined(_WIN32) && defined(RPCS3_UWP)
	LOG_CHANNEL(uwp_mem_log, "UWPMEM");

	// UWP apps may only create executable memory through the *FromApp APIs (with the
	// codeGeneration capability). Executable views of a file mapping are refused (tested:
	// ERROR_INVALID_PARAMETER), so map them writable first and then change protection.
	namespace uwp_mem
	{
		constexpr DWORD exec_mask = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

		inline PVOID virtual_alloc(PVOID addr, SIZE_T size, DWORD type, DWORD prot)
		{
			if (!(prot & exec_mask))
				return ::VirtualAllocFromApp(addr, size, type, prot);

			PVOID ret = ::VirtualAllocFromApp(addr, size, type, PAGE_READWRITE);
			ULONG old;
			if (ret && (type & MEM_COMMIT) && !::VirtualProtectFromApp(ret, size, prot, &old))
			{
				// JIT code (asmjit, LLVM) can't run without this; say so before the caller's ensure() fails.
				const DWORD err = ::GetLastError();
				uwp_mem_log.error("VirtualProtectFromApp(%p, 0x%x, 0x%x) for JIT memory failed (%u)", ret, size, prot, err);
				::SetLastError(err);
				return nullptr;
			}
			return ret;
		}

		inline BOOL virtual_protect(PVOID addr, SIZE_T size, DWORD prot, PDWORD old)
		{
			return ::VirtualProtectFromApp(addr, size, prot, reinterpret_cast<PULONG>(old));
		}

		inline HANDLE create_file_mapping(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD size_hi, DWORD size_lo, LPCWSTR name)
		{
			return ::CreateFileMappingFromApp(file, sa, prot, (ULONG64{size_hi} << 32) | size_lo, name);
		}

		inline PVOID map_view_3(HANDLE h, HANDLE process, PVOID base, ULONG64 off, SIZE_T size, ULONG type, ULONG prot, MEM_EXTENDED_PARAMETER* params, ULONG count)
		{
			// UWP can't map views as executable directly: map them as data, then change the protection.
			const ULONG data_prot = !(prot & exec_mask) ? prot : (prot == PAGE_EXECUTE_WRITECOPY ? PAGE_WRITECOPY : PAGE_READWRITE);
			PVOID ret = ::MapViewOfFile3FromApp(h, process, base, off, size, type, data_prot, params, count);

			if (!ret && data_prot == PAGE_WRITECOPY)
			{
				// Copy-on-write views of large sparse files may be refused; fall back to a shared view.
				const DWORD err = ::GetLastError();
				ret = ::MapViewOfFile3FromApp(h, process, base, off, size, type, PAGE_READWRITE, params, count);
				uwp_mem_log.warning("copy-on-write view at %p (0x%x bytes) failed (%u), shared view: %p", base, size, err, ret);
			}

			if (!ret)
			{
				const DWORD err = ::GetLastError();
				uwp_mem_log.error("MapViewOfFile3FromApp(base=%p, off=0x%x, size=0x%x, type=0x%x, prot=0x%x) failed (%u)",
					base, off, size, type, data_prot, err);
				::SetLastError(err);
				return nullptr;
			}

			ULONG old;
			if (data_prot != prot && !::VirtualProtectFromApp(ret, size, prot, &old))
			{
				const DWORD err = ::GetLastError();
				uwp_mem_log.error("VirtualProtectFromApp(%p, 0x%x, 0x%x) on a view failed (%u)", ret, size, prot, err);
				::UnmapViewOfFile2(process, ret, (type & MEM_REPLACE_PLACEHOLDER) ? MEM_PRESERVE_PLACEHOLDER : 0);
				::SetLastError(err);
				return nullptr;
			}
			return ret;
		}

		inline PVOID map_view_ex(HANDLE h, DWORD access, DWORD off_hi, DWORD off_lo, SIZE_T size, PVOID base)
		{
			// Same access-to-protection rules as kernelbase's MapViewOfFileEx.
			const bool exec = access & FILE_MAP_EXECUTE;
			ULONG prot;
			if (access & FILE_MAP_COPY)
				prot = exec ? PAGE_EXECUTE_WRITECOPY : PAGE_WRITECOPY;
			else if (access & FILE_MAP_WRITE)
				prot = exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
			else if (access & FILE_MAP_READ)
				prot = exec ? PAGE_EXECUTE_READ : PAGE_READONLY;
			else
				prot = PAGE_NOACCESS;
			return map_view_3(h, ::GetCurrentProcess(), base, (ULONG64{off_hi} << 32) | off_lo, size, 0, prot, nullptr, 0);
		}
	}

#define VirtualAlloc utils::uwp_mem::virtual_alloc
#define VirtualProtect utils::uwp_mem::virtual_protect
#define CreateFileMappingW utils::uwp_mem::create_file_mapping
#define MapViewOfFileEx utils::uwp_mem::map_view_ex

	inline const auto VirtualAlloc2 = &::VirtualAlloc2FromApp;
	inline const auto MapViewOfFile3 = &uwp_mem::map_view_3;
	inline const auto UnmapViewOfFile2 = &::UnmapViewOfFile2;

	bool has_win10_memory_mapping_api()
	{
		return true;
	}
#elif defined(_WIN32)
	DYNAMIC_IMPORT("KernelBase.dll", VirtualAlloc2, PVOID(HANDLE Process, PVOID Base, SIZE_T Size, ULONG AllocType, ULONG Prot, MEM_EXTENDED_PARAMETER*, ULONG));
	DYNAMIC_IMPORT("KernelBase.dll", MapViewOfFile3, PVOID(HANDLE Handle, HANDLE Process, PVOID Base, ULONG64 Off, SIZE_T ViewSize, ULONG AllocType, ULONG Prot, MEM_EXTENDED_PARAMETER*, ULONG));
	DYNAMIC_IMPORT("KernelBase.dll", UnmapViewOfFile2, BOOL(HANDLE Process, PVOID BaseAddress, ULONG UnmapFlags));

	bool has_win10_memory_mapping_api()
	{
		return VirtualAlloc2 && MapViewOfFile3 && UnmapViewOfFile2;
	}
#endif
#ifdef _WIN32

	struct map_info_t
	{
		u64 addr = 0;
		u64 size = 0;
		atomic_t<u8> state{};
	};

	lf_array<map_info_t, 32> s_is_mapping{};

	bool is_memory_mappping_memory(u64 addr)
	{
		if (!addr)
		{
			return false;
		}

		return s_is_mapping.for_each([addr](const map_info_t& info)
		{
			if (info.state == 1)
			{
				if (addr >= info.addr && addr < info.addr + info.size)
				{
					return true;
				}
			}

			return false;
		}).second;
	}

	u64 unmap_mappping_memory(u64 addr, u64 size)
	{
		if (!addr || !size)
		{
			return 0;
		}

		return s_is_mapping.for_each([addr, size](map_info_t& info) -> u64
		{
			if (info.state == 1)
			{
				if (addr == info.addr && size == info.size)
				{
					if (info.state.compare_and_swap_test(1, 0))
					{
						return info.size;
					}
				}
			}

			return 0;
		}).second;
	}

	bool map_mappping_memory(u64 addr, u64 size)
	{
		if (!addr || !size)
		{
			return false;
		}

		ensure(s_is_mapping.for_each([addr, size](map_info_t& info)
		{
			if (!info.addr && info.state.compare_and_swap_test(0, 2))
			{
				info.addr = addr;
				info.size = size;
				info.state = 1;
				return true;
			}

			return false;
		}, false).second);

		return true;
	}

	bool is_memory_mappping_memory(const void* addr)
	{
		return is_memory_mappping_memory(reinterpret_cast<u64>(addr));
	}
#endif

	long get_page_size()
	{
		static const long r = []() -> long
		{
#ifdef _WIN32
			SYSTEM_INFO info;
			::GetSystemInfo(&info);
			return info.dwPageSize;
#else
			return ::sysconf(_SC_PAGESIZE);
#endif
		}();

		return ensure(r, FN(((x & (x - 1)) == 0 && x > 0 && x <= 0x10000)));
	}

	// Convert memory protection (internal)
	static auto operator +(protection prot)
	{
#ifdef _WIN32
		DWORD _prot = PAGE_NOACCESS;
		switch (prot)
		{
		case protection::rw: _prot = PAGE_READWRITE; break;
		case protection::ro: _prot = PAGE_READONLY; break;
		case protection::no: break;
		case protection::wx: _prot = PAGE_EXECUTE_READWRITE; break;
		case protection::rx: _prot = PAGE_EXECUTE_READ; break;
		}
#else
		int _prot = PROT_NONE;
		switch (prot)
		{
		case protection::rw: _prot = PROT_READ | PROT_WRITE; break;
		case protection::ro: _prot = PROT_READ; break;
		case protection::no: break;
		case protection::wx: _prot = PROT_READ | PROT_WRITE | PROT_EXEC; break;
		case protection::rx: _prot = PROT_READ | PROT_EXEC; break;
		}
#endif

		return _prot;
	}

	void* memory_reserve(usz size, void* use_addr, [[maybe_unused]] bool is_memory_mapping, [[maybe_unused]] bool can_be_jit)
	{
#ifdef _WIN32
		if (is_memory_mapping && has_win10_memory_mapping_api())
		{
			if (auto ptr = VirtualAlloc2(nullptr, use_addr, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0))
			{
				map_mappping_memory(reinterpret_cast<u64>(ptr), size);
				return ptr;
			}

			return nullptr;
		}

		return ::VirtualAlloc(use_addr, size, MEM_RESERVE, PAGE_NOACCESS);
#else
		if (use_addr && reinterpret_cast<uptr>(use_addr) % 0x10000)
		{
			return nullptr;
		}

		const auto orig_size = size;

		if (!use_addr)
		{
			// Hack: Ensure aligned 64k allocations
			size += 0x10000;
		}

		// Memory mapping regions will be replaced by file-backed MAP_FIXED mappings
		// (via shm::map), which is incompatible with MAP_JIT. Only use MAP_JIT for
		// non-mapping regions that need JIT executable support.
#ifdef __APPLE__
		const int jit_flag = is_memory_mapping || !can_be_jit ? 0 : MAP_JIT;
#ifdef ARCH_ARM64
		auto ptr = ::mmap(use_addr, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | jit_flag | c_map_noreserve, -1, 0);
#else
		auto ptr = ::mmap(use_addr, size, PROT_NONE, MAP_ANON | MAP_PRIVATE | jit_flag | c_map_noreserve, -1, 0);
#endif
#else
		auto ptr = ::mmap(use_addr, size, PROT_NONE, MAP_ANON | MAP_PRIVATE | c_map_noreserve, -1, 0);
#endif

		if (ptr == reinterpret_cast<void*>(uptr{umax}))
		{
			return nullptr;
		}

		if (use_addr && ptr != use_addr)
		{
			::munmap(ptr, size);
			return nullptr;
		}

		if (!use_addr && ptr)
		{
			// Continuation of the hack above
			const auto misalign = reinterpret_cast<uptr>(ptr) % 0x10000;
			::munmap(ptr, 0x10000 - misalign);

			if (misalign)
			{
				::munmap(static_cast<u8*>(ptr) + size - misalign, misalign);
			}

			ptr = static_cast<u8*>(ptr) + (0x10000 - misalign);
		}

		if constexpr (c_madv_hugepage != 0)
		{
			if (orig_size % 0x200000 == 0)
			{
				::madvise(ptr, orig_size, c_madv_hugepage);
			}
		}

		if constexpr (c_madv_no_dump != 0)
		{
			ensure(::madvise(ptr, orig_size, c_madv_no_dump) != -1);
		}
		else
		{
			ensure(::madvise(ptr, orig_size, c_madv_free) != -1);
		}

		return ptr;
#endif
	}

	void memory_commit(void* pointer, usz size, protection prot)
	{
		if (!size)
		{
			return;
		}

#ifdef _WIN32
		ensure(::VirtualAlloc(pointer, size, MEM_COMMIT, +prot));
#else
		const u64 ptr64 = reinterpret_cast<u64>(pointer);
		ensure(::mprotect(reinterpret_cast<void*>(ptr64 & -get_page_size()), size + (ptr64 & (get_page_size() - 1)), +prot) != -1);

		if constexpr (c_madv_dump != 0)
		{
			ensure(::madvise(reinterpret_cast<void*>(ptr64 & -get_page_size()), size + (ptr64 & (get_page_size() - 1)), c_madv_dump) != -1);
		}
		else
		{
			ensure(::madvise(reinterpret_cast<void*>(ptr64 & -get_page_size()), size + (ptr64 & (get_page_size() - 1)), MADV_WILLNEED) != -1);
		}
#endif
	}

	void memory_decommit(void* pointer, usz size, [[maybe_unused]] bool can_be_jit)
	{
		if (!size)
		{
			return;
		}

#ifdef _WIN32
		ensure(::VirtualFree(pointer, size, MEM_DECOMMIT));
#else
		const u64 ptr64 = reinterpret_cast<u64>(pointer);
#if defined(__APPLE__) && defined(ARCH_ARM64)
		// Hack: on macOS, Apple explicitly fails mmap if you combine MAP_FIXED and MAP_JIT.
		// So we unmap the space and just hope it maps to the same address we got before instead.
		// The Xcode manpage says the pointer is a hint and the OS will try to map at the hint location
		// so this isn't completely undefined behavior.
		ensure(::munmap(pointer, size) != -1);
		ensure(::mmap(pointer, size, PROT_NONE,  MAP_ANON | MAP_PRIVATE | (can_be_jit ? MAP_JIT : 0), -1, 0) == pointer);
#else
		ensure(::mmap(pointer, size, PROT_NONE, MAP_FIXED | MAP_ANON | MAP_PRIVATE | c_map_noreserve, -1, 0) != reinterpret_cast<void*>(uptr{umax}));
#endif

		if constexpr (c_madv_no_dump != 0)
		{
			ensure(::madvise(reinterpret_cast<void*>(ptr64 & -get_page_size()), size + (ptr64 & (get_page_size() - 1)), c_madv_no_dump) != -1);
		}
		else
		{
			ensure(::madvise(reinterpret_cast<void*>(ptr64 & -get_page_size()), size + (ptr64 & (get_page_size() - 1)), c_madv_free) != -1);
		}
#endif
	}

	void memory_reset(void* pointer, usz size, protection prot, [[maybe_unused]] bool can_be_jit)
	{
		if (!size)
		{
			return;
		}

#ifdef _WIN32
		memory_decommit(pointer, size);
		memory_commit(pointer, size, prot);
#else
		const u64 ptr64 = reinterpret_cast<u64>(pointer);
#if defined(__APPLE__) && defined(ARCH_ARM64)
		ensure(::munmap(pointer, size) != -1);
		ensure(::mmap(pointer, size, +prot,  MAP_ANON | MAP_PRIVATE | (can_be_jit ? MAP_JIT : 0), -1, 0) == pointer);
#else
		ensure(::mmap(pointer, size, +prot, MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0) != reinterpret_cast<void*>(uptr{umax}));
#endif

		if constexpr (c_madv_hugepage != 0)
		{
			if (size % 0x200000 == 0)
			{
				::madvise(reinterpret_cast<void*>(ptr64 & -get_page_size()), size + (ptr64 & (get_page_size() - 1)), c_madv_hugepage);
			}
		}

		if constexpr (c_madv_dump != 0)
		{
			ensure(::madvise(reinterpret_cast<void*>(ptr64 & -get_page_size()), size + (ptr64 & (get_page_size() - 1)), c_madv_dump) != -1);
		}
		else
		{
			ensure(::madvise(reinterpret_cast<void*>(ptr64 & -get_page_size()), size + (ptr64 & (get_page_size() - 1)), MADV_WILLNEED) != -1);
		}
#endif
	}

	void memory_release(void* pointer, usz size)
	{
		if (!size)
		{
			return;
		}

#ifdef _WIN32
		unmap_mappping_memory(reinterpret_cast<u64>(pointer), size);
		ensure(::VirtualFree(pointer, 0, MEM_RELEASE));
#else
		ensure(::munmap(pointer, size) != -1);
#endif
	}

#if defined(_WIN32) && defined(RPCS3_UWP)
	static bool memory_protect_lazy(void* pointer, usz size, protection prot, bool keep_reserved);
#endif

	void memory_protect(void* pointer, usz size, protection prot)
	{
		if (!size)
		{
			return;
		}

#ifdef _WIN32

		DWORD old;
		if (::VirtualProtect(pointer, size, +prot, &old))
		{
			return;
		}

#ifdef RPCS3_UWP
		// Pages of lazily committed memory can't be protected before they are committed.
		if (memory_protect_lazy(pointer, size, prot, false))
		{
			return;
		}
#endif

		for (u64 addr = reinterpret_cast<u64>(pointer), end = addr + size; addr < end;)
		{
			const u64 boundary = (addr + 0x10000) & -0x10000;
			const u64 block_size = std::min(boundary, end) - addr;

			if (!::VirtualProtect(reinterpret_cast<LPVOID>(addr), block_size, +prot, &old))
			{
				fmt::throw_exception("VirtualProtect failed (%p, 0x%x, addr=0x%x, error=%s)", pointer, size, addr, fmt::win_error{GetLastError(), nullptr});
			}

			// Next region
			addr += block_size;
		}
#else
		const u64 ptr64 = reinterpret_cast<u64>(pointer);
		ensure(::mprotect(reinterpret_cast<void*>(ptr64 & -get_page_size()), size + (ptr64 & (get_page_size() - 1)), +prot) != -1);
#endif
	}

	bool memory_lock(void* pointer, usz size)
	{
		if (!size)
		{
			return true;
		}

#ifdef _WIN32
		return ::VirtualLock(pointer, size);
#else
		return !::mlock(pointer, size);
#endif
	}

	void* memory_map_fd([[maybe_unused]] native_handle fd, [[maybe_unused]] usz size, [[maybe_unused]] protection prot)
	{
#ifdef _WIN32
		// TODO
		return nullptr;
#else
		const auto result = ::mmap(nullptr, size, +prot, MAP_SHARED, fd, 0);

		if (result == reinterpret_cast<void*>(uptr{umax}))
		{
			[[unlikely]] return nullptr;
		}

		return result;
#endif
	}

#if defined(_WIN32) && defined(RPCS3_UWP)
	// Number of live lazily committed shm objects (memory_commit_lazy has nothing to do without them).
	static atomic_t<u32> s_lazy_shm_count{0};

	shm::shm(u64 size, lazy_commit_t, u32 flags)
		: m_flags(flags)
		, m_size(utils::align(size, 0x10000))
	{
		const ULARGE_INTEGER max_size{ .QuadPart = m_size };
		m_handle = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE, max_size.HighPart, max_size.LowPart, nullptr);
		if (!m_handle)
		{
			uwp_mem_log.error("CreateFileMappingFromApp(0x%x bytes, SEC_RESERVE) failed (%u), committing in full", m_size, ::GetLastError());
			m_handle = ensure(::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, max_size.HighPart, max_size.LowPart, nullptr));
			return;
		}

		m_lazy = true;
		s_lazy_shm_count++;
		if (m_size >= 0x100'0000)
		{
			uwp_mem_log.notice("Lazily committed shm: %u MB (flags 0x%x)", m_size >> 20, flags);
		}
	}

	bool memory_commit_lazy(const void* pointer, usz size)
	{
		if (!s_lazy_shm_count || !size)
		{
			return false;
		}

		bool found = false;

		for (u64 addr = reinterpret_cast<u64>(pointer) & -0x1000, end = reinterpret_cast<u64>(pointer) + size; addr < end;)
		{
			::MEMORY_BASIC_INFORMATION mem{};
			if (!::VirtualQuery(reinterpret_cast<void*>(addr), &mem, sizeof(mem)))
			{
				break;
			}

			const u64 region_end = reinterpret_cast<u64>(mem.BaseAddress) + mem.RegionSize;

			// Uncommitted pages of a mapped view only exist in SEC_RESERVE sections, which only
			// shm::lazy_commit creates.
			if (mem.State == MEM_RESERVE && mem.Type == MEM_MAPPED)
			{
				found = true;

				// Commit whole 64 KB granules around the range (fewer faults), within this view.
				const u64 lo = std::max<u64>(addr & -0x10000, reinterpret_cast<u64>(mem.AllocationBase));
				const u64 hi = std::min<u64>(utils::align<u64>(std::min(end, region_end), 0x10000), region_end);

				if (!::VirtualAllocFromApp(reinterpret_cast<void*>(lo), hi - lo, MEM_COMMIT, PAGE_READWRITE) &&
					!::VirtualAllocFromApp(reinterpret_cast<void*>(addr), std::min(end, region_end) - addr, MEM_COMMIT, PAGE_READWRITE))
				{
					uwp_mem_log.error("Committing lazy shm pages at 0x%x (0x%x bytes) failed (%u)", addr, std::min(end, region_end) - addr, ::GetLastError());
					return false;
				}

				// A commit that leaves the page uncommitted would make the faulting access retry forever.
				::MEMORY_BASIC_INFORMATION after{};
				if (!::VirtualQuery(reinterpret_cast<void*>(addr), &after, sizeof(after)) || after.State != MEM_COMMIT)
				{
					uwp_mem_log.error("Lazy shm page at 0x%x still not committed (state=0x%x, protect=0x%x)", addr, after.State, after.Protect);
					return false;
				}

				// Progress log every 16 MB committed.
				static atomic_t<u64> s_committed{0};
				const u64 before = s_committed.fetch_add(hi - lo);
				if ((before >> 24) != ((before + hi - lo) >> 24))
					uwp_mem_log.notice("Lazy shm: 0x%x bytes committed so far", before + hi - lo);
			}

			addr = region_end;
		}

		return found;
	}

	// memory_protect for a range with lazily committed pages: pages nobody touched yet stay
	// reserved when they become read-write (the fault handler commits them read-write), and
	// when they become inaccessible because they are freed (keep_reserved); other protections
	// commit them first (an inaccessible page must fault into RSX's handlers, not get committed).
	// False if the range has no such pages.
	static bool memory_protect_lazy(void* pointer, usz size, protection prot, bool keep_reserved)
	{
		if (!s_lazy_shm_count)
		{
			return false;
		}

		bool found = false;
		DWORD old;

		for (u64 addr = reinterpret_cast<u64>(pointer), end = addr + size; addr < end;)
		{
			::MEMORY_BASIC_INFORMATION mem{};
			if (!::VirtualQuery(reinterpret_cast<void*>(addr), &mem, sizeof(mem)))
			{
				return false;
			}

			const u64 part_end = std::min<u64>(end, reinterpret_cast<u64>(mem.BaseAddress) + mem.RegionSize);
			const auto part = reinterpret_cast<void*>(addr);

			if (mem.State == MEM_RESERVE && mem.Type == MEM_MAPPED)
			{
				found = true;

				if (prot != protection::rw && !(prot == protection::no && keep_reserved) &&
					(!::VirtualAllocFromApp(part, part_end - addr, MEM_COMMIT, PAGE_READWRITE) || !::VirtualProtect(part, part_end - addr, +prot, &old)))
				{
					return false;
				}
			}
			else if (!::VirtualProtect(part, part_end - addr, +prot, &old))
			{
				return false;
			}

			addr = part_end;
		}

		return found;
	}

	void memory_protect_freed(void* pointer, usz size)
	{
		if (!memory_protect_lazy(pointer, size, protection::no, true))
		{
			memory_protect(pointer, size, protection::no);
		}
	}

	void memory_zero(void* pointer, usz size)
	{
		if (!s_lazy_shm_count)
		{
			std::memset(pointer, 0, size);
			return;
		}

		for (u64 addr = reinterpret_cast<u64>(pointer), end = addr + size; addr < end;)
		{
			::MEMORY_BASIC_INFORMATION mem{};
			if (!::VirtualQuery(reinterpret_cast<void*>(addr), &mem, sizeof(mem)))
			{
				std::memset(reinterpret_cast<void*>(addr), 0, end - addr);
				return;
			}

			const u64 part_end = std::min<u64>(end, reinterpret_cast<u64>(mem.BaseAddress) + mem.RegionSize);

			if (mem.State == MEM_COMMIT)
			{
				std::memset(reinterpret_cast<void*>(addr), 0, part_end - addr);
			}

			addr = part_end;
		}
	}
#else
	shm::shm(u64 size, lazy_commit_t, u32 flags)
		: shm(size, flags)
	{
	}

	bool memory_commit_lazy(const void*, usz)
	{
		return false;
	}

	void memory_protect_freed(void* pointer, usz size)
	{
		memory_protect(pointer, size, protection::no);
	}

	void memory_zero(void* pointer, usz size)
	{
		std::memset(pointer, 0, size);
	}
#endif

	shm::shm(u64 size, u32 flags)
		: m_flags(flags)
		, m_size(utils::align(size, 0x10000))
	{
#ifdef _WIN32
		const ULARGE_INTEGER max_size{ .QuadPart = m_size };
		m_handle = ensure(::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, max_size.HighPart, max_size.LowPart, nullptr));
#ifdef RPCS3_UWP
		if (m_size >= 0x100'0000)
		{
			uwp_mem_log.warning("Fully committed shm: %u MB (flags 0x%x)", m_size >> 20, flags);
		}
#endif
#elif defined(__linux__) || defined(__FreeBSD__)
		m_file = -1;

		// Try to use 2MB pages for 2M-aligned shm
		if constexpr (c_mfd_huge_2mb != 0)
		{
			if (m_size % 0x200000 == 0 && flags & 2)
			{
				m_file = ::memfd_create_("2M", c_mfd_huge_2mb);
			}
		}

		if (m_file == -1)
		{
			m_file = ::memfd_create_("", 0);
		}

		ensure(m_file >= 0);
		ensure(::ftruncate(m_file, m_size) >= 0);
#else
		const std::string name = "/rpcs3-mem-" + std::to_string(reinterpret_cast<u64>(this));

		while ((m_file = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, S_IWUSR | S_IRUSR)) == -1)
		{
			if (errno == EMFILE)
			{
				fmt::throw_exception("Too many open files. Raise the limit and try again.");
			}

			ensure(errno == EEXIST);
		}

		ensure(::shm_unlink(name.c_str()) >= 0);
		ensure(::ftruncate(m_file, m_size) >= 0);
#endif
	}

	shm::shm(u64 size, const std::string& storage)
		: m_size(utils::align(size, 0x10000))
	{
#if defined(_WIN32) && defined(RPCS3_UWP)
		// Sparse temp files can't be mapped from a UWP app (ERROR_INVALID_PARAMETER), so use
		// page-file backed memory. It is committed up front, which is fine for these blocks.
		static_cast<void>(storage);
		const ULARGE_INTEGER max_size{ .QuadPart = m_size };
		m_handle = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, max_size.HighPart, max_size.LowPart, nullptr);
		if (!m_handle)
		{
			uwp_mem_log.fatal("CreateFileMappingFromApp(0x%x bytes) failed (%u)", m_size, ::GetLastError());
		}
		else if (m_size >= 0x100'0000)
		{
			uwp_mem_log.warning("Fully committed shm: %u MB (%s)", m_size >> 20, storage);
		}
#elif defined(_WIN32)
		fs::file f;

		auto open_with_cleanup = [](fs::file& f, const std::string& path)
		{
			f.close();

			for (u32 i = 1; i <= 3; i++)
			{
				// Cleanup
				fs::remove_file(fmt::format("%s.%d.tmp", path, i));
			}

			for (u32 try_count = 3, i = 0; !f && i < try_count; i++, ::Sleep(100))
			{
				// Bug workaround: removing old file may be safer than rewriting it
				if (!fs::remove_file(path) && fs::g_tls_error != fs::error::noent)
				{
					// Try MoveFile
					fs::rename(path, fmt::format("%s.%d.tmp", path, i + 1), true);
				}

				if (f.open(path, fs::read + fs::write + fs::create + fs::excl))
				{
					return true;
				}

				if (fs::g_tls_error != fs::error::exist && fs::g_tls_error != fs::error::acces)
				{
					return false;
				}
			}

			return false;
		};

		std::string storage1 = fs::get_temp_dir();
		std::string storage2 = fs::get_cache_dir();

		if (storage.empty())
		{
			storage1 += "rpcs3_vm_sparse.tmp";
			storage2 += "rpcs3_vm_sparse.tmp";
		}
		else
		{
			storage1 += storage;
			storage2 += storage;
		}

		std::function<bool(const std::string&, HANDLE, usz)> set_sparse_and_map = [&](const std::string& storagex, HANDLE h, usz m_size) -> bool
		{
			// In case failed, revert changes and forward failure
			auto clean = [&](bool result)
			{
				if (!result)
				{
					const fs::error last_fs_error = fs::g_tls_error;
					const DWORD last_win_error = ::GetLastError();

					fs::remove_file(storagex);

					fs::g_tls_error = last_fs_error;
					::SetLastError(last_win_error);
				}

				return result;
			};

			FILE_SET_SPARSE_BUFFER arg{.SetSparse = true};
			FILE_BASIC_INFO info0{};
			ensure(clean(GetFileInformationByHandleEx(h, FileBasicInfo, &info0, sizeof(info0))));

			if ((info0.FileAttributes & FILE_ATTRIBUTE_ARCHIVE) || (~info0.FileAttributes & FILE_ATTRIBUTE_TEMPORARY))
			{
				info0.FileAttributes &= ~FILE_ATTRIBUTE_ARCHIVE;
				info0.FileAttributes |= FILE_ATTRIBUTE_TEMPORARY;
				ensure(clean(SetFileInformationByHandle(h, FileBasicInfo, &info0, sizeof(info0))));
			}

			if (DWORD bytesReturned{}; (info0.FileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) || DeviceIoControl(h, FSCTL_SET_SPARSE, &arg, sizeof(arg), nullptr, 0, &bytesReturned, nullptr))
			{
				if ((info0.FileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) == 0 && !storagex.empty())
				{
					// Retry once (bug workaround)
					if (!open_with_cleanup(f, storagex))
					{
						return false;
					}

					return set_sparse_and_map("", f.get_handle(), m_size);
				}

				FILE_STANDARD_INFO info;
				FILE_END_OF_FILE_INFO _eof{};
				ensure(clean(GetFileInformationByHandleEx(h, FileStandardInfo, &info, sizeof(info))));
				ensure(clean(GetFileSizeEx(h, &_eof.EndOfFile)));

				if (info.AllocationSize.QuadPart && _eof.EndOfFile.QuadPart == static_cast<LONGLONG>(m_size))
				{
					// Truncate file since it may be dirty (fool-proof)
					DWORD ret = 0;
					FILE_ALLOCATED_RANGE_BUFFER dummy{};
					dummy.Length.QuadPart = m_size;

					if (!DeviceIoControl(h, FSCTL_QUERY_ALLOCATED_RANGES, &dummy, sizeof(dummy), nullptr, 0, &ret, 0) || ret)
					{
						_eof.EndOfFile.QuadPart = 0;
					}
				}

				if (_eof.EndOfFile.QuadPart != static_cast<LONGLONG>(m_size))
				{
					// Reset file size to 0 if it doesn't match
					_eof.EndOfFile.QuadPart = 0;
					ensure(clean(SetFileInformationByHandle(h, FileEndOfFileInfo, &_eof, sizeof(_eof))));
				}

				// It seems impossible to automatically delete file on exit when file mapping is used
				if (f.size() != m_size)
				{
					// Resize the file gradually (bug workaround)
					for (usz i = 0; i < m_size / (1024 * 1024 * 256); i++)
					{
						ensure(clean(f.trunc((i + 1) * (1024 * 1024 * 256))));
					}

					ensure(clean(f.trunc(m_size)));
				}

				m_handle = ::CreateFileMappingW(f.get_handle(), nullptr, PAGE_READWRITE, 0, 0, nullptr);
				if (clean(!m_handle))
				{
					ensure(storagex == storage1);
					return false;
				}

				return true;
			}

			return false;
		};

		// Attempt to remove from secondary storage in case this succeeds
		fs::remove_file(storage2);

		if (!open_with_cleanup(f, storage1) || !set_sparse_and_map(storage1, f.get_handle(), m_size))
		{
			// Attempt to remove from main storage in case this succeeds
			f.close();
			fs::remove_file(storage1);

			// Fallback storage
			ensure(open_with_cleanup(f, storage2));

			if (!set_sparse_and_map(storage2, f.get_handle(), m_size))
			{
				MessageBoxW(0, L"Failed to initialize sparse file.\nCan't find a filesystem with sparse file support (NTFS).", L"RPCS3", MB_ICONERROR);
			}

			m_storage = std::move(storage2);
		}
		else
		{
			m_storage = std::move(storage1);
		}
#else

#ifdef __linux__
#ifdef ANDROID
		if constexpr (constexpr char c = '?')
#else
		if (const char c = fs::file("/proc/sys/vm/overcommit_memory").read<char>(); c == '0' || c == '1')
#endif
		{
			// Simply use memfd for overcommit memory
			m_file = ensure(::memfd_create_("", 0), FN(x >= 0));
			ensure(::ftruncate(m_file, m_size) >= 0);
			return;
		}
		else
		{
			fprintf(stderr, "Reading /proc/sys/vm/overcommit_memory: %c", c);
		}
#else
		int vm_overcommit = 0;

#if defined(__NetBSD__) || defined(__APPLE__)
		// Always ON
		vm_overcommit = 0;
#elif defined(__FreeBSD__)
		auto vm_sz = sizeof(int);
		int mib[2]{CTL_VM, VM_OVERCOMMIT};
		if (::sysctl(mib, 2, &vm_overcommit, &vm_sz, NULL, 0) != 0)
			vm_overcommit = -1;
#else
		vm_overcommit = -1;
#endif

		if ((vm_overcommit & 3) == 0)
		{
#if defined(__FreeBSD__)
			m_file = ensure(::memfd_create_("", 0), FN(x >= 0));
#else
			const std::string name = "/rpcs3-mem2-" + std::to_string(reinterpret_cast<u64>(this));

			while ((m_file = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, S_IWUSR | S_IRUSR)) == -1)
			{
				if (errno == EMFILE)
				{
					fmt::throw_exception("Too many open files. Raise the limit and try again.");
				}

				ensure(errno == EEXIST);
			}

			ensure(::shm_unlink(name.c_str()) >= 0);
#endif
			ensure(::ftruncate(m_file, m_size) >= 0);
			return;
		}
#endif

		if (!storage.empty())
		{
			m_file = ::open(storage.c_str(), O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);
			::unlink(storage.c_str());
		}
		else
		{
			std::string storage = fs::get_cache_dir() + "rpcs3_vm_sparse.tmp";
			m_file = ::open(storage.c_str(), O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);
			::unlink(storage.c_str());
		}

		ensure(m_file >= 0);
		struct ::stat stats;
		ensure(::fstat(m_file, &stats) >= 0);

		if (!(stats.st_size ^ m_size) && !stats.st_blocks)
		{
			// Already initialized
			return;
		}

		// Truncate file since it may be dirty (fool-proof)
		ensure(::ftruncate(m_file, 0) >= 0);
		ensure(::ftruncate(m_file, 0x100000) >= 0);
		stats.st_size = 0x100000;

#ifdef SEEK_DATA
		errno = EINVAL;
		if (stats.st_blocks * 512 >= 0x100000 && ::lseek(m_file, 0, SEEK_DATA) ^ stats.st_size && errno != ENXIO)
		{
			fmt::throw_exception("Failed to initialize sparse file in '%s'\n"
				"It seems this filesystem doesn't support sparse files (%d).\n",
				storage.empty() ? fs::get_cache_dir().c_str() : storage.c_str(), +errno);
		}
#endif

		if (stats.st_size ^ m_size)
		{
			// Fix file size
			ensure(::ftruncate(m_file, m_size) >= 0);
		}
#endif
	}

	shm::~shm()
	{
		this->unmap_self();

#ifdef _WIN32
		::CloseHandle(m_handle);
#if defined(RPCS3_UWP)
		if (m_lazy)
			s_lazy_shm_count--;
#endif
#else
		::close(m_file);
#endif

		if (!m_storage.empty())
			fs::remove_file(m_storage);
	}

	u8* shm::map(void* ptr, protection prot, bool cow) const
	{
#ifdef _WIN32
		DWORD access = FILE_MAP_WRITE;
		switch (prot)
		{
		case protection::rw:
		case protection::ro:
		case protection::no:
			break;
		case protection::wx:
		case protection::rx:
			access |= FILE_MAP_EXECUTE;
			break;
		}

		if (cow)
		{
			access |= FILE_MAP_COPY;
		}

		if (auto ret = static_cast<u8*>(::MapViewOfFileEx(m_handle, access, 0, 0, m_size, ptr)))
		{
			if (prot != protection::rw && prot != protection::wx)
			{
				DWORD old;
				if (!::VirtualProtect(ret, m_size, +prot, &old))
				{
					::UnmapViewOfFile(ret);
					return nullptr;
				}
			}

			return ret;
		}

		return nullptr;
#else
		const u64 ptr64 = reinterpret_cast<u64>(ptr) & -0x10000;

		if (ptr64)
		{
			const auto result = ::mmap(reinterpret_cast<void*>(ptr64), m_size, +prot, (cow ? MAP_PRIVATE : MAP_SHARED) | MAP_FIXED, m_file, 0);

			return reinterpret_cast<u8*>(result);
		}
		else
		{
			const u64 res64 = reinterpret_cast<u64>(::mmap(reinterpret_cast<void*>(ptr64), m_size + 0xf000, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0));

			const u64 aligned = utils::align(res64, 0x10000);
			const auto result = ::mmap(reinterpret_cast<void*>(aligned), m_size, +prot, (cow ? MAP_PRIVATE : MAP_SHARED) | MAP_FIXED, m_file, 0);

			// Now cleanup remnants
			if (aligned > res64)
			{
				ensure(::munmap(reinterpret_cast<void*>(res64), aligned - res64) == 0);
			}

			if (aligned < res64 + 0xf000)
			{
				ensure(::munmap(reinterpret_cast<void*>(aligned + m_size), (res64 + 0xf000) - (aligned)) == 0);
			}

			return reinterpret_cast<u8*>(result);
		}
#endif
	}

	u8* shm::try_map(void* ptr, protection prot, bool cow) const
	{
		// Non-null pointer shall be specified
		const auto target = ensure(reinterpret_cast<u8*>(reinterpret_cast<u64>(ptr) & -0x10000));

#ifdef _WIN32
		return this->map(target, prot, cow);
#else
		const auto result = reinterpret_cast<u8*>(::mmap(reinterpret_cast<void*>(target), m_size, +prot, (cow ? MAP_PRIVATE : MAP_SHARED), m_file, 0));

		if (result == reinterpret_cast<void*>(uptr{umax}))
		{
			[[unlikely]] return nullptr;
		}

		return result;
#endif
	}

	std::pair<u8*, std::string> shm::map_critical(void* ptr, protection prot, bool cow)
	{
		const auto target = reinterpret_cast<u8*>(reinterpret_cast<u64>(ptr) & -0x10000);

#ifdef _WIN32
		::MEMORY_BASIC_INFORMATION mem{};
		if (!::VirtualQuery(target, &mem, sizeof(mem)))
		{
			return {nullptr, fmt::format("VirtualQuery() Failed with %s", fmt::win_error{GetLastError(), nullptr})};
		}

		if (mem.State != MEM_RESERVE)
		{
			return {nullptr, fmt::format("VirtualQuery() reported unexpected memory info: state=0x%x, %s", mem.State, std::as_bytes(std::span(&mem, 1)))};
		}

		const auto base = static_cast<u8*>(mem.AllocationBase);
		const auto size = mem.RegionSize + (target - base);

		if (is_memory_mappping_memory(ptr))
		{
			if (base < target && !::VirtualFree(base, target - base, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
			{
				return {nullptr, "Failed to split allocation base"};
			}

			if (target + m_size < base + size && !::VirtualFree(target, m_size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
			{
				return {nullptr, "Failed to split allocation end"};
			}

			DWORD access = 0;

			switch (prot)
			{
			case protection::rw:
			case protection::ro:
			case protection::no:
				access = cow ? PAGE_WRITECOPY : PAGE_READWRITE;
				break;
			case protection::wx:
			case protection::rx:
				access = cow ? PAGE_EXECUTE_WRITECOPY : PAGE_EXECUTE_READWRITE;
				break;
			}

			if (MapViewOfFile3(m_handle, GetCurrentProcess(), target, 0, m_size, MEM_REPLACE_PLACEHOLDER, access, nullptr, 0))
			{
				if (prot != protection::rw && prot != protection::wx)
				{
					DWORD old;
#ifdef RPCS3_UWP
					// Lazily committed views have no committed pages to protect yet (and the fault handler
					// only commits pages of allocated readable guest memory).
					if (m_lazy && memory_protect_lazy(target, m_size, prot, true))
					{
					}
					else
#endif
					if (!::VirtualProtect(target, m_size, +prot, &old))
					{
						UnmapViewOfFile2(GetCurrentProcess(), target, MEM_PRESERVE_PLACEHOLDER);
						return {nullptr, "Failed to protect"};
					}
				}

				return {target, {}};
			}

			return {nullptr, "Failed to map3"};
		}

		if (!::VirtualFree(mem.AllocationBase, 0, MEM_RELEASE))
		{
			return {nullptr, "VirtualFree() failed on allocation base"};
		}

		if (base < target && !::VirtualAlloc(base, target - base, MEM_RESERVE, PAGE_NOACCESS))
		{
			return {nullptr, "VirtualAlloc() failed to reserve allocation base"};
		}

		if (target + m_size < base + size && !::VirtualAlloc(target + m_size, base + size - target - m_size, MEM_RESERVE, PAGE_NOACCESS))
		{
			return {nullptr, "VirtualAlloc() failed to reserve allocation end"};
		}
#endif
		const auto mapped_addr = this->map(target, prot, cow);

		if (!mapped_addr)
		{
			return {nullptr, "Failed to map"};
		}

		return {mapped_addr, {}};
	}

	u8* shm::map_self(protection prot)
	{
		void* ptr = m_ptr;

		for (void* mapped = nullptr; !ptr;)
		{
			if (!mapped)
			{
				mapped = this->map(nullptr, prot);

				if (!mapped)
				{
					if ((ptr = m_ptr))
					{
						break;
					}

					return nullptr;
				}
			}

			// Install mapped memory
			if (m_ptr.compare_exchange(ptr, mapped))
			{
				ptr = mapped;
			}
			else if (ptr)
			{
				// Mapped already, nothing to do.
				ensure(ptr != mapped);
				this->unmap(mapped);
			}
		}

		return static_cast<u8*>(ptr);
	}

	void shm::unmap(void* ptr) const
	{
#ifdef _WIN32
		::UnmapViewOfFile(ptr);
#else
		::munmap(ptr, m_size);
#endif
	}

	void shm::unmap_critical(void* ptr)
	{
		const auto target = reinterpret_cast<u8*>(reinterpret_cast<u64>(ptr) & -0x10000);

#ifdef _WIN32
		// Free (reserved or placeholder) neighbours only. The uncommitted pages of a lazily committed
		// view next to this one are MEM_RESERVE too, but they belong to that view (MEM_MAPPED).
		const auto is_free = [](const ::MEMORY_BASIC_INFORMATION& m) { return m.State == MEM_RESERVE && m.Type != MEM_MAPPED; };

		if (is_memory_mappping_memory(ptr))
		{
			ensure(UnmapViewOfFile2(GetCurrentProcess(), target, MEM_PRESERVE_PLACEHOLDER));

			::MEMORY_BASIC_INFORMATION mem{}, mem2{};
			ensure(::VirtualQuery(target - 1, &mem, sizeof(mem)) && ::VirtualQuery(target + m_size, &mem2, sizeof(mem2)));

			const auto size1 = is_free(mem) ? target - static_cast<u8*>(mem.AllocationBase) : 0;
			const auto size2 =  is_free(mem2) ? mem2.RegionSize : 0;

			if (!size1 && !size2)
			{
				return;
			}

			ensure(::VirtualFree(size1 ? mem.AllocationBase : target, m_size + size1 + size2, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS));
			return;
		}

		this->unmap(target);

		::MEMORY_BASIC_INFORMATION mem, mem2;
		if (!::VirtualQuery(target - 1, &mem, sizeof(mem)) || !::VirtualQuery(target + m_size, &mem2, sizeof(mem2)))
		{
			return;
		}

		if (is_free(mem) && !::VirtualFree(mem.AllocationBase, 0, MEM_RELEASE))
		{
			return;
		}

		if (is_free(mem2) && !::VirtualFree(mem2.AllocationBase, 0, MEM_RELEASE))
		{
			return;
		}

		const auto size1 = is_free(mem) ? target - static_cast<u8*>(mem.AllocationBase) : 0;
		const auto size2 = is_free(mem2) ? mem2.RegionSize : 0;

		if (!::VirtualAlloc(is_free(mem) ? mem.AllocationBase : target, m_size + size1 + size2, MEM_RESERVE, PAGE_NOACCESS))
		{
			return;
		}
#else
		// This method is faster but leaves mapped remnants of the shm (until overwritten)
		ensure(::mprotect(target, m_size, PROT_NONE) != -1);
#endif
	}

	void shm::unmap_self()
	{
		if (auto ptr = m_ptr.exchange(nullptr))
		{
			this->unmap(ptr);
		}
	}
} // namespace utils
