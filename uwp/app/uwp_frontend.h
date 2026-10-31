#pragma once

// Controller-driven menu for the UWP shell: game list, PS3 firmware installer and a few
// settings, drawn in software into a BGRA canvas the shell shows while no game runs.

#include "util/types.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct IInspectable;

namespace uwp_frontend
{
	constexpr u32 canvas_width = 1920;
	constexpr u32 canvas_height = 1080;

	struct host_info
	{
		std::string local_dir;   // LocalState, with trailing slash
		std::string package_dir; // installed package folder, with trailing slash
		// Runs a function on the emulator ("main") thread, which owns Emu state changes.
		std::function<void(std::function<void()>)> run_on_main;
		// Set when no USB/external drive holds the PS3 hard drive: games and package installs
		// are refused with this text, since LocalState is far too small for them.
		std::string no_storage;
	};

	// Emulator thread, after Emu.Init() and the shell's fixed settings.
	void init(const host_info& host);

	// Emulator thread: boot a game folder, ISO or program the way the menu does
	// (picks built-in libraries when no firmware is installed). Returns false on failure.
	bool boot(const std::string& path);

	// UI thread, once per frame. Polls the gamepad (and keyboard through the CoreWindow).
	// Returns true while the menu is shown; then `pixels` is the canvas_width x canvas_height
	// BGRA canvas and `changed` says whether it differs from the previous frame.
	bool frame(IInspectable* core_window, const u32*& pixels, bool& changed);

	// UI thread, while a game boots without a frame to show: a loading screen with the
	// emulator's progress text (e.g. PPU modules compiling). Same canvas as frame().
	bool loading_frame(const std::string& title, const std::string& detail, u32 done, u32 total, const u32*& pixels);

	// Emulator callbacks.
	void on_emulation_started();
	void on_emulation_stopped();

	// ---- Model for the XAML shell (uwp_xaml.cpp) ----
	// The menu logic (scanning, installs, settings) above is shared with the software-drawn menu;
	// these functions give the XAML pages a thread-safe view of it. They never touch the canvas.

	// A decoded game icon (ICON0.PNG, 320x176 on the PS3): BGRA as 0xAARRGGBB, straight alpha.
	struct icon_data
	{
		u32 w = 0, h = 0;
		std::vector<u32> px;
	};

	struct game_view
	{
		std::string title, title_id, kind, version, source, path;
		std::shared_ptr<const icon_data> icon; // empty when the game has none
	};

	struct install_view
	{
		std::string title;
		std::string kind; // "Firmware", "Package" or "License"
		std::string path;
		u64 size = 0;
	};

	enum class status_kind : u32
	{
		dim,
		ok,
		warn,
		error,
	};

	struct snapshot
	{
		std::vector<game_view> games;
		std::vector<install_view> installs;
		std::string firmware; // installed firmware version, empty when none
		std::string status;
		status_kind status_color = status_kind::dim;
		std::string select_path; // game to select first (the last played one), empty: the top entry
		bool select_pending = false; // set until the list has been shown once; cleared by clear_select_pending()
		bool scanning = false;
		bool scanning_usb = false;
		std::string telemetry_id; // the anonymous install ID (uwp_telemetry)
		bool telemetry_on = false;
		std::string app_version;   // this package's version
		std::string update_latest; // the newest release on GitHub, empty: no answer (yet)
		bool update_checked = false; // GitHub answered: current or outdated
		bool update_outdated = false;
	};

	struct job_view
	{
		bool active = false;
		std::string title;
		u32 done = 0, total = 0;
		bool server = false; // an RPCN server call, not an install
	};

	struct dialog_view
	{
		std::string title, text;
		std::string accept_label; // empty: a single OK button
		std::function<void()> on_accept;
		std::string cancel_label; // empty: "Cancel"
		std::function<void()> on_cancel; // the cancel button or B
		bool refresh_settings = false;   // the answer changes a setting: rebuild the Settings page
	};

	struct setting_view
	{
		std::string label, note;
		std::vector<std::string> values; // what the setting is set to
		std::vector<std::string> shown;  // what the page shows for each value
		int current = 0;
		bool toggle = false;  // exactly false / true: shown as a switch
		bool in_game = false; // takes effect while a game runs (shown in the pause menu)
		bool pause_only = false; // not on the Settings tab
		std::string section;     // pause menu: the group heading
		bool action = false;  // no value list: a screen that open_setting() opens (RPCN account); `note` stays, `value` is shown
		std::string value;
	};

	// True once init() has run on the emulator thread (the settings need the emulator's configuration).
	bool model_ready();
	// Changes whenever the lists, status or scan state changed (job progress is polled with get_job()).
	u64 model_serial();
	void get_snapshot(snapshot& out);
	void clear_select_pending();
	job_view get_job();
	std::optional<dialog_view> take_pending_dialog();

	void request_rescan();
	// Boots a Games-tab entry (by path), remembering it as the last played game.
	void start_game(const std::string& path);
	// The confirmation dialog for an Install-tab entry (by path); licenses install at once and answer with a result.
	std::optional<dialog_view> install_prompt(const std::string& path);

	std::vector<setting_view> get_settings();
	void set_setting(u32 index, u32 value_index);
	// An action row (setting_view::action): opens its screen, which get_screen() then shows.
	void open_setting(u32 index);

	// ---- Settings of one game (Games tab, Menu button), UI thread, while no game runs ----
	// The pause menu's settings and a few boot-time ones, as the game boots with them. Changes are saved to the
	// game's custom config (custom_configs/config_<serial>.yml), which RPCS3 applies on top of config.yml.
	// False for entries without a serial (the XMB, loose programs).
	bool open_game_settings(const std::string& path);
	void close_game_settings();
	std::string game_settings_title();
	bool game_settings_own(); // the game has settings of its own (custom or built-in)
	bool game_settings_saved(); // ... in its custom config, which reset_game_settings() removes
	// With a Patches section: a row that downloads RPCS3's community patches (open_game_setting) and a switch per
	// patch of the game, saved to patch_config.yml (uwp_patches.h).
	std::vector<setting_view> get_game_settings();
	void set_game_setting(u32 index, u32 value_index);
	void open_game_setting(u32 index); // an action row of get_game_settings()
	// Changes when get_game_settings() would show something else (a download ended, a patch group switched others off).
	u64 game_settings_serial();
	// Removes the game's custom config: config.yml (and any built-in settings) apply again.
	void reset_game_settings();

	// ---- Screens the menu logic opens itself (the RPCN account screen and its text fields) ----
	// All UI thread.

	// Runs work that worker threads (RPCN server calls) handed to the UI thread. Every tick.
	void pump();

	// A list of actions with a status text above it.
	struct screen_view
	{
		std::string title, header;
		struct item
		{
			std::string label, detail;
		};
		std::vector<item> items;
	};

	// A text field that the menu logic asks for (one at a time, over the screen).
	struct text_view
	{
		u64 serial = 0; // changes for each new field
		std::string title, prompt, value;
		bool password = false;
		const char* input_scope = "Default"; // XAML InputScopeNameValue
		u32 max_len = 0;
	};

	std::optional<screen_view> get_screen();
	void screen_select(u32 index);
	void screen_close();
	std::optional<text_view> get_text_field();
	// The typed text: an error to show (and the field stays), or empty when it was taken.
	std::string text_field_done(const std::string& value);
	void text_field_cancel();

	bool in_game();
	// Render thread: true once when View + Menu has been held for a second while a game runs.
	bool poll_pause_combo();
	// Pauses or resumes the running game for the pause menu.
	void set_paused(bool paused);
	// The app is being suspended (Xbox Home) / resumed: pause the running game, and resume it only if
	// this paused it. Any thread; suspend waits up to 500 ms for the pause.
	void suspend_game();
	void resume_game();
	void stop_game();
	// Pause menu: stops the game and boots it again.
	void restart_game();

	// The rest of RPCS3's home menu, for the pause menu (UI thread). The lists open as screens (get_screen()).
	bool has_trophies(); // the running game has a trophy set
	void open_trophies();
	void open_friends(); // RPCN friends and requests (needs an account)
	void save_state();   // saves and goes on playing (or quits, with the Suspend setting)
	bool has_save_states();
	void open_load_state();
	void take_screenshot(); // the next frame, once the pause menu has closed
}
