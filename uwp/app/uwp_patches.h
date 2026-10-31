#pragma once

// RPCS3's community game patches (as the desktop Patch Manager): patch.yml is downloaded from
// rpcs3.net (the same request the desktop app makes, which answers "up to date" when the local
// file's SHA-256 matches), checked by the core's patch engine and saved to
// <config>/patches/patch.yml. Which patches are on is kept in patch_config.yml, which the patch
// engine reads when a game boots, so a patch turned on here applies from the game's next start.
// The per-game settings (Menu on a game in the Games tab) list the patches of one game.

#include <functional>
#include <string>
#include <vector>

namespace uwp_patches
{
	enum class result
	{
		updated, // a newer patch.yml was saved
		current, // patch.yml is up to date
		failed,  // no answer, a server error, or a file that failed the checks (see the log)
	};

	// Starts a download on its own thread unless one runs; on_done runs on that thread.
	// `message` is a sentence for the user.
	bool download(std::function<void(result r, const std::string& message)> on_done);
	bool downloading();

	// ---- The patches of one game (UI thread; the patch files are read once and kept) ----
	struct patch_row
	{
		std::string description;
		std::string app_version; // the game version it is for, or "All"
		std::string author, notes, patch_version, group;
		bool enabled = false;
		bool configurable = false; // has adjustable values (their defaults apply)
	};

	struct game_patches
	{
		std::vector<patch_row> rows;
		std::vector<std::string> other_versions; // versions of this game that only other patches are for
	};

	// The patches for `serial`: the ones for `app_version` and for all versions (all of them when the version is unknown).
	game_patches list(const std::string& serial, const std::string& app_version);
	// Turns a row of list() on or off and saves patch_config.yml. Turning one on turns the others of its
	// patch group off (the patch engine applies one per group). True when other rows changed too.
	bool set_enabled(const std::string& serial, const std::string& app_version, const patch_row& row, bool on);

	// Reads the patch files again at the next list() (after a download).
	void invalidate();
	// patch.yml is there.
	bool have_patch_file();
	// When patch.yml was last downloaded, e.g. "2026-10-31", or empty.
	std::string patch_file_date();

	// For the tests: the top-level string and number fields of the server's JSON answer.
	bool parse_answer(const std::string& json, int& return_code, std::string& version, std::string& sha256, std::string& patch);
}
