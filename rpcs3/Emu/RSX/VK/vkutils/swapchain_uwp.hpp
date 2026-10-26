#pragma once

#include "swapchain_core.h"
#include "util/logs.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

LOG_CHANNEL(uwp_present_log, "UWPPRESENT");

// UWP has no Vulkan window-system integration (Dozen only presents to HWNDs). Frames are
// copied to host memory like the other native swapchains and handed to the app shell,
// which shows them in its own CoreWindow swapchain.
extern void uwp_get_display_size(u32& width, u32& height);
extern void uwp_present_frame(const void* bgra_pixels, u32 width, u32 height);

namespace vk
{
	void acquire_global_submit_lock();
	void release_global_submit_lock();
	extern atomic_t<u64> g_uwp_present_wait_us, g_uwp_present_copy_us; // VKCommandStream.cpp

	// Off: the RSX thread copies each frame to the shell itself, as before (LocalState/sync_present_copy).
	extern bool g_uwp_async_present_copy; // VKCommandStream.cpp

	class swapchain_UWP : public native_swapchain_base
	{
		// present() runs right after the frame's commands are submitted, before the GPU has
		// copied the image to its host buffer. Reading it then shows an old or half-written
		// frame (judder). Instead each present queues a fence behind the frame and hands the
		// previous frame to the shell once its fence has signalled (one frame of latency).
		std::vector<VkFence> m_fences;
		s32 m_pending = -1;

		// The fence wait and the 8 MB copy to the shell (~1.3 ms a frame on the Xbox, on the RSX
		// thread, which limits Minecraft in the world) run on a frame copier thread. An image stays
		// acquired until the copier is done with it; m_released tells the RSX thread it's free again.
		// Images are acquired at the flip, a frame after the previous hand-over, so the copy (~1.3 ms)
		// is long done by then and three images still do (also the number of RSX frame contexts).
		std::thread m_copier;
		std::mutex m_copier_mutex;
		std::condition_variable m_copier_cv;
		s32 m_copy_queued = -1;
		bool m_copier_quit = false;
		std::unique_ptr<std::atomic<bool>[]> m_released;

		// Frame pacing stats, logged every c_stats_frames presents.
		static constexpr u32 c_stats_frames = 300;
		u64 m_last_present_us = 0;
		u64 m_interval_sum_us = 0, m_interval_max_us = 0;
		std::atomic<u64> m_wait_sum_us = 0, m_copy_sum_us = 0;
		u32 m_stats_count = 0;

		static u64 now_us()
		{
			LARGE_INTEGER t, f;
			QueryPerformanceCounter(&t);
			QueryPerformanceFrequency(&f);
			return static_cast<u64>(t.QuadPart) * 1000000 / static_cast<u64>(f.QuadPart);
		}

		// Waits for the image's fence and gives its pixels to the shell.
		void copy_out(u32 image)
		{
			const u64 t0 = now_us();
			vkWaitForFences(dev, 1, &m_fences[image], VK_TRUE, 1'000'000'000ull);
			const u64 t1 = now_us();
			auto& src = swapchain_images[image];
			uwp_present_frame(src.second->get_pixels(), m_width, m_height);
			src.second->free_pixels();
			const u64 t2 = now_us();
			m_wait_sum_us += t1 - t0;
			m_copy_sum_us += t2 - t1;
			g_uwp_present_wait_us += t1 - t0;
			g_uwp_present_copy_us += t2 - t1;
		}

		void hand_over(u32 image)
		{
			if (!m_copier.joinable())
			{
				copy_out(image);
				swapchain_images[image].first = false;
				return;
			}

			std::lock_guard lock(m_copier_mutex);
			// A frame the copier hasn't started yet is dropped for the newer one (the shell only
			// shows the newest frame anyway).
			if (m_copy_queued >= 0)
				m_released[m_copy_queued] = true;
			m_copy_queued = static_cast<s32>(image);
			m_copier_cv.notify_one();
		}

		void copier_loop()
		{
			std::unique_lock lock(m_copier_mutex);
			while (true)
			{
				m_copier_cv.wait(lock, [this] { return m_copier_quit || m_copy_queued >= 0; });
				if (m_copy_queued < 0)
					return;
				const u32 image = static_cast<u32>(m_copy_queued);
				m_copy_queued = -1;
				lock.unlock();
				copy_out(image);
				m_released[image] = true;
				lock.lock();
			}
		}

		void start_copier()
		{
			m_released = std::make_unique<std::atomic<bool>[]>(swapchain_images.size());
			m_copy_queued = -1;
			m_copier_quit = false;
			if (g_uwp_async_present_copy)
				m_copier = std::thread([this] { copier_loop(); });
		}

		// Copies a frame still queued, then stops the copier thread.
		void stop_copier()
		{
			if (!m_copier.joinable())
				return;
			{
				std::lock_guard lock(m_copier_mutex);
				m_copier_quit = true;
				m_copier_cv.notify_one();
			}
			m_copier.join();
			collect_released();
		}

		void collect_released()
		{
			for (usz i = 0; m_released && i < swapchain_images.size(); i++)
			{
				if (m_released[i].exchange(false))
					swapchain_images[i].first = false;
			}
		}

		void release_fences()
		{
			stop_copier();
			if (m_pending >= 0)
			{
				vkWaitForFences(dev, 1, &m_fences[m_pending], VK_TRUE, 1'000'000'000ull);
				swapchain_images[m_pending].first = false;
				m_pending = -1;
			}
			for (VkFence f : m_fences)
				vkDestroyFence(dev, f, nullptr);
			m_fences.clear();
		}

	public:
		using native_swapchain_base::native_swapchain_base;

		~swapchain_UWP() override
		{
			stop_copier();
		}

		bool init() override
		{
			if (!swapchain_images.empty())
				destroy(false);

			uwp_get_display_size(m_width, m_height);
			init_swapchain_images(dev, 3);

			m_fences.resize(swapchain_images.size());
			for (VkFence& f : m_fences)
			{
				const VkFenceCreateInfo info{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
				vkCreateFence(dev, &info, nullptr, &f);
			}
			start_copier();
			return true;
		}

		void create(display_handle_t& handle) override
		{
			window_handle = handle;
		}

		void destroy(bool full = true) override
		{
			release_fences();
			swapchain_images.clear();

			if (full)
				dev.destroy();
		}

		VkResult acquire_next_swapchain_image(VkSemaphore semaphore, u64 timeout, u32* result) override
		{
			collect_released();
			return native_swapchain_base::acquire_next_swapchain_image(semaphore, timeout, result);
		}

		VkResult present(VkSemaphore /*semaphore*/, u32 image) override
		{
			// Fence behind this frame's work (queue order: signals once the copy is done). A frame the
			// copier dropped may come back before its fence signalled; a pending fence can't be reset.
			vkWaitForFences(dev, 1, &m_fences[image], VK_TRUE, 1'000'000'000ull);
			vkResetFences(dev, 1, &m_fences[image]);
			acquire_global_submit_lock();
			vkQueueSubmit(dev.get_graphics_queue(), 0, nullptr, m_fences[image]);
			release_global_submit_lock();

			if (m_pending >= 0)
				hand_over(static_cast<u32>(m_pending));
			m_pending = static_cast<s32>(image);

			const u64 now = now_us();
			if (!m_last_present_us)
				uwp_present_log.notice("First frame presented (frame copy on %s)", m_copier.joinable() ? "its own thread" : "the RSX thread");
			if (m_last_present_us)
			{
				const u64 interval = now - m_last_present_us;
				if (interval >= 250'000)
					uwp_present_log.notice("Frame gap of %.3f s", interval / 1e6);
				m_interval_sum_us += interval;
				m_interval_max_us = std::max(m_interval_max_us, interval);
				if (++m_stats_count == c_stats_frames)
				{
					uwp_present_log.notice(" %.1f fps avg, frame interval avg %.1f ms max %.1f ms, GPU wait %.2f ms, copy %.2f ms",
						1e6 * m_stats_count / m_interval_sum_us, m_interval_sum_us / 1000.0 / m_stats_count, m_interval_max_us / 1000.0,
						m_wait_sum_us.exchange(0) / 1000.0 / m_stats_count, m_copy_sum_us.exchange(0) / 1000.0 / m_stats_count);
					m_stats_count = 0;
					m_interval_sum_us = m_interval_max_us = 0;
				}
			}
			m_last_present_us = now;
			return VK_SUCCESS;
		}
	};

	using swapchain_NATIVE = swapchain_UWP;

	[[maybe_unused]] static
	VkSurfaceKHR make_WSI_surface(VkInstance, display_handle_t, WSI_config*)
	{
		return VK_NULL_HANDLE;
	}
}
