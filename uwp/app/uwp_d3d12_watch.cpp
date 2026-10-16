// D3D12 object accounting for the Xbox (no PIX or debug layer in a UWP title): patches the D3D12
// device's vtable so every resource, heap, descriptor heap, pipeline state and command object
// Dozen creates is counted while it lives. On the console D3D12 memory comes out of the same 5 GB
// as everything else, and shows up only as anonymous private allocations in VirtualQuery.

#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <unordered_map>

extern "C" void* uwp_lazy_lookup(const wchar_t* dll, const char* name);

namespace
{
	enum kind : unsigned
	{
		k_res_default, k_res_upload, k_res_readback, k_res_custom,
		k_heap_default, k_heap_upload, k_heap_readback, k_heap_custom,
		k_placed, k_desc_cpu, k_desc_gpu, k_pso, k_root_sig, k_cmd_alloc, k_cmd_list, k_query_heap,
		k_count
	};

	const char* const kind_names[k_count] = {
		"committed default", "committed upload", "committed readback", "committed custom",
		"heap default", "heap upload", "heap readback", "heap custom",
		"placed", "desc heaps cpu", "desc heaps gpu", "PSOs", "root sigs", "cmd allocators", "cmd lists", "query heaps",
	};

	struct counter
	{
		std::atomic<long long> live{0}, bytes{0}, created{0};
	};

	counter g_counters[k_count];

	struct entry
	{
		kind k;
		unsigned long long bytes;
	};

	std::recursive_mutex g_lock;
	std::unordered_map<void*, entry>* g_live; // never destroyed (objects can outlive static destructors)

	// Release of each object class we have seen, keyed by its vtable.
	struct release_hook
	{
		void* vtbl;
		ULONG(STDMETHODCALLTYPE* orig)(IUnknown*);
	};
	release_hook g_release[32];
	unsigned g_release_count;

	bool patch(void** slot, void* fn, void** orig)
	{
		DWORD old;
		if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
			return false;
		if (orig)
			*orig = *slot;
		*slot = fn;
		VirtualProtect(slot, sizeof(void*), old, &old);
		return true;
	}

	ULONG STDMETHODCALLTYPE hooked_release(IUnknown* obj)
	{
		std::lock_guard lock(g_lock);
		void* vtbl = obj->lpVtbl;
		ULONG(STDMETHODCALLTYPE* orig)(IUnknown*) = nullptr;
		for (unsigned i = 0; i < g_release_count; i++)
			if (g_release[i].vtbl == vtbl)
				orig = g_release[i].orig;

		const ULONG left = orig(obj);
		if (!left)
		{
			if (auto it = g_live->find(obj); it != g_live->end())
			{
				g_counters[it->second.k].live--;
				g_counters[it->second.k].bytes -= it->second.bytes;
				g_live->erase(it);
			}
		}
		return left;
	}

	void track(HRESULT hr, void** out, kind k, unsigned long long bytes)
	{
		if (FAILED(hr) || !out || !*out)
			return;

		auto* obj = static_cast<IUnknown*>(*out);
		std::lock_guard lock(g_lock);
		g_counters[k].created++;
		g_counters[k].live++;
		g_counters[k].bytes += bytes;
		(*g_live)[obj] = {k, bytes};

		void* vtbl = obj->lpVtbl;
		for (unsigned i = 0; i < g_release_count; i++)
			if (g_release[i].vtbl == vtbl)
				return;
		if (g_release_count < std::size(g_release))
		{
			void* orig = nullptr;
			if (patch(reinterpret_cast<void**>(&obj->lpVtbl->Release), reinterpret_cast<void*>(&hooked_release), &orig))
				g_release[g_release_count++] = {vtbl, reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(IUnknown*)>(orig)};
		}
	}

	kind heap_kind(D3D12_HEAP_TYPE type, kind base)
	{
		switch (type)
		{
		case D3D12_HEAP_TYPE_UPLOAD: return kind(base + 1);
		case D3D12_HEAP_TYPE_READBACK: return kind(base + 2);
		case D3D12_HEAP_TYPE_CUSTOM: return kind(base + 3);
		default: return base;
		}
	}

	ID3D12DeviceVtbl* g_orig; // copy of the original device vtable entries

	unsigned long long resource_bytes(ID3D12Device* dev, const D3D12_RESOURCE_DESC* desc)
	{
		if (!desc)
			return 0;
		if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
			return desc->Width;

		const UINT subresources = desc->MipLevels * (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc->DepthOrArraySize);
		UINT64 total = 0;
		g_orig->GetCopyableFootprints(dev, desc, 0, subresources ? subresources : 1, 0, nullptr, nullptr, nullptr, &total);
		return total == ~0ull ? 0 : total * (desc->SampleDesc.Count ? desc->SampleDesc.Count : 1);
	}

	D3D12_RESOURCE_DESC to_desc(const D3D12_RESOURCE_DESC1* d)
	{
		D3D12_RESOURCE_DESC r{};
		if (d)
			r = {d->Dimension, d->Alignment, d->Width, d->Height, d->DepthOrArraySize, d->MipLevels, d->Format, d->SampleDesc, d->Layout, d->Flags};
		return r;
	}

	decltype(ID3D12Device4Vtbl::CreateCommittedResource1) o_ccr1;
	decltype(ID3D12Device8Vtbl::CreateCommittedResource2) o_ccr2;
	decltype(ID3D12Device10Vtbl::CreateCommittedResource3) o_ccr3;
	decltype(ID3D12Device4Vtbl::CreateHeap1) o_heap1;
	decltype(ID3D12Device2Vtbl::CreatePipelineState) o_pso_stream;
	decltype(ID3D12Device4Vtbl::CreateCommandList1) o_cmd_list1;

	HRESULT STDMETHODCALLTYPE h_ccr(ID3D12Device* dev, const D3D12_HEAP_PROPERTIES* props, D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC* desc,
		D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out)
	{
		const HRESULT hr = g_orig->CreateCommittedResource(dev, props, flags, desc, state, clear, riid, out);
		track(hr, out, heap_kind(props ? props->Type : D3D12_HEAP_TYPE_DEFAULT, k_res_default), resource_bytes(dev, desc));
		return hr;
	}

	HRESULT STDMETHODCALLTYPE h_ccr1(ID3D12Device4* dev, const D3D12_HEAP_PROPERTIES* props, D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC* desc,
		D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* session, REFIID riid, void** out)
	{
		const HRESULT hr = o_ccr1(dev, props, flags, desc, state, clear, session, riid, out);
		track(hr, out, heap_kind(props ? props->Type : D3D12_HEAP_TYPE_DEFAULT, k_res_default), resource_bytes(reinterpret_cast<ID3D12Device*>(dev), desc));
		return hr;
	}

	HRESULT STDMETHODCALLTYPE h_ccr2(ID3D12Device8* dev, const D3D12_HEAP_PROPERTIES* props, D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC1* desc,
		D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* session, REFIID riid, void** out)
	{
		const HRESULT hr = o_ccr2(dev, props, flags, desc, state, clear, session, riid, out);
		const D3D12_RESOURCE_DESC d = to_desc(desc);
		track(hr, out, heap_kind(props ? props->Type : D3D12_HEAP_TYPE_DEFAULT, k_res_default), resource_bytes(reinterpret_cast<ID3D12Device*>(dev), desc ? &d : nullptr));
		return hr;
	}

	HRESULT STDMETHODCALLTYPE h_ccr3(ID3D12Device10* dev, const D3D12_HEAP_PROPERTIES* props, D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC1* desc,
		D3D12_BARRIER_LAYOUT layout, const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* session, UINT32 n_formats, DXGI_FORMAT* formats, REFIID riid, void** out)
	{
		const HRESULT hr = o_ccr3(dev, props, flags, desc, layout, clear, session, n_formats, formats, riid, out);
		const D3D12_RESOURCE_DESC d = to_desc(desc);
		track(hr, out, heap_kind(props ? props->Type : D3D12_HEAP_TYPE_DEFAULT, k_res_default), resource_bytes(reinterpret_cast<ID3D12Device*>(dev), desc ? &d : nullptr));
		return hr;
	}

	HRESULT STDMETHODCALLTYPE h_heap(ID3D12Device* dev, const D3D12_HEAP_DESC* desc, REFIID riid, void** out)
	{
		const HRESULT hr = g_orig->CreateHeap(dev, desc, riid, out);
		track(hr, out, heap_kind(desc ? desc->Properties.Type : D3D12_HEAP_TYPE_DEFAULT, k_heap_default), desc ? desc->SizeInBytes : 0);
		return hr;
	}

	HRESULT STDMETHODCALLTYPE h_heap1(ID3D12Device4* dev, const D3D12_HEAP_DESC* desc, ID3D12ProtectedResourceSession* session, REFIID riid, void** out)
	{
		const HRESULT hr = o_heap1(dev, desc, session, riid, out);
		track(hr, out, heap_kind(desc ? desc->Properties.Type : D3D12_HEAP_TYPE_DEFAULT, k_heap_default), desc ? desc->SizeInBytes : 0);
		return hr;
	}

	HRESULT STDMETHODCALLTYPE h_placed(ID3D12Device* dev, ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC* desc,
		D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out)
	{
		const HRESULT hr = g_orig->CreatePlacedResource(dev, heap, offset, desc, state, clear, riid, out);
		track(hr, out, k_placed, resource_bytes(dev, desc));
		return hr;
	}

	HRESULT STDMETHODCALLTYPE h_desc_heap(ID3D12Device* dev, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid, void** out)
	{
		const HRESULT hr = g_orig->CreateDescriptorHeap(dev, desc, riid, out);
		if (desc)
		{
			const UINT inc = g_orig->GetDescriptorHandleIncrementSize(dev, desc->Type);
			track(hr, out, (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) ? k_desc_gpu : k_desc_cpu, 1ull * desc->NumDescriptors * inc);
		}
		return hr;
	}

	HRESULT STDMETHODCALLTYPE h_gfx_pso(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** out)
	{
		const HRESULT hr = g_orig->CreateGraphicsPipelineState(dev, desc, riid, out);
		track(hr, out, k_pso, desc ? desc->VS.BytecodeLength + desc->PS.BytecodeLength + desc->GS.BytecodeLength : 0);
		return hr;
	}

	HRESULT STDMETHODCALLTYPE h_cs_pso(ID3D12Device* dev, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid, void** out)
	{
		const HRESULT hr = g_orig->CreateComputePipelineState(dev, desc, riid, out);
		track(hr, out, k_pso, desc ? desc->CS.BytecodeLength : 0);
		return hr;
	}

	// Sums the shader bytecode in a pipeline state stream (the bytes counted for PSOs are DXIL sizes).
	unsigned long long stream_bytecode(const D3D12_PIPELINE_STATE_STREAM_DESC* desc)
	{
		if (!desc || !desc->pPipelineStateSubobjectStream)
			return 0;
		unsigned long long total = 0;
		auto* p = static_cast<const char*>(desc->pPipelineStateSubobjectStream);
		const auto* end = p + desc->SizeInBytes;
		while (p + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= end)
		{
			const auto type = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*>(p);
			// Each subobject is its type (padded to pointer alignment) followed by its data, padded to pointer size.
			std::size_t data_size = 0, data_align = sizeof(void*);
			switch (type)
			{
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
				total += reinterpret_cast<const D3D12_SHADER_BYTECODE*>(p + sizeof(void*))->BytecodeLength;
				data_size = sizeof(D3D12_SHADER_BYTECODE);
				break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: data_size = sizeof(void*); break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: data_size = sizeof(D3D12_STREAM_OUTPUT_DESC); break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: data_size = sizeof(D3D12_BLEND_DESC); data_align = 4; break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: data_size = sizeof(UINT); data_align = 4; break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: data_size = sizeof(D3D12_RASTERIZER_DESC); data_align = 4; break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: data_size = sizeof(D3D12_DEPTH_STENCIL_DESC); data_align = 4; break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: data_size = sizeof(D3D12_INPUT_LAYOUT_DESC); break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: data_size = sizeof(UINT); data_align = 4; break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: data_size = sizeof(UINT); data_align = 4; break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: data_size = sizeof(D3D12_RT_FORMAT_ARRAY); data_align = 4; break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: data_size = sizeof(UINT); data_align = 4; break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: data_size = sizeof(DXGI_SAMPLE_DESC); data_align = 4; break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: data_size = sizeof(UINT); data_align = 4; break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: data_size = sizeof(D3D12_CACHED_PIPELINE_STATE); break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: data_size = sizeof(UINT); data_align = 4; break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: data_size = sizeof(D3D12_DEPTH_STENCIL_DESC1); data_align = 4; break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: data_size = sizeof(D3D12_VIEW_INSTANCING_DESC); break;
			default: return total; // unknown layout: stop counting rather than misparse
			}
			const std::size_t offset = (sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) + data_align - 1) & ~(data_align - 1);
			p += (offset + data_size + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
		}
		return total;
	}

	HRESULT STDMETHODCALLTYPE h_pso_stream(ID3D12Device2* dev, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid, void** out)
	{
		const HRESULT hr = o_pso_stream(dev, desc, riid, out);
		track(hr, out, k_pso, stream_bytecode(desc));
		return hr;
	}

	HRESULT STDMETHODCALLTYPE h_root_sig(ID3D12Device* dev, UINT mask, const void* blob, SIZE_T size, REFIID riid, void** out)
	{
		const HRESULT hr = g_orig->CreateRootSignature(dev, mask, blob, size, riid, out);
		track(hr, out, k_root_sig, size);
		return hr;
	}

	HRESULT STDMETHODCALLTYPE h_cmd_alloc(ID3D12Device* dev, D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** out)
	{
		const HRESULT hr = g_orig->CreateCommandAllocator(dev, type, riid, out);
		track(hr, out, k_cmd_alloc, 0);
		return hr;
	}

	HRESULT STDMETHODCALLTYPE h_cmd_list(ID3D12Device* dev, UINT mask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* alloc, ID3D12PipelineState* pso, REFIID riid, void** out)
	{
		const HRESULT hr = g_orig->CreateCommandList(dev, mask, type, alloc, pso, riid, out);
		track(hr, out, k_cmd_list, 0);
		return hr;
	}

	HRESULT STDMETHODCALLTYPE h_cmd_list1(ID3D12Device4* dev, UINT mask, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void** out)
	{
		const HRESULT hr = o_cmd_list1(dev, mask, type, flags, riid, out);
		track(hr, out, k_cmd_list, 0);
		return hr;
	}

	HRESULT STDMETHODCALLTYPE h_query_heap(ID3D12Device* dev, const D3D12_QUERY_HEAP_DESC* desc, REFIID riid, void** out)
	{
		const HRESULT hr = g_orig->CreateQueryHeap(dev, desc, riid, out);
		track(hr, out, k_query_heap, desc ? desc->Count * 8ull : 0);
		return hr;
	}

	char g_status[160] = "not installed";
}

// Called once before the Vulkan device is created (Dozen has loaded d3d12.dll by then).
extern "C" void uwp_d3d12_watch_install()
{
	static bool done = false;
	if (done)
		return;
	done = true;

	using create_fn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
	const auto create = reinterpret_cast<create_fn>(uwp_lazy_lookup(L"d3d12.dll", "D3D12CreateDevice"));
	if (!create)
	{
		std::snprintf(g_status, sizeof(g_status), "d3d12.dll not loaded");
		return;
	}

	ID3D12Device* dev = nullptr;
	const HRESULT hr = create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device, reinterpret_cast<void**>(&dev));
	if (FAILED(hr) || !dev)
	{
		std::snprintf(g_status, sizeof(g_status), "D3D12CreateDevice failed (0x%08lx)", static_cast<unsigned long>(hr));
		return;
	}

	g_live = new std::unordered_map<void*, entry>();
	ID3D12DeviceVtbl* vt = dev->lpVtbl;
	g_orig = new ID3D12DeviceVtbl(*vt);

	auto slot = [&](auto& member) { return reinterpret_cast<void**>(&member); };
	auto hook = [&](auto& member, auto fn) { patch(slot(member), reinterpret_cast<void*>(fn), nullptr); };

	hook(vt->CreateCommittedResource, &h_ccr);
	hook(vt->CreateHeap, &h_heap);
	hook(vt->CreatePlacedResource, &h_placed);
	hook(vt->CreateDescriptorHeap, &h_desc_heap);
	hook(vt->CreateGraphicsPipelineState, &h_gfx_pso);
	hook(vt->CreateComputePipelineState, &h_cs_pso);
	hook(vt->CreateRootSignature, &h_root_sig);
	hook(vt->CreateCommandAllocator, &h_cmd_alloc);
	hook(vt->CreateCommandList, &h_cmd_list);
	hook(vt->CreateQueryHeap, &h_query_heap);

	// Later interface versions extend the same vtable; only touch the entries the device implements.
	unsigned version = 0;
	auto supports = [&](REFIID iid) {
		void* p = nullptr;
		if (FAILED(dev->lpVtbl->QueryInterface(dev, iid, &p)) || !p)
			return false;
		const bool same = static_cast<ID3D12Device*>(p)->lpVtbl == vt;
		static_cast<ID3D12Device*>(p)->lpVtbl->Release(static_cast<ID3D12Device*>(p));
		return same;
	};

	if (supports(IID_ID3D12Device2))
	{
		version = 2;
		auto* v2 = reinterpret_cast<ID3D12Device2Vtbl*>(vt);
		o_pso_stream = v2->CreatePipelineState;
		hook(v2->CreatePipelineState, &h_pso_stream);
	}
	if (supports(IID_ID3D12Device4))
	{
		version = 4;
		auto* v4 = reinterpret_cast<ID3D12Device4Vtbl*>(vt);
		o_ccr1 = v4->CreateCommittedResource1;
		o_heap1 = v4->CreateHeap1;
		o_cmd_list1 = v4->CreateCommandList1;
		hook(v4->CreateCommittedResource1, &h_ccr1);
		hook(v4->CreateHeap1, &h_heap1);
		hook(v4->CreateCommandList1, &h_cmd_list1);
	}
	if (supports(IID_ID3D12Device8))
	{
		version = 8;
		auto* v8 = reinterpret_cast<ID3D12Device8Vtbl*>(vt);
		o_ccr2 = v8->CreateCommittedResource2;
		hook(v8->CreateCommittedResource2, &h_ccr2);
	}
	if (supports(IID_ID3D12Device10))
	{
		version = 10;
		auto* v10 = reinterpret_cast<ID3D12Device10Vtbl*>(vt);
		o_ccr3 = v10->CreateCommittedResource3;
		hook(v10->CreateCommittedResource3, &h_ccr3);
	}

	std::snprintf(g_status, sizeof(g_status), "watching device %p (vtable %p, ID3D12Device%u)", static_cast<void*>(dev), static_cast<void*>(vt), version);
	dev->lpVtbl->Release(dev); // the runtime keeps one device per adapter: Dozen holds it too
}

// "D3D12 live: <kind> N/X MB ..." for the memory log (created counts after the slash in brackets).
extern "C" void uwp_d3d12_watch_summary(char* buf, std::size_t size)
{
	if (!g_live)
	{
		std::snprintf(buf, size, "%s", g_status);
		return;
	}

	std::size_t pos = 0;
	long long total = 0;
	for (unsigned k = 0; k < k_count && pos < size; k++)
	{
		const long long live = g_counters[k].live, bytes = g_counters[k].bytes, created = g_counters[k].created;
		total += (k == k_placed || k == k_pso || k == k_root_sig) ? 0 : bytes;
		if (!created)
			continue;
		pos += std::snprintf(buf + pos, size - pos, "%s%s %lld (%lld made) %.1f MB", pos ? ", " : "", kind_names[k], live, created, bytes / 1048576.0);
	}
	if (pos < size)
		std::snprintf(buf + pos, size - pos, " | memory-owning total %.0f MB", total / 1048576.0);
}
