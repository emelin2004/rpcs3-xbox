// Heap guard for the Xbox test builds: the console has no page heap or debugger, and an
// intermittent heap corruption shows up ~1.5 s after launch. Every operator new block gets a
// header and a trailing canary; operator delete checks both and, if either was overwritten,
// writes the details and a stack (exe-relative offsets) to LocalState/heap_guard.log, then
// fails fast so a crash dump is taken at the culprit's free instead of somewhere later.
//
// That costs 80 bytes per block (header, alignment slack and canary), ~230 MB for the ~3 million
// live blocks of a Portal 2 session. By default blocks get a 16-byte header instead (size and
// caller for the memory logs, a tag that still catches double and mismatched deletes); the full
// guard is back with an empty LocalState/heap_guard file (uwp_heap_guard_open_log).

#include <windows.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <new>
#include <atomic>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{
	constexpr std::uint64_t c_magic_live = 0x4b4f4c4c414d4e57ull; // "WNMALLOK"
	constexpr std::uint64_t c_magic_dead = 0x44414544474e5557ull;
	constexpr unsigned char c_canary = 0xA5;
	constexpr std::size_t c_tail = 16;

	struct alignas(16) header
	{
		std::size_t size;
		void* caller;
		void* raw;       // what malloc returned
		std::uint32_t align; // 0 for plain new
		std::uint32_t pad;
		std::uint64_t magic;
		std::uint64_t magic_inv;
	};
	static_assert(sizeof(header) == 48);

	// Default header: malloc's 16-byte alignment is kept, the caller is stored exe-relative.
	struct alignas(16) compact_header
	{
		std::uint32_t size;
		std::uint32_t caller_rva;
		std::uint64_t tag;
	};
	static_assert(sizeof(compact_header) == 16);
	constexpr std::uint64_t c_tag_live = 0x4b4f50434d4e5557ull; // "WNMCPOK"
	constexpr std::uint64_t c_tag_dead = 0x44504344474e5557ull;

	// Both headers end with a u64 right before the user data: header::magic_inv or compact_header::tag.
	static_assert(offsetof(header, magic_inv) + 8 == sizeof(header));
	static_assert(offsetof(compact_header, tag) + 8 == sizeof(compact_header));

	std::atomic<bool> s_full_guard{false};

	HANDLE s_log = INVALID_HANDLE_VALUE;

	void write_log(const char* text)
	{
		if (s_log != INVALID_HANDLE_VALUE)
		{
			DWORD written;
			WriteFile(s_log, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
			FlushFileBuffers(s_log);
		}
		OutputDebugStringA(text);
	}

	std::uintptr_t rva(const void* p)
	{
		return reinterpret_cast<std::uintptr_t>(p) - reinterpret_cast<std::uintptr_t>(&__ImageBase);
	}

	[[noreturn]] void report_stack(char (&buf)[2048], int n)
	{
		n += std::snprintf(buf + n, sizeof(buf) - n, "\n  stack:");
		void* frames[32];
		const USHORT count = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
		for (USHORT i = 0; i < count && n < 2000; i++)
			n += std::snprintf(buf + n, sizeof(buf) - n, " %llx", static_cast<unsigned long long>(rva(frames[i])));
		std::snprintf(buf + n, sizeof(buf) - n, "\n");
		write_log(buf);
		__fastfail(50); // FAST_FAIL_HEAP_METADATA_CORRUPTION
	}

	[[noreturn]] void report(const char* what, header* h, void* user)
	{
		char buf[2048];
		int n = std::snprintf(buf, sizeof(buf), "heap guard: %s: block %p (user %p), size 0x%zx, allocated from exe+0x%llx, freed from exe+0x%llx\n",
			what, static_cast<void*>(h), user, h->size, static_cast<unsigned long long>(rva(h->caller)),
			static_cast<unsigned long long>(rva(__builtin_return_address(0))));

		// Bytes around the block: header and the start of the user data, then the tail.
		const auto* b = reinterpret_cast<const unsigned char*>(h);
		n += std::snprintf(buf + n, sizeof(buf) - n, "  head:");
		for (int i = 0; i < 64 && n < 1200; i++)
			n += std::snprintf(buf + n, sizeof(buf) - n, " %02x", b[i]);
		if (h->magic == c_magic_live && h->size < (1ull << 40))
		{
			const auto* t = static_cast<const unsigned char*>(user) + h->size;
			n += std::snprintf(buf + n, sizeof(buf) - n, "\n  tail:");
			for (int i = -16; i < 48 && n < 1900; i++)
				n += std::snprintf(buf + n, sizeof(buf) - n, " %02x", t[i]);
		}
		report_stack(buf, n);
	}

	[[noreturn]] void report_compact(const char* what, compact_header* h, void* user)
	{
		char buf[2048];
		int n = std::snprintf(buf, sizeof(buf), "heap guard: %s: block %p (user %p), size 0x%x, allocated from exe+0x%x, freed from exe+0x%llx\n  head:",
			what, static_cast<void*>(h), user, h->size, h->caller_rva, static_cast<unsigned long long>(rva(__builtin_return_address(0))));
		const auto* b = reinterpret_cast<const unsigned char*>(h);
		for (int i = 0; i < 48; i++)
			n += std::snprintf(buf + n, sizeof(buf) - n, " %02x", b[i]);
		report_stack(buf, n);
	}

	// Live operator new bytes per caller (open addressing, never removed), for memory logs.
	struct caller_slot
	{
		std::atomic<std::uintptr_t> caller{0};
		std::atomic<std::int64_t> bytes{0};
		std::atomic<std::int64_t> count{0};
	};
	constexpr std::size_t c_slots = 16384;
	caller_slot s_callers[c_slots];
	std::atomic<std::int64_t> s_live_bytes{0}, s_live_count{0};

	caller_slot* find_slot(void* caller_ptr)
	{
		const std::uintptr_t caller = reinterpret_cast<std::uintptr_t>(caller_ptr) | 1;
		std::size_t i = (caller * 0x9E3779B97F4A7C15ull) >> 50;
		for (std::size_t n = 0; n < c_slots; n++, i = (i + 1) % c_slots)
		{
			std::uintptr_t cur = s_callers[i].caller.load(std::memory_order_relaxed);
			if (cur == caller)
				return &s_callers[i];
			if (cur == 0 && s_callers[i].caller.compare_exchange_strong(cur, caller))
				return &s_callers[i];
			if (cur == caller)
				return &s_callers[i];
		}
		return nullptr;
	}

	void account(void* caller, std::int64_t size, std::int64_t count)
	{
		s_live_bytes.fetch_add(size, std::memory_order_relaxed);
		s_live_count.fetch_add(count, std::memory_order_relaxed);
		if (caller_slot* slot = find_slot(caller))
		{
			slot->bytes.fetch_add(size, std::memory_order_relaxed);
			slot->count.fetch_add(count, std::memory_order_relaxed);
		}
	}

	void* guarded_alloc(std::size_t size, std::size_t align, void* caller)
	{
		const std::uintptr_t caller_rva = rva(caller);
		if (align == 0 && !s_full_guard.load(std::memory_order_relaxed) && size <= 0xffff'ffffu && caller_rva <= 0xffff'ffffu)
		{
			auto* h = static_cast<compact_header*>(std::malloc(sizeof(compact_header) + size));
			if (!h)
				return nullptr;
			h->size = static_cast<std::uint32_t>(size);
			h->caller_rva = static_cast<std::uint32_t>(caller_rva);
			h->tag = c_tag_live;
			account(caller, static_cast<std::int64_t>(size), 1);
			return h + 1;
		}

		const std::size_t a = align > 16 ? align : 16;
		void* raw = std::malloc(sizeof(header) + a + size + c_tail);
		if (!raw)
			return nullptr;
		const std::uintptr_t user_addr = (reinterpret_cast<std::uintptr_t>(raw) + sizeof(header) + a - 1) & ~(a - 1);
		auto* user = reinterpret_cast<unsigned char*>(user_addr);
		auto* h = reinterpret_cast<header*>(user) - 1;
		h->size = size;
		h->caller = caller;
		h->raw = raw;
		h->align = static_cast<std::uint32_t>(align); // 0 only for plain new (aligned new may ask for <= 16)
		h->pad = 0;
		h->magic = c_magic_live;
		h->magic_inv = ~c_magic_live;
		std::memset(user + size, c_canary, c_tail);
		account(caller, static_cast<std::int64_t>(size), 1);
		return user;
	}

	void guarded_free(void* p, bool aligned)
	{
		if (!p)
			return;
		const std::uint64_t tag = static_cast<const std::uint64_t*>(p)[-1];
		if (tag == c_tag_live || tag == c_tag_dead)
		{
			auto* c = static_cast<compact_header*>(p) - 1;
			if (tag == c_tag_dead)
				report_compact("double delete", c, p);
			if (aligned)
				report_compact("plain new freed by aligned delete", c, p);
			c->tag = c_tag_dead;
			account(reinterpret_cast<unsigned char*>(&__ImageBase) + c->caller_rva, -static_cast<std::int64_t>(c->size), -1);
			std::free(c);
			return;
		}
		auto* h = static_cast<header*>(p) - 1;
		if (h->magic == c_magic_dead && h->magic_inv == ~c_magic_dead)
			report("double delete", h, p);
		if (h->magic != c_magic_live || h->magic_inv != ~c_magic_live)
			report("header overwritten (or memory not from operator new)", h, p);
		if (aligned != (h->align != 0))
			report(aligned ? "plain new freed by aligned delete" : "aligned new freed by plain delete", h, p);
		const auto* tail = static_cast<const unsigned char*>(p) + h->size;
		for (std::size_t i = 0; i < c_tail; i++)
		{
			if (tail[i] != c_canary)
				report("block overrun", h, p);
		}
		h->magic = c_magic_dead;
		h->magic_inv = ~c_magic_dead;
		account(h->caller, -static_cast<std::int64_t>(h->size), -1);
		std::free(h->raw);
	}

	void* alloc_or_throw(std::size_t size, std::size_t align, void* caller)
	{
		if (size == 0)
			size = 1;
		for (;;)
		{
			if (void* p = guarded_alloc(size, align, caller))
				return p;
			std::new_handler handler = std::get_new_handler();
			if (!handler)
				__fastfail(7); // FAST_FAIL_FATAL_APP_EXIT: out of memory, built without exceptions
			handler();
		}
	}
}

// Live operator new bytes and blocks, and the callers holding the most (exe-relative addresses).
extern "C" void uwp_heap_guard_summary(char* buf, std::size_t size, unsigned top)
{
	struct entry { std::uintptr_t caller; std::int64_t bytes, count; };
	entry best[16]{};
	top = top > 16 ? 16 : top;
	for (auto& slot : s_callers)
	{
		const std::uintptr_t caller = slot.caller.load(std::memory_order_relaxed);
		const std::int64_t bytes = slot.bytes.load(std::memory_order_relaxed);
		if (!caller || top == 0 || bytes <= best[top - 1].bytes)
			continue;
		unsigned i = top - 1;
		for (; i > 0 && best[i - 1].bytes < bytes; i--)
			best[i] = best[i - 1];
		best[i] = {caller & ~std::uintptr_t{1}, bytes, slot.count.load(std::memory_order_relaxed)};
	}
	int n = std::snprintf(buf, size, "operator new live %lld MB in %lld blocks; top callers:",
		static_cast<long long>(s_live_bytes.load() >> 20), static_cast<long long>(s_live_count.load()));
	for (unsigned i = 0; i < top && best[i].caller && n > 0 && static_cast<std::size_t>(n) < size; i++)
		n += std::snprintf(buf + n, size - n, " exe+0x%llx %lld MB/%lld", static_cast<unsigned long long>(rva(reinterpret_cast<void*>(best[i].caller))),
			static_cast<long long>(best[i].bytes >> 20), static_cast<long long>(best[i].count));
}

// Turns the full guard on for blocks allocated from now on (LocalState/heap_guard).
extern "C" void uwp_heap_guard_open_log(const wchar_t* path)
{
	s_full_guard = true;
	s_log = CreateFile2(path, GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, nullptr);
}

#define RA __builtin_return_address(0)
void* operator new(std::size_t size) { return alloc_or_throw(size, 0, RA); }
void* operator new[](std::size_t size) { return alloc_or_throw(size, 0, RA); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
	return guarded_alloc(size ? size : 1, 0, RA);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
	return guarded_alloc(size ? size : 1, 0, RA);
}
void operator delete(void* p) noexcept { guarded_free(p, false); }
void operator delete[](void* p) noexcept { guarded_free(p, false); }
void operator delete(void* p, const std::nothrow_t&) noexcept { guarded_free(p, false); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { guarded_free(p, false); }
void operator delete(void* p, std::size_t) noexcept { guarded_free(p, false); }
void operator delete[](void* p, std::size_t) noexcept { guarded_free(p, false); }

// Over-aligned allocations are guarded the same way; a mismatched new/delete pair is reported.
void* operator new(std::size_t size, std::align_val_t al) { return alloc_or_throw(size, static_cast<std::size_t>(al), RA); }
void* operator new[](std::size_t size, std::align_val_t al) { return alloc_or_throw(size, static_cast<std::size_t>(al), RA); }
void* operator new(std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept
{
	return guarded_alloc(size ? size : 1, static_cast<std::size_t>(al), RA);
}
void* operator new[](std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept
{
	return guarded_alloc(size ? size : 1, static_cast<std::size_t>(al), RA);
}
void operator delete(void* p, std::align_val_t) noexcept { guarded_free(p, true); }
void operator delete[](void* p, std::align_val_t) noexcept { guarded_free(p, true); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { guarded_free(p, true); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { guarded_free(p, true); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { guarded_free(p, true); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { guarded_free(p, true); }
