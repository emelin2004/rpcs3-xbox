#include "stdafx.h"
#include "uwp_patches.h"
#include "uwp_telemetry.h"
#include "uwp_update.h"
#include "Utilities/bin_patch.h"
#include "Utilities/File.h"
#include "Crypto/utils.h"
#include "util/logs.hpp"

#include <windows.h>
#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <sstream>

LOG_CHANNEL(ptc_log, "Patches");

namespace uwp_patches
{
	namespace
	{
		// The desktop Patch Manager's request (rpcs3qt/patch_manager_dialog.cpp): the newest patch.yml for this patch
		// engine version, or "up to date" (return_code 1) when the local file's SHA-256 is given and still current.
		constexpr const char* c_url = "https://rpcs3.net/compatibility?patch&api=v1&v=";
		constexpr usz c_max_answer = 64u << 20; // ~1.3 MB in 2026

		atomic_t<bool> g_downloading{false};

		usz curl_append(char* data, usz size, usz n, void* user)
		{
			std::string& out = *static_cast<std::string*>(user);
			if (out.size() + size * n > c_max_answer)
				return 0;
			out.append(data, size * n);
			return size * n;
		}

		std::string fetch(const std::string& url, long& http)
		{
			std::string answer;
			CURL* c = curl_easy_init();
			if (!c)
				return answer;
			const std::string agent = fmt::format("RPCS3Xbox/%s", uwp_update::app_version());
			const std::string_view roots = uwp_telemetry::tls_roots();
			curl_blob blob{const_cast<char*>(roots.data()), roots.size(), CURL_BLOB_NOCOPY};
			char error[CURL_ERROR_SIZE]{};
			curl_easy_setopt(c, CURLOPT_URL, url.c_str());
			curl_easy_setopt(c, CURLOPT_USERAGENT, agent.c_str());
			curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &blob);
			curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
			curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
			curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
			curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
			curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
			curl_easy_setopt(c, CURLOPT_ERRORBUFFER, error);
			curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_append);
			curl_easy_setopt(c, CURLOPT_WRITEDATA, &answer);
			const CURLcode res = curl_easy_perform(c);
			http = 0;
			if (res == CURLE_OK)
				curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
			else
				ptc_log.warning("Patch download failed: %s (%s)", curl_easy_strerror(res), error);
			curl_easy_cleanup(c);
			return answer;
		}

		// ---- The patch files, as the patch engine reads them at boot ----
		patch_engine::patch_map g_map;
		bool g_loaded = false;

		void load_all()
		{
			if (g_loaded)
				return;
			g_loaded = true;
			g_map.clear();
			// The order of the desktop Patch Manager: patch.yml, imported_patch.yml, then the per-game files.
			const std::string dir = patch_engine::get_patches_path();
			std::vector<std::string> names = {"patch.yml", "imported_patch.yml"};
			for (const auto& entry : fs::dir(dir))
			{
				if (!entry.is_directory && entry.name.ends_with("_patch.yml") && entry.name != "imported_patch.yml")
					names.push_back(entry.name);
			}
			for (const std::string& name : names)
			{
				if (!patch_engine::load(g_map, dir + name))
					ptc_log.error("Errors in patch file %s", name);
			}
			ptc_log.notice("Patch files read: %d hashes", g_map.size());
		}

		bool version_matches(const std::string& patch_version, const std::string& app_version)
		{
			return app_version.empty() || patch_version == patch_key::all || patch_version == app_version;
		}

		// Calls fn(config_values, description, info, patch_version) for every entry of `serial`.
		template <typename F>
		void for_serial(const std::string& serial, F&& fn)
		{
			for (auto& [hash, container] : g_map)
			{
				for (auto& [description, info] : container.patch_info_map)
				{
					for (auto& [title, serials] : info.titles)
					{
						const auto it = serials.find(serial);
						if (it == serials.end())
							continue;
						for (auto& [version, values] : it->second)
							fn(values, description, info, version);
					}
				}
			}
		}
	}

	namespace
	{
		struct download_args
		{
			std::function<void(result, const std::string&)> on_done;
		};

		std::pair<result, std::string> run_download()
		{
			const std::string path = patch_engine::get_patches_path() + "patch.yml";
			std::string url = c_url + patch_engine_version;
			if (fs::file f{path})
			{
				const std::string text = f.to_string();
				url += "&sha256=" + sha256_get_hash(text.c_str(), text.size(), true);
			}

			ptc_log.notice("Patch download: %s", url);
			long http = 0;
			const std::string answer = fetch(url, http);
			if (http != 200)
			{
				if (http)
					ptc_log.warning("Patch download: the server answered %d", http);
				return {result::failed, "The patch server could not be reached. Check the network and try again."};
			}

			int code = 0;
			std::string version, sha256, patch;
			if (!parse_answer(answer, code, version, sha256, patch))
			{
				ptc_log.error("Patch download: the answer is not valid JSON (%d bytes)", answer.size());
				return {result::failed, "The patch server sent an answer that could not be read."};
			}
			if (code == 1)
			{
				ptc_log.notice("Patch download: patch.yml is up to date");
				return {result::current, "The patches are up to date."};
			}
			if (code != 0)
			{
				ptc_log.error("Patch download: return code %d", code);
				return {result::failed, code == -2 ? "The patch server is under maintenance. Try again later." : "The patch server answered with an error."};
			}
			if (version != patch_engine_version || patch.empty() || sha256 != sha256_get_hash(patch.c_str(), patch.size(), true))
			{
				ptc_log.error("Patch download: wrong version (%s), no patch or checksum mismatch", version);
				return {result::failed, "The downloaded patches failed the checks and were not saved."};
			}

			patch_engine::patch_map check;
			std::stringstream messages;
			if (!patch_engine::load(check, "From Download", patch, true, &messages))
			{
				ptc_log.error("Patch download: errors in the downloaded patch.yml:\n%s", messages.str());
				return {result::failed, "The downloaded patches have errors and were not saved."};
			}

			const std::string dir = patch_engine::get_patches_path();
			if (!fs::create_path(dir))
			{
				ptc_log.error("Patch download: could not create %s (%s)", dir, fs::g_tls_error);
				return {result::failed, "The patches could not be saved."};
			}
			// Copied, not moved: a game booting meanwhile still finds a patch.yml.
			if (fs::is_file(path) && !fs::copy_file(path, path + ".old", true))
				ptc_log.warning("Patch download: could not back up patch.yml (%s)", fs::g_tls_error);
			fs::pending_file file(path);
			if (!file.file || !file.file.write(patch) || !file.commit())
			{
				ptc_log.error("Patch download: could not save %s (%s)", path, fs::g_tls_error);
				return {result::failed, "The patches could not be saved."};
			}
			ptc_log.success("Patch download: saved patch.yml (%d bytes, %d hashes)", patch.size(), check.size());
			return {result::updated, "The newest patches were downloaded."};
		}

		DWORD WINAPI download_thread(void* arg)
		{
			const std::unique_ptr<download_args> args(static_cast<download_args*>(arg));
			const auto [r, message] = run_download();
			g_downloading = false;
			if (args->on_done)
				args->on_done(r, message);
			return 0;
		}
	}

	bool download(std::function<void(result, const std::string&)> on_done)
	{
		if (g_downloading.exchange(true))
			return false;
		auto* args = new download_args{std::move(on_done)};
		// TLS and the YAML check of a ~1 MB file: more than the default stack.
		if (const HANDLE h = CreateThread(nullptr, 8 * 1024 * 1024, download_thread, args, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr))
		{
			CloseHandle(h);
			return true;
		}
		delete args;
		g_downloading = false;
		return false;
	}

	bool downloading()
	{
		return g_downloading;
	}

	game_patches list(const std::string& serial, const std::string& app_version)
	{
		load_all();
		game_patches out;
		std::map<std::pair<std::string, std::string>, patch_row> rows; // by description and version
		for_serial(serial, [&](const patch_engine::patch_config_values& values, const std::string& description, const patch_engine::patch_info& info, const std::string& version)
		{
			if (!version_matches(version, app_version))
			{
				if (std::find(out.other_versions.begin(), out.other_versions.end(), version) == out.other_versions.end())
					out.other_versions.push_back(version);
				return;
			}
			patch_row& row = rows[{description, version}];
			if (row.description.empty())
			{
				row.description = description;
				row.app_version = version;
				row.author = info.author;
				row.notes = info.notes;
				row.patch_version = info.patch_version;
				row.group = info.patch_group;
			}
			// One description can be split over several hashes (the game's PPU and SPU code): on when one is.
			row.enabled |= values.enabled;
			row.configurable |= !info.default_config_values.empty();
		});
		for (auto& [key, row] : rows)
			out.rows.push_back(std::move(row));
		std::sort(out.other_versions.begin(), out.other_versions.end());
		return out;
	}

	bool set_enabled(const std::string& serial, const std::string& app_version, const patch_row& row, bool on)
	{
		load_all();
		bool others = false;
		for_serial(serial, [&](patch_engine::patch_config_values& values, const std::string& description, const patch_engine::patch_info& info, const std::string& version)
		{
			if (description == row.description && version == row.app_version)
				values.enabled = on;
			else if (on && values.enabled && !row.group.empty() && info.patch_group == row.group && version_matches(version, app_version))
			{
				values.enabled = false;
				others = true;
			}
		});
		patch_engine::save_config(g_map);
		ptc_log.notice("Patch '%s' (%s, %s) %s%s", row.description, serial, row.app_version, on ? "on" : "off", others ? ", others of its group off" : "");
		return others;
	}

	void invalidate()
	{
		g_loaded = false;
		g_map.clear();
	}

	bool have_patch_file()
	{
		return fs::is_file(patch_engine::get_patches_path() + "patch.yml");
	}

	std::string patch_file_date()
	{
		fs::stat_t st{};
		if (!fs::get_stat(patch_engine::get_patches_path() + "patch.yml", st) || !st.mtime)
			return {};
		using namespace std::chrono;
		const year_month_day day{floor<days>(sys_seconds{seconds{st.mtime}})};
		return fmt::format("%04d-%02d-%02d", static_cast<int>(day.year()), static_cast<unsigned>(day.month()), static_cast<unsigned>(day.day()));
	}
}
