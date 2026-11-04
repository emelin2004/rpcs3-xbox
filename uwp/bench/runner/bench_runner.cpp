// Headless PC runner for the bench/test ELFs (no Qt, Null renderer, no input).
// Usage: rpcs3_bench_runner [--ppu llvm|interp] [--spu llvm|asmjit|interp] [--timeout s] elf...
// Prints "RUN <elf>", the program's TTY output, then "END <elf> <exit> <seconds>s".

#include "stdafx.h"
#include "Emu/System.h"
#include "Emu/vfs_config.h"
#include "Emu/system_config.h"
#include "Emu/emu_callbacks.h"
#include "Emu/IdManager.h"
#include "Emu/RSX/Core/RSXRendererFactory.h"
#include "Emu/RSX/GSFrameBase.h"
#include "Emu/Cell/Modules/cellMsgDialog.h"
#include "Emu/Cell/Modules/cellOskDialog.h"
#include "Emu/Cell/Modules/cellSaveData.h"
#include "Emu/Cell/Modules/sceNpTrophy.h"
#include "Emu/Io/Null/null_camera_handler.h"
#include "Emu/Io/Null/null_music_handler.h"
#include "Emu/Io/Null/NullKeyboardHandler.h"
#include "Emu/Io/Null/NullMouseHandler.h"
#include "Emu/Io/KeyboardHandler.h"
#include "Emu/Io/MouseHandler.h"
#include "Emu/Audio/AudioBackend.h"
#include "Emu/Audio/Null/NullAudioBackend.h"
#include "Emu/Audio/Null/null_enumerator.h"
#include "Input/pad_thread.h"
#include "Utilities/File.h"
#include "util/logs.hpp"
#include "util/video_source.h"

#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

LOG_CHANNEL(sys_log, "SYS");

extern const std::map<std::string_view, int> g_prx_list;

// Normally in the Qt frontend: print and quit.
[[noreturn]] void report_fatal_error(std::string_view text, bool, bool)
{
	std::fprintf(stderr, "FATAL: %.*s\n", static_cast<int>(text.size()), text.data());
	logs::listener::sync_all();
	std::_Exit(2);
}

namespace
{
	std::mutex g_queue_mutex;
	std::deque<std::pair<std::function<void()>, atomic_t<u32>*>> g_queue;
	std::thread::id g_main_id;

	void run_queue()
	{
		while (true)
		{
			std::pair<std::function<void()>, atomic_t<u32>*> item;
			{
				std::lock_guard lock(g_queue_mutex);
				if (g_queue.empty())
					return;
				item = std::move(g_queue.front());
				g_queue.pop_front();
			}
			item.first();
			if (item.second)
			{
				*item.second = true;
				item.second->notify_one();
			}
		}
	}

	void create_callbacks()
	{
		auto& cb = g_emu_callbacks;
		cb.update_emu_settings = []() {};
		cb.save_emu_settings = []() {};
		cb.try_to_quit = [](bool force, std::function<void()> on_exit) { if (force && on_exit) on_exit(); return force; };
		cb.call_from_main_thread = [](std::function<void()> func, atomic_t<u32>* wake_up)
		{
			if (std::this_thread::get_id() == g_main_id)
			{
				func();
				if (wake_up)
				{
					*wake_up = true;
					wake_up->notify_one();
				}
				return;
			}
			std::lock_guard lock(g_queue_mutex);
			g_queue.emplace_back(std::move(func), wake_up);
		};
		cb.init_kb_handler = []() { ensure(g_fxo->init<KeyboardHandlerBase, NullKeyboardHandler>(Emu.DeserialManager())); };
		cb.init_mouse_handler = []() { ensure(g_fxo->init<MouseHandlerBase, NullMouseHandler>(Emu.DeserialManager())); };
		cb.init_pad_handler = [](std::string_view title_id) { ensure(g_fxo->init<named_thread<pad_thread>>(nullptr, nullptr, title_id)); };
		cb.get_audio = []() -> std::shared_ptr<AudioBackend> { return std::make_shared<NullAudioBackend>(); };
		cb.get_audio_enumerator = [](u64) -> std::shared_ptr<audio_device_enumerator> { return std::make_shared<null_enumerator>(); };
		cb.init_gs_render = [](utils::serial* ar) { rsx::create_render_thread(ar); };
		cb.close_gs_frame = []() {};
		cb.get_gs_frame = []() -> std::unique_ptr<GSFrameBase> { return {}; };
		cb.get_camera_handler = []() -> std::shared_ptr<camera_handler_base> { return std::make_shared<null_camera_handler>(); };
		cb.get_music_handler = []() -> std::shared_ptr<music_handler_base> { return std::make_shared<null_music_handler>(); };
		cb.get_msg_dialog = []() -> std::shared_ptr<MsgDialogBase> { return {}; };
		cb.get_osk_dialog = []() -> std::shared_ptr<OskDialogBase> { return {}; };
		cb.get_save_dialog = []() -> std::unique_ptr<SaveDialogBase> { return {}; };
		cb.get_sendmessage_dialog = []() -> std::shared_ptr<SendMessageDialogBase> { return {}; };
		cb.get_recvmessage_dialog = []() -> std::shared_ptr<RecvMessageDialogBase> { return {}; };
		cb.get_trophy_notification_dialog = []() -> std::unique_ptr<TrophyNotificationBase> { return {}; };
		cb.on_run = [](bool) {};
		cb.on_pause = []() {};
		cb.on_resume = []() {};
		cb.on_stop = []() {};
		cb.on_ready = []() {};
		cb.on_emulation_stop_no_response = [](std::shared_ptr<atomic_t<bool>>, int) {};
		cb.on_save_state_progress = [](std::shared_ptr<atomic_t<bool>>, stx::shared_ptr<utils::serial>, stx::atomic_ptr<std::string>*, std::shared_ptr<void>) {};
		cb.enable_disc_eject = [](bool) {};
		cb.enable_disc_insert = [](bool) {};
		cb.on_missing_fw = []() {};
		cb.handle_taskbar_progress = [](s32, s32) {};
		cb.get_localized_string = [](localized_string_id, const char*) -> std::string { return {}; };
		cb.get_localized_u32string = [](localized_string_id, const char*) -> std::u32string { return {}; };
		cb.get_localized_setting = [](const cfg::_base*, u32) -> std::string { return {}; };
		cb.get_photo_path = [](std::string_view) -> std::string { return {}; };
		cb.play_sound = [](const std::string&, std::optional<f32>) {};
		cb.get_image_info = [](const std::string&, std::string&, s32&, s32&, s32&) { return false; };
		cb.get_scaled_image = [](const std::string&, s32, s32, s32&, s32&, u8*, bool) { return false; };
		cb.get_font_dirs = []() { return std::vector<std::string>{}; };
		cb.on_install_pkgs = [](const std::vector<std::string>&, bool) { return false; };
		cb.add_breakpoint = [](u32) {};
		cb.display_sleep_control_supported = []() { return false; };
		cb.enable_display_sleep = [](bool) {};
		cb.check_microphone_permissions = []() {};
		cb.make_video_source = []() -> std::unique_ptr<video_source> { return nullptr; };
		cb.enable_gamemode = [](bool) {};
		cb.get_database_config = [](const std::string&) -> std::string { return {}; };
	}
}

int main(int argc, char** argv)
{
	g_main_id = std::this_thread::get_id();
	std::string ppu = "llvm", spu = "llvm";
	u32 timeout = 300;
	std::vector<std::string> elfs;
	for (int i = 1; i < argc; i++)
	{
		const std::string a = argv[i];
		if (a == "--ppu" && i + 1 < argc) ppu = argv[++i];
		else if (a == "--spu" && i + 1 < argc) spu = argv[++i];
		else if (a == "--timeout" && i + 1 < argc) timeout = std::atoi(argv[++i]);
		else elfs.push_back(a);
	}

	static std::unique_ptr<logs::listener> log_file = logs::make_file_listener(fs::get_log_dir() + "RPCS3.log", 256 * 1024 * 1024);
	logs::set_init({}); // or the logger keeps every line in memory
	create_callbacks();
	Emu.SetSupportedRenderers({video_renderer::null});
	Emu.SetHasGui(false);
	Emu.SetHeadless(true);
	Emu.SetUsr("00000001");
	Emu.Init();

	g_cfg.video.renderer.set(video_renderer::null);
	g_cfg.audio.renderer.set(audio_renderer::null);
	g_cfg.core.ppu_decoder.set(ppu == "interp" ? ppu_decoder_type::_static : ppu_decoder_type::llvm);
	g_cfg.core.spu_decoder.set(spu == "interp" ? spu_decoder_type::_static : spu == "asmjit" ? spu_decoder_type::asmjit : spu_decoder_type::llvm);
	// Without firmware every firmware library must use RPCS3's built-in (HLE) version.
	if (!fs::is_file(g_cfg_vfs.get_dev_flash() + "sys/external/liblv2.sprx"))
	{
		std::set<std::string> libs;
		for (const auto& [name, hle] : g_prx_list)
		{
			if (!hle)
				libs.emplace(std::string(name) + ":hle");
		}
		g_cfg.core.libraries_control.set_set(std::move(libs));
		std::fprintf(stderr, "no firmware: using HLE for all firmware libraries\n");
	}
	Emulator::SaveSettings(g_cfg.to_string(), {});

	const std::string tty_path = fs::get_log_dir() + "TTY.log";
	int failures = 0;
	for (const std::string& elf : elfs)
	{
		const u64 tty_start = fs::file(tty_path).size();
		std::printf("RUN %s\n", elf.c_str());
		std::fflush(stdout);
		const auto t0 = std::chrono::steady_clock::now();
		const game_boot_result res = Emu.BootGame(elf, "", true);
		bool timed_out = false;
		if (res == game_boot_result::no_errors)
		{
			while (!Emu.IsStopped(true))
			{
				run_queue();
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
				if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(timeout))
				{
					timed_out = true;
					Emu.GracefulShutdown(false, true);
				}
			}
			run_queue();
		}
		const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

		logs::listener::sync_all();
		fs::file tty(tty_path);
		std::string out;
		if (tty && tty.size() > tty_start)
		{
			out.resize(tty.size() - tty_start);
			out.resize(tty.read_at(tty_start, out.data(), out.size()));
		}
		const std::string output_txt = elf.substr(0, elf.find_last_of('/') + 1) + "output.txt";
		if (fs::file f{output_txt})
		{
			out += "--- output.txt ---\n" + f.to_string();
			f.close();
			fs::remove_file(output_txt);
		}
		if (!out.empty() && !out.ends_with('\n'))
			out += '\n';
		std::fwrite(out.data(), 1, out.size(), stdout);
		std::printf("END %s %s %.3fs\n", elf.c_str(), res != game_boot_result::no_errors ? fmt::format("%s", res).c_str() : timed_out ? "timeout" : "ok", secs);
		std::fflush(stdout);
		failures += res != game_boot_result::no_errors || timed_out;
	}

	logs::listener::sync_all();
	std::_Exit(failures);
}
