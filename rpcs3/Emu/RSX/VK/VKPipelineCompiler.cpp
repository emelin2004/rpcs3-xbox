#include "stdafx.h"
#include "VKPipelineCompiler.h"
#include "VKRenderPass.h"
#include "vkutils/device.h"
#include "Utilities/Thread.h"
#include "Emu/system_config.h"

#include "util/sysinfo.hpp"

#include "Emu/Cell/timers.hpp"

#include <functional>
#include <thread>

#ifdef RPCS3_UWP
// Stencil masks and depth bias are baked into the pipelines instead of being dynamic state
// (LocalState/dynamic_stencil_bias turns it off, uwp_main.cpp). Dozen on the Xbox's D3D12
// (no IndependentFrontAndBackStencilRefMask, no DynamicDepthBias) can't make those dynamic: it
// builds a D3D12 PSO per value set at the first draw that uses it, on the RSX thread, which
// stalled Portal 2 for 0.1-4 s at a time. Baked in, the PSO is made when the pipeline is
// created, which is on the async pipeline compiler threads.
bool g_uwp_static_pipeline_state = true;

// Dozen keeps the SPIR-V -> DXIL translation of every pipeline in the VkPipelineCache, which RPCS3
// creates empty at each boot: on the Xbox that is 0.4-1.5 s of CPU per pipeline, ~750 pipelines
// (8 minutes of compile) in a Portal 2 session with warm caches. Kept per game in this file
// (VKGSRender.cpp sets it), loaded at start and saved in the background after new pipelines.
std::string g_uwp_pipeline_cache_path;
bool g_uwp_pso_library = false;
#endif

namespace vk
{
	// Global list of worker threads
	std::unique_ptr<named_thread_group<pipe_compiler>> g_pipe_compilers;
	int g_num_pipe_compilers = 0;
	atomic_t<int> g_compiler_index{};
	VkPipelineCache g_pipeline_cache = VK_NULL_HANDLE;

#ifdef RPCS3_UWP
	atomic_t<u64> g_uwp_pipelines_created = 0; // VKProgramPipeline.cpp
	static atomic_t<u64> g_uwp_pipelines_saved = 0; // g_uwp_pipelines_created when the cache was loaded or last saved
	static std::unique_ptr<named_thread<std::function<void()>>> g_uwp_cache_saver;

	// What the saver does and since when, logged when a stop has to wait for it: on .77 three stops (PS Home,
	// 50 Cent) waited over 10 s for it, one with no new pipeline since its last 67 ms save.
	enum uwp_saver_phase : u32 { uwp_saver_waiting, uwp_saver_getting, uwp_saver_writing, uwp_saver_renaming };
	static atomic_t<u32> g_uwp_saver_phase = uwp_saver_waiting;
	static atomic_t<u64> g_uwp_saver_phase_since = 0;

	static void uwp_saver_set_phase(uwp_saver_phase phase)
	{
		g_uwp_saver_phase_since = get_system_time();
		g_uwp_saver_phase = phase;
	}

	static void uwp_save_pipeline_cache()
	{
		const auto start = get_system_time();
		const u64 count = g_uwp_pipelines_created;
		std::vector<u8> data;
		uwp_saver_set_phase(uwp_saver_getting);

		for (int attempt = 0; attempt < 3; attempt++)
		{
			usz size = 0;
			if (vkGetPipelineCacheData(*g_render_device, g_pipeline_cache, &size, nullptr) != VK_SUCCESS || !size)
				return;

			if (size > 512ull << 20)
			{
				rsx_log.error("UWP: pipeline cache is %u MB, not saving it", size >> 20);
				return;
			}

			// Room for pipelines that finish between the two calls
			data.resize(size + (size >> 4));
			size = data.size();

			if (const VkResult r = vkGetPipelineCacheData(*g_render_device, g_pipeline_cache, &size, data.data()); r == VK_SUCCESS)
			{
				data.resize(size);
				break;
			}

			data.clear();
		}

		if (data.empty())
			return;

		// Timed apart: most saves take 0.1-0.3 s, but some took 7 s, 34 s (F1) and 254 s (Portal 2, with
		// the cache on a USB drive), and a stop waits for a save that is running. On 0.1.35.0 one write
		// took 969 s (GTA V, 11 MB) and a Portal 2 stop waited 173 s+ on one, so write and rename are timed
		// apart too. From here on only local copies are used: a stop may leave this thread writing.
		const auto got = get_system_time();
		const std::string path = g_uwp_pipeline_cache_path;
		const std::string tmp = path + ".tmp";
		uwp_saver_set_phase(uwp_saver_writing);
		const bool written = fs::write_file(tmp, fs::rewrite, data);
		const auto wrote = get_system_time();
		uwp_saver_set_phase(uwp_saver_renaming);
		if (written && fs::rename(tmp, path, true))
		{
			g_uwp_pipelines_saved = count;
			const auto end = get_system_time();
			const auto log = end - start >= 1'000'000 ? &rsx_log.warning : &rsx_log.notice;
			(*log)("UWP: saved the pipeline cache (%u KB, %u pipelines made this session) in %.0f ms (%.0f ms getting it, %.0f ms writing it, %.0f ms renaming it)",
				data.size() >> 10, count, (end - start) / 1000., (got - start) / 1000., (wrote - got) / 1000., (end - wrote) / 1000.);
		}
		else
			rsx_log.error("UWP: could not save the pipeline cache to %s (%s, after %.0f ms)", path, fs::g_tls_error, (get_system_time() - got) / 1000.);
	}

	static const char* uwp_saver_phase_name(u32 phase)
	{
		switch (phase)
		{
		case uwp_saver_getting: return "getting the cache data";
		case uwp_saver_writing: return "writing the file";
		case uwp_saver_renaming: return "renaming the file";
		default: return "waiting";
		}
	}
#endif

	pipe_compiler::pipe_compiler()
	{
		// TODO: Initialize workqueue
	}

	pipe_compiler::~pipe_compiler()
	{
		// TODO: Destroy and do cleanup
		m_pipeline_cache = VK_NULL_HANDLE;
	}

	void pipe_compiler::initialize(const vk::render_device* pdev, VkPipelineCache pipeline_cache)
	{
		m_device = pdev;
		m_pipeline_cache = pipeline_cache;
	}

	void pipe_compiler::operator()()
	{
		thread_ctrl::scoped_priority low_prio(-1);

		while (thread_ctrl::state() != thread_state::aborting)
		{
			for (auto&& job : m_work_queue.pop_all())
			{
				if (!job.is_graphics_job)
				{
					auto compiled = int_compile_compute_pipe(job.compute_data, job.inputs, job.flags);
					job.callback_func(compiled);
					continue;
				}

				if (job.create_info_func)
				{
					auto compiled = int_compile_graphics_pipe(job.create_info_func, job.inputs, {}, job.flags);
					job.callback_func(compiled);
					continue;
				}

#ifdef RPCS3_UWP
				if (job.deferred_shaders[0])
				{
					job.graphics_modules[0] = job.deferred_shaders[0]->compile_once();
					job.graphics_modules[1] = job.deferred_shaders[1]->compile_once();
				}
#endif
				auto compiled = int_compile_graphics_pipe(job.graphics_data, job.graphics_modules, job.inputs, {}, job.flags);
				job.callback_func(compiled);
			}

			thread_ctrl::wait_on(m_work_queue);
		}
	}

	std::unique_ptr<glsl::program> pipe_compiler::int_compile_compute_pipe(
		const VkComputePipelineCreateInfo& create_info,
		const std::vector<glsl::program_input>& cs_inputs,
		op_flags flags)
	{
		auto program = std::make_unique<glsl::program>(*m_device, create_info, cs_inputs);
		program->link(m_pipeline_cache, flags & SEPARATE_SHADER_OBJECTS);
		return program;
	}

	std::unique_ptr<glsl::program> pipe_compiler::int_compile_graphics_pipe(
		const VkGraphicsPipelineCreateInfo& create_info,
		const std::vector<glsl::program_input>& vs_inputs,
		const std::vector<glsl::program_input>& fs_inputs,
		op_flags flags)
	{
		auto program = std::make_unique<glsl::program>(*m_device, create_info, vs_inputs, fs_inputs);
		program->link(m_pipeline_cache, flags & SEPARATE_SHADER_OBJECTS);
		return program;
	}

	std::unique_ptr<glsl::program> pipe_compiler::int_compile_graphics_pipe(
		const vk::pipeline_props &create_info,
		VkShaderModule modules[2],
		const std::vector<glsl::program_input>& vs_inputs,
		const std::vector<glsl::program_input>& fs_inputs,
		op_flags flags)
	{
		VkPipelineShaderStageCreateInfo shader_stages[2] = {};
		shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		shader_stages[0].module = modules[0];
		shader_stages[0].pName = "main";

		shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		shader_stages[1].module = modules[1];
		shader_stages[1].pName = "main";

		std::vector<VkDynamicState> dynamic_state_descriptors;
		dynamic_state_descriptors.push_back(VK_DYNAMIC_STATE_VIEWPORT);
		dynamic_state_descriptors.push_back(VK_DYNAMIC_STATE_SCISSOR);
		dynamic_state_descriptors.push_back(VK_DYNAMIC_STATE_LINE_WIDTH);
		dynamic_state_descriptors.push_back(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
#ifdef RPCS3_UWP
		if (!g_uwp_static_pipeline_state)
#endif
		{
			dynamic_state_descriptors.push_back(VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK);
			dynamic_state_descriptors.push_back(VK_DYNAMIC_STATE_STENCIL_WRITE_MASK);
		}
		dynamic_state_descriptors.push_back(VK_DYNAMIC_STATE_STENCIL_REFERENCE);
#ifdef RPCS3_UWP
		if (!g_uwp_static_pipeline_state)
#endif
		dynamic_state_descriptors.push_back(VK_DYNAMIC_STATE_DEPTH_BIAS);

		auto pdss = &create_info.state.ds;
		VkPipelineDepthStencilStateCreateInfo ds2;
		if (g_render_device->get_depth_bounds_support()) [[likely]]
		{
			dynamic_state_descriptors.push_back(VK_DYNAMIC_STATE_DEPTH_BOUNDS);
		}
		else if (pdss->depthBoundsTestEnable)
		{
			rsx_log.warning("Depth bounds test is enabled in the pipeline object but not supported by the current driver.");

			ds2 = *pdss;
			pdss = &ds2;
			ds2.depthBoundsTestEnable = VK_FALSE;
		}

		VkPipelineDynamicStateCreateInfo dynamic_state_info = {};
		dynamic_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamic_state_info.pDynamicStates = dynamic_state_descriptors.data();
		dynamic_state_info.dynamicStateCount = ::size32(dynamic_state_descriptors);

		VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };

		VkPipelineViewportStateCreateInfo vp = {};
		vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		vp.viewportCount = 1;
		vp.scissorCount = 1;

		auto pmss = &create_info.state.ms;
		VkPipelineMultisampleStateCreateInfo ms2;
		ensure(pmss->rasterizationSamples == VkSampleCountFlagBits((create_info.renderpass_key >> 16) & 0xF)); // "Multisample state mismatch!"

		if (pmss->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT || pmss->sampleShadingEnable) [[unlikely]]
		{
			ms2 = *pmss;
			pmss = &ms2;

			if (ms2.rasterizationSamples != VK_SAMPLE_COUNT_1_BIT)
			{
				// Update the sample mask pointer
				ms2.pSampleMask = &create_info.state.temp_storage.msaa_sample_mask;
			}

			if (g_cfg.video.antialiasing_level == msaa_level::none && ms2.sampleShadingEnable)
			{
				// Do not compile with MSAA enabled if multisampling is disabled
				rsx_log.warning("MSAA is disabled globally but a shader with multi-sampling enabled was submitted for compilation.");
				ms2.sampleShadingEnable = VK_FALSE;
			}
		}

		// Rebase pointers from pipeline structure in case it is moved/copied
		VkPipelineColorBlendStateCreateInfo cs = create_info.state.cs;
		cs.pAttachments = create_info.state.att_state;

		VkPipelineRasterizationStateCreateInfo rs = create_info.state.rs;
		VkPipelineRasterizationProvokingVertexStateCreateInfoEXT provoking_vertex_state{};
		if (flags & USE_LAST_PROVOKING_VERTEX)
		{
			ensure(m_device->get_provoking_vertex_last_support());
			provoking_vertex_state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT;
			provoking_vertex_state.pNext = rs.pNext;
			provoking_vertex_state.provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
			rs.pNext = &provoking_vertex_state;
		}

		VkGraphicsPipelineCreateInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		info.pVertexInputState = &vi;
		info.pInputAssemblyState = &create_info.state.ia;
		info.pRasterizationState = &rs;
		info.pColorBlendState = &cs;
		info.pMultisampleState = pmss;
		info.pViewportState = &vp;
		info.pDepthStencilState = pdss;
		info.stageCount = 2;
		info.pStages = shader_stages;
		info.pDynamicState = &dynamic_state_info;
		info.layout = VK_NULL_HANDLE;
		info.basePipelineIndex = -1;
		info.basePipelineHandle = VK_NULL_HANDLE;
		info.renderPass = vk::get_renderpass(*m_device, create_info.renderpass_key);

		return int_compile_graphics_pipe(info, vs_inputs, fs_inputs, flags);
	}

	std::unique_ptr<glsl::program> pipe_compiler::int_compile_graphics_pipe(
		graphics_pipe_create_callback_t pipe_info_create_fn,
		const std::vector<glsl::program_input>& vs_inputs,
		const std::vector<glsl::program_input>& fs_inputs,
		op_flags flags)
	{
		VkGraphicsPipelineCreateInfo create_info = pipe_info_create_fn();
		return int_compile_graphics_pipe(create_info, vs_inputs, fs_inputs, flags);
	}

	std::unique_ptr<glsl::program> pipe_compiler::compile(
		const VkComputePipelineCreateInfo& create_info,
		op_flags flags, callback_t callback,
		const std::vector<glsl::program_input>& cs_inputs)
	{
		if (flags & COMPILE_INLINE)
		{
			return int_compile_compute_pipe(create_info, cs_inputs, flags);
		}

		m_work_queue.push(create_info, cs_inputs, flags, callback);
		return {};
	}

	std::unique_ptr<glsl::program> pipe_compiler::compile(
		const VkGraphicsPipelineCreateInfo& create_info,
		op_flags flags, callback_t /*callback*/,
		const std::vector<glsl::program_input>& vs_inputs,
		const std::vector<glsl::program_input>& fs_inputs)
	{
		// It is very inefficient to defer this as all pointers need to be saved
		ensure(flags & COMPILE_INLINE, "Asynchronous compilation is not allowed for raw graphics pipeline input");
		return int_compile_graphics_pipe(create_info, vs_inputs, fs_inputs, flags);
	}

	std::unique_ptr<glsl::program> pipe_compiler::compile(
		const vk::pipeline_props &create_info,
		VkShaderModule vs,
		VkShaderModule fs,
		op_flags flags, callback_t callback,
		const std::vector<glsl::program_input>& vs_inputs,
		const std::vector<glsl::program_input>& fs_inputs)
	{
		VkShaderModule modules[] = { vs, fs };
		if (flags & COMPILE_INLINE)
		{
			return int_compile_graphics_pipe(create_info, modules, vs_inputs, fs_inputs, flags);
		}

		m_work_queue.push(create_info, modules, vs_inputs, fs_inputs, flags, callback);
		return {};
	}

#ifdef RPCS3_UWP
	std::unique_ptr<glsl::program> pipe_compiler::compile(
		const vk::pipeline_props& create_info,
		glsl::shader* vs,
		glsl::shader* fs,
		op_flags flags, callback_t callback,
		const std::vector<glsl::program_input>& vs_inputs,
		const std::vector<glsl::program_input>& fs_inputs)
	{
		if (flags & COMPILE_INLINE)
		{
			VkShaderModule modules[] = { vs->compile_once(), fs->compile_once() };
			return int_compile_graphics_pipe(create_info, modules, vs_inputs, fs_inputs, flags);
		}

		glsl::shader* shaders[] = { vs, fs };
		m_work_queue.push(create_info, shaders, vs_inputs, fs_inputs, flags, callback);
		return {};
	}
#endif

	std::unique_ptr<glsl::program> pipe_compiler::compile(
		graphics_pipe_create_callback_t get_create_info,
		op_flags flags, callback_t callback,
		const std::vector<glsl::program_input>& vs_inputs,
		const std::vector<glsl::program_input>& fs_inputs)
	{
		if (flags & COMPILE_INLINE)
		{
			return int_compile_graphics_pipe(get_create_info, vs_inputs, fs_inputs, flags);
		}

		m_work_queue.push(get_create_info, vs_inputs, fs_inputs, flags, callback);
		return {};
	}

	int decay_num_worker_threads(int num_worker_threads)
	{
		if (num_worker_threads <= 0)
		{
			// Select a conservative but modern default for async pipeline compilation.
			// Older heuristics topped out too early on high-core CPUs and left large
			// shader bursts queued longer than necessary.
			const auto hw_threads = utils::get_thread_count();

			if (hw_threads >= 24)
			{
				num_worker_threads = 12;
			}
			else if (hw_threads >= 16)
			{
				num_worker_threads = 8;
			}
			else if (hw_threads > 12)
			{
				num_worker_threads = 6;
			}
			else if (hw_threads > 8)
			{
				num_worker_threads = 4;
			}
			else if (hw_threads == 8)
			{
				num_worker_threads = 2;
			}
			else
			{
				num_worker_threads = 1;
			}

			rsx_log.notice("Async pipeline compiler auto-selected %d worker(s) for %u host thread(s).",
				num_worker_threads, hw_threads);
		}

		return num_worker_threads;
	}

	void initialize_pipe_compiler(int num_worker_threads, VkPipelineCache pipe_cache)
	{
		num_worker_threads = decay_num_worker_threads(num_worker_threads);

		ensure(num_worker_threads >= 1);
		ensure(g_render_device); // "Cannot initialize pipe compiler before creating a logical device"

		// Create the shared pipeline cache
		if (!pipe_cache)
		{
			VkPipelineCacheCreateInfo drv_cache_info{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
#ifdef RPCS3_UWP
			std::vector<u8> initial;
			if (fs::file f{g_uwp_pipeline_cache_path}; !g_uwp_pipeline_cache_path.empty() && f)
				initial = f.to_vector<u8>();
			drv_cache_info.initialDataSize = initial.size();
			drv_cache_info.pInitialData = initial.data();

			if (vkCreatePipelineCache(*g_render_device, &drv_cache_info, nullptr, &g_pipeline_cache) != VK_SUCCESS && !initial.empty())
			{
				rsx_log.error("UWP: the driver refused the saved pipeline cache, starting empty");
				drv_cache_info.initialDataSize = 0;
				drv_cache_info.pInitialData = nullptr;
				vkCreatePipelineCache(*g_render_device, &drv_cache_info, nullptr, &g_pipeline_cache);
			}

			rsx_log.notice("UWP: pipeline cache: loaded %u KB from %s", initial.size() >> 10, g_uwp_pipeline_cache_path.empty() ? "(no game)" : g_uwp_pipeline_cache_path);

			if (g_pipeline_cache && !g_uwp_pipeline_cache_path.empty())
			{
				g_uwp_pipelines_saved = g_uwp_pipelines_created.load();
				uwp_saver_set_phase(uwp_saver_waiting);

				// Save 10 s after the last new pipeline, or every minute while they keep coming
				g_uwp_cache_saver = std::make_unique<named_thread<std::function<void()>>>("VK Pipeline Cache Saver", []()
				{
					u64 saved = g_uwp_pipelines_created, seen = saved, last_save = get_system_time();
					u64 last_check = get_system_time();

					while (thread_ctrl::state() != thread_state::aborting)
					{
						// Checked every 250 ms instead of one 10 s wait: a 50 Cent stop on .77 waited over 12.9 s
						// for an idle saver (no new pipeline since its last save), past its own 10 s timeout.
						thread_ctrl::wait_for(250'000);

						if (thread_ctrl::state() == thread_state::aborting)
							break;

						if (get_system_time() - last_check < 10'000'000)
							continue;

						last_check = get_system_time();

						const u64 now_count = g_uwp_pipelines_created;
						const bool quiet = now_count == seen;
						seen = now_count;

						if (now_count != saved && (quiet || get_system_time() - last_save >= 60'000'000))
						{
							uwp_save_pipeline_cache();
							uwp_saver_set_phase(uwp_saver_waiting);
							saved = now_count;
							last_save = get_system_time();
						}
					}
				});
			}
#else
			vkCreatePipelineCache(*g_render_device, &drv_cache_info, nullptr, &g_pipeline_cache);
#endif
		}
		else
		{
			g_pipeline_cache = pipe_cache;
		}

		// Create the thread pool
		g_pipe_compilers = std::make_unique<named_thread_group<pipe_compiler>>("RSX.W", num_worker_threads);
		g_num_pipe_compilers = num_worker_threads;

		// Initialize the workers. At least one inline compiler shall exist (doesn't actually run)
		for (pipe_compiler& compiler : *g_pipe_compilers.get())
		{
			compiler.initialize(g_render_device, g_pipeline_cache);
		}
	}

	void resize_pipe_compiler(int num_worker_threads)
	{
		num_worker_threads = decay_num_worker_threads(num_worker_threads);
		if (static_cast<u32>(num_worker_threads) <= g_pipe_compilers->size())
		{
			// Just lie about how many compilers we have
			g_num_pipe_compilers = num_worker_threads;
			return;
		}

		// We need a bigger thread pool. Very unlikely but provided for correctness.
		g_pipe_compilers.reset();
		g_num_pipe_compilers = 0;

		initialize_pipe_compiler(num_worker_threads, g_pipeline_cache);
	}

	void destroy_pipe_compiler()
	{
		g_pipe_compilers.reset();

#ifdef RPCS3_UWP
		if (g_uwp_cache_saver)
		{
			const u64 stop_start = get_system_time();
			const u32 phase = g_uwp_saver_phase;
			rsx_log.notice("UWP: stopping the pipeline cache saver (%s for %.0f ms)", uwp_saver_phase_name(phase), (stop_start - g_uwp_saver_phase_since) / 1000.);

			// A file write on the USB drive can hang for minutes (969 s in GTA V, 173 s+ in a Portal 2 stop
			// that the player then ended from Xbox Home). Give it 2 s, then let it finish in the background:
			// it only uses its own copies by then, and the tmp file + rename keeps the old cache intact if
			// the app ends first.
			const auto in_file_io = [] { const u32 p = g_uwp_saver_phase; return p == uwp_saver_writing || p == uwp_saver_renaming; };
			while (in_file_io() && get_system_time() - stop_start < 2'000'000)
				std::this_thread::sleep_for(std::chrono::milliseconds(10));

			bool left_running = false;
			if (in_file_io())
			{
				rsx_log.warning("UWP: the pipeline cache saver is stuck %s (%.0f ms), leaving it to finish in the background",
					uwp_saver_phase_name(g_uwp_saver_phase), (get_system_time() - g_uwp_saver_phase_since) / 1000.);
				*g_uwp_cache_saver = thread_state::aborting; // ends its loop after this save, without a join
				(void)g_uwp_cache_saver.release(); // never joined or freed; the thread ends on its own
				left_running = true;
			}
			else
			{
				g_uwp_cache_saver.reset();
			}

			if (const u64 took = get_system_time() - stop_start; took >= 1'000'000)
				rsx_log.warning("UWP: the pipeline cache saver took %.0f ms to stop", took / 1000.);

			// Not again when the saver already wrote everything: the stop waits for this save. Not either
			// while a write still hangs: this one would wait on the same drive.
			if (!left_running && g_pipeline_cache && g_uwp_pipelines_created != g_uwp_pipelines_saved)
				uwp_save_pipeline_cache();
		}
#endif

		if (g_pipeline_cache)
		{
			vkDestroyPipelineCache(*g_render_device, g_pipeline_cache, nullptr);
			g_pipeline_cache = VK_NULL_HANDLE;
		}
	}

	pipe_compiler* get_pipe_compiler()
	{
		ensure(g_pipe_compilers);
		int thread_index = g_compiler_index++;

		return g_pipe_compilers.get()->begin() + (thread_index % g_num_pipe_compilers);
	}
}
