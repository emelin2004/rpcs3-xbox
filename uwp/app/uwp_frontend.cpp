// Controller-driven menu for the RPCS3 UWP shell (no Qt, no XAML).
//
// Everything is drawn on the CPU into a 1920x1080 BGRA canvas with stb_truetype; the shell
// uploads it to its D3D11 swapchain the same way it shows the emulator's frames. Input comes
// from Windows.Gaming.Input (gamepad) and CoreWindow key states (keyboard, for PC testing).
//
// Tabs: Games (boot folders, ISOs, programs), Install (firmware PUP, PKG, RAP), Settings
// (with the RPCN account screen and its on-screen keyboard).
// While a game runs, holding View + Menu for a second stops it and returns here.

#include "stdafx.h"
#include "uwp_frontend.h"
#include "uwp_firmware.h"
#include "uwp_update.h"
#include "uwp_rpcn.h"
#include "uwp_telemetry.h"
#include "uwp_textinput.h"

#include "Emu/System.h"
#include "Emu/emu_callbacks.h"
#include "Emu/system_config.h"
#include "Emu/system_utils.hpp"
#include "Emu/vfs_config.h"
#include "Emu/NP/rpcn_config.h"
#include "Loader/ISO.h"
#include "Loader/PSF.h"
#include "Loader/TROPUSR.h"
#include "Emu/VFS.h"
#include "Emu/IdManager.h"
#include "Emu/savestate_utils.hpp"
#include "Emu/Cell/Modules/sceNpTrophy.h"
#include "Crypto/unpkg.h"
#include "Utilities/Thread.h"
#include "Utilities/File.h"
#include "util/sysinfo.hpp"
#include "util/cctype.hpp"
#include "util/yaml.hpp"

#include <roapi.h>
#include <winstring.h>
#include <inspectable.h>

#include <stb_truetype.h>
#include <stb_image.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

extern atomic_t<bool> g_user_asked_for_screenshot; // RSXThread.cpp

// Firmware library list (Emu/Cell/PPUModule.cpp): true = HLE by default.
extern const std::map<std::string_view, int> g_prx_list;

LOG_CHANNEL(ui_log, "UWPUI");

namespace uwp_frontend
{
	namespace
	{
		// ---- WinRT ABI declarations (Windows.Gaming.Input, CoreWindow key state) ----
		// Vtable order and IIDs follow the Windows SDK / Wine IDL; unused methods are placeholder slots.
#define UI_SLOT(n) virtual HRESULT STDMETHODCALLTYPE slot_##n() = 0

		struct gamepad_reading
		{
			UINT64 timestamp;
			UINT32 buttons;
			double left_trigger, right_trigger;
			double left_x, left_y, right_x, right_y;
		};

		struct IGamepad : IInspectable
		{
			UI_SLOT(get_Vibration);
			UI_SLOT(put_Vibration);
			virtual HRESULT STDMETHODCALLTYPE GetCurrentReading(gamepad_reading* value) = 0;
		};

		struct IGamepadVectorView : IInspectable // IVectorView<Gamepad>
		{
			virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32 index, IGamepad** item) = 0;
			virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32* size) = 0;
			UI_SLOT(IndexOf);
			UI_SLOT(GetMany);
		};

		struct IGamepadStatics : IInspectable
		{
			UI_SLOT(add_GamepadAdded);
			UI_SLOT(remove_GamepadAdded);
			UI_SLOT(add_GamepadRemoved);
			UI_SLOT(remove_GamepadRemoved);
			virtual HRESULT STDMETHODCALLTYPE get_Gamepads(IGamepadVectorView** value) = 0;
		};

		// ICoreWindow up to GetAsyncKeyState.
		struct ICoreWindowKeys : IInspectable
		{
			UI_SLOT(get_AutomationHostProvider);
			UI_SLOT(get_Bounds);
			UI_SLOT(get_CustomProperties);
			UI_SLOT(get_Dispatcher);
			UI_SLOT(get_FlowDirection);
			UI_SLOT(put_FlowDirection);
			UI_SLOT(get_IsInputEnabled);
			UI_SLOT(put_IsInputEnabled);
			UI_SLOT(get_PointerCursor);
			UI_SLOT(put_PointerCursor);
			UI_SLOT(get_PointerPosition);
			UI_SLOT(get_Visible);
			UI_SLOT(Activate);
			UI_SLOT(Close);
			virtual HRESULT STDMETHODCALLTYPE GetAsyncKeyState(int virtual_key, UINT32* state) = 0; // CoreVirtualKeyStates
		};
#undef UI_SLOT

		// {8bbce529-d49c-39e9-9560-e47dde96b7c8}
		constexpr GUID IID_IGamepadStatics{0x8bbce529, 0xd49c, 0x39e9, {0x95, 0x60, 0xe4, 0x7d, 0xde, 0x96, 0xb7, 0xc8}};

		enum gamepad_button : u32
		{
			gp_menu = 0x1,
			gp_view = 0x2,
			gp_a = 0x4,
			gp_b = 0x8,
			gp_x = 0x10,
			gp_y = 0x20,
			gp_up = 0x40,
			gp_down = 0x80,
			gp_left = 0x100,
			gp_right = 0x200,
			gp_lb = 0x400,
			gp_rb = 0x800,
		};

		// Menu actions; bits so that held state can be tracked like buttons.
		enum action : u32
		{
			act_up = 1 << 0,
			act_down = 1 << 1,
			act_left = 1 << 2,
			act_right = 1 << 3,
			act_accept = 1 << 4,
			act_back = 1 << 5,
			act_refresh = 1 << 6,
			act_prev_tab = 1 << 7,
			act_next_tab = 1 << 8,
			act_page_up = 1 << 9,
			act_page_down = 1 << 10,
			act_x = 1 << 11,
			act_start = 1 << 12,
			act_view = 1 << 13,
		};

		// ---- Colors (BGRA in a u32: 0xAARRGGBB) ----
		constexpr u32 col_bg = 0xff15161b;
		constexpr u32 col_header = 0xff1e2028;
		constexpr u32 col_panel = 0xff1b1d24;
		constexpr u32 col_sel = 0xff2b5fae;
		constexpr u32 col_text = 0xffeceef2;
		constexpr u32 col_dim = 0xff9aa0ab;
		constexpr u32 col_accent = 0xff4d8fe8;
		constexpr u32 col_ok = 0xff4cc27a;
		constexpr u32 col_warn = 0xffe8b04d;
		constexpr u32 col_err = 0xffe85d5d;
		constexpr u32 col_shade = 0xc0000000;

		// TV-safe margins.
		constexpr int margin_x = 110;
		constexpr int margin_y = 60;

		// ---- Fonts ----
		struct glyph
		{
			int w = 0, h = 0, xoff = 0, yoff = 0;
			float advance = 0;
			std::vector<u8> bitmap;
		};

		struct font_face
		{
			std::vector<u8> data;
			stbtt_fontinfo info{};
		};

		class font_set
		{
			std::vector<std::unique_ptr<font_face>> m_faces;
			std::unordered_map<u64, glyph> m_cache;

		public:
			void clear()
			{
				m_faces.clear();
				m_cache.clear();
			}

			bool add(const std::string& path)
			{
				fs::file f(path);
				if (!f)
					return false;
				auto face = std::make_unique<font_face>();
				face->data = f.to_vector<u8>();
				if (!stbtt_InitFont(&face->info, face->data.data(), stbtt_GetFontOffsetForIndex(face->data.data(), 0)))
					return false;
				ui_log.notice("UI font: %s", path);
				m_faces.push_back(std::move(face));
				return true;
			}

			bool empty() const { return m_faces.empty(); }

			const glyph& get(char32_t cp, int px)
			{
				const u64 key = (u64{static_cast<u32>(px)} << 32) | cp;
				if (auto it = m_cache.find(key); it != m_cache.end())
					return it->second;

				glyph& g = m_cache[key];
				if (m_faces.empty())
					return g;

				// First face that has the glyph, else the primary face's "missing" box.
				font_face* face = m_faces.front().get();
				int index = 0;
				for (auto& f : m_faces)
				{
					if (const int i = stbtt_FindGlyphIndex(&f->info, cp))
					{
						face = f.get();
						index = i;
						break;
					}
				}

				const float scale = stbtt_ScaleForPixelHeight(&face->info, static_cast<float>(px));
				int adv = 0, lsb = 0;
				stbtt_GetGlyphHMetrics(&face->info, index, &adv, &lsb);
				g.advance = adv * scale;
				int x0, y0, x1, y1;
				stbtt_GetGlyphBitmapBox(&face->info, index, scale, scale, &x0, &y0, &x1, &y1);
				g.w = std::max(0, x1 - x0);
				g.h = std::max(0, y1 - y0);
				g.xoff = x0;
				g.yoff = y0;
				g.bitmap.resize(usz{static_cast<u32>(g.w)} * g.h);
				if (g.w && g.h)
					stbtt_MakeGlyphBitmap(&face->info, g.bitmap.data(), g.w, g.h, g.w, scale, scale, index);
				return g;
			}
		};

		std::u32string utf8_to_u32(std::string_view s)
		{
			std::u32string out;
			for (usz i = 0; i < s.size();)
			{
				const u8 c = s[i];
				char32_t cp = '?';
				usz n = 1;
				if (c < 0x80)
					cp = c;
				else if ((c >> 5) == 6 && i + 1 < s.size())
					cp = ((c & 0x1f) << 6) | (s[i + 1] & 0x3f), n = 2;
				else if ((c >> 4) == 14 && i + 2 < s.size())
					cp = ((c & 0x0f) << 12) | ((s[i + 1] & 0x3f) << 6) | (s[i + 2] & 0x3f), n = 3;
				else if ((c >> 3) == 30 && i + 3 < s.size())
					cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3f) << 12) | ((s[i + 2] & 0x3f) << 6) | (s[i + 3] & 0x3f), n = 4;
				out.push_back(cp);
				i += n;
			}
			return out;
		}

		// ---- Canvas ----
		// A decoded game icon (ICON0.PNG, 320x176 on the PS3), BGRA with straight alpha.
		using icon_image = icon_data;

		std::shared_ptr<icon_image> decode_icon(const std::vector<u8>& png)
		{
			if (png.empty())
				return nullptr;
			int w = 0, h = 0, comp = 0;
			stbi_uc* rgba = stbi_load_from_memory(png.data(), ::narrow<int>(png.size()), &w, &h, &comp, 4);
			if (!rgba)
				return nullptr;
			auto img = std::make_shared<icon_image>();
			if (w > 0 && h > 0 && w <= 2048 && h <= 2048)
			{
				img->w = w;
				img->h = h;
				img->px.resize(usz{img->w} * img->h);
				for (usz i = 0; i < img->px.size(); i++)
				{
					const stbi_uc* p = rgba + i * 4;
					img->px[i] = (u32{p[3]} << 24) | (u32{p[0]} << 16) | (u32{p[1]} << 8) | p[2];
				}
			}
			stbi_image_free(rgba);
			return img->px.empty() ? nullptr : img;
		}

		class canvas
		{
		public:
			std::vector<u32> px = std::vector<u32>(usz{canvas_width} * canvas_height);
			font_set fonts;

			void fill(int x, int y, int w, int h, u32 c)
			{
				const int x0 = std::max(x, 0), y0 = std::max(y, 0);
				const int x1 = std::min<int>(x + w, canvas_width), y1 = std::min<int>(y + h, canvas_height);
				const u32 a = c >> 24;
				for (int yy = y0; yy < y1; yy++)
				{
					u32* row = px.data() + usz{static_cast<u32>(yy)} * canvas_width;
					if (a == 0xff)
						std::fill(row + x0, row + std::max(x0, x1), c);
					else
						for (int xx = x0; xx < x1; xx++)
							row[xx] = blend(row[xx], c, a);
				}
			}

			// Bilinear-scaled, alpha-blended copy of an image into (x, y, w, h).
			void image(const icon_image& img, int x, int y, int w, int h)
			{
				if (!img.w || !img.h || w <= 0 || h <= 0)
					return;
				for (int j = std::max(0, -y); j < h && y + j < static_cast<int>(canvas_height); j++)
				{
					const float sy = std::clamp((j + 0.5f) * img.h / h - 0.5f, 0.f, img.h - 1.f);
					const u32 y0 = static_cast<u32>(sy), y1 = std::min(y0 + 1, img.h - 1);
					const float fy = sy - y0;
					u32* row = px.data() + usz{static_cast<u32>(y + j)} * canvas_width;
					for (int i = std::max(0, -x); i < w && x + i < static_cast<int>(canvas_width); i++)
					{
						const float sx = std::clamp((i + 0.5f) * img.w / w - 0.5f, 0.f, img.w - 1.f);
						const u32 x0 = static_cast<u32>(sx), x1 = std::min(x0 + 1, img.w - 1);
						const float fx = sx - x0;
						const u32 c00 = img.px[usz{y0} * img.w + x0], c10 = img.px[usz{y0} * img.w + x1];
						const u32 c01 = img.px[usz{y1} * img.w + x0], c11 = img.px[usz{y1} * img.w + x1];
						u32 out = 0;
						for (u32 sh = 0; sh < 32; sh += 8)
						{
							const float top = ((c00 >> sh) & 0xff) * (1 - fx) + ((c10 >> sh) & 0xff) * fx;
							const float bot = ((c01 >> sh) & 0xff) * (1 - fx) + ((c11 >> sh) & 0xff) * fx;
							out |= static_cast<u32>(top * (1 - fy) + bot * fy + 0.5f) << sh;
						}
						const u32 a = out >> 24;
						if (a)
							row[x + i] = a == 0xff ? out : blend(row[x + i], out, a);
					}
				}
			}

			// Outline of a rectangle, `t` pixels thick.
			void frame_rect(int x, int y, int w, int h, int t, u32 c)
			{
				fill(x, y, w, t, c);
				fill(x, y + h - t, w, t, c);
				fill(x, y, t, h, c);
				fill(x + w - t, y, t, h, c);
			}

			static u32 blend(u32 dst, u32 src, u32 a)
			{
				const u32 ia = 255 - a;
				const u32 r = (((src >> 16) & 0xff) * a + ((dst >> 16) & 0xff) * ia) / 255;
				const u32 g = (((src >> 8) & 0xff) * a + ((dst >> 8) & 0xff) * ia) / 255;
				const u32 b = ((src & 0xff) * a + (dst & 0xff) * ia) / 255;
				return 0xff000000 | (r << 16) | (g << 8) | b;
			}

			int text_width(std::string_view s, int size)
			{
				float w = 0;
				for (char32_t cp : utf8_to_u32(s))
					w += fonts.get(cp, size).advance;
				return static_cast<int>(std::ceil(w));
			}

			// Draws text with its top edge at y (line height ~ 1.25 * size). Clips to max_w with "...".
			int text(int x, int y, std::string_view s, int size, u32 color, int max_w = 0)
			{
				std::u32string str = utf8_to_u32(s);
				if (max_w > 0 && text_width(s, size) > max_w)
				{
					const float ell = fonts.get('.', size).advance * 3;
					float w = 0;
					usz n = 0;
					for (; n < str.size(); n++)
					{
						const float a = fonts.get(str[n], size).advance;
						if (w + a + ell > max_w)
							break;
						w += a;
					}
					str.resize(n);
					str += U"...";
				}

				const int baseline = y + size * 95 / 100;
				float pen = static_cast<float>(x);
				for (char32_t cp : str)
				{
					const glyph& g = fonts.get(cp, size);
					const int gx = static_cast<int>(pen) + g.xoff;
					const int gy = baseline + g.yoff;
					for (int j = 0; j < g.h; j++)
					{
						const int yy = gy + j;
						if (yy < 0 || yy >= static_cast<int>(canvas_height))
							continue;
						u32* row = px.data() + usz{static_cast<u32>(yy)} * canvas_width;
						for (int i = 0; i < g.w; i++)
						{
							const int xx = gx + i;
							const u8 cov = g.bitmap[usz{static_cast<u32>(j)} * g.w + i];
							if (cov && xx >= 0 && xx < static_cast<int>(canvas_width))
								row[xx] = blend(row[xx], color, cov * (color >> 24) / 255);
						}
					}
					pen += g.advance;
				}
				return static_cast<int>(pen) - x;
			}

			// Word-wrapped text; returns the height used.
			int paragraph(int x, int y, std::string_view s, int size, u32 color, int max_w)
			{
				const int line_h = size * 135 / 100;
				int cy = y;
				usz start = 0;
				while (start <= s.size())
				{
					const usz nl = s.find('\n', start);
					std::string_view line = s.substr(start, nl == umax ? umax : nl - start);
					// Greedy wrap on spaces.
					while (!line.empty())
					{
						usz cut = line.size();
						while (text_width(line.substr(0, cut), size) > max_w)
						{
							const usz sp = line.rfind(' ', cut - 1);
							if (sp == umax || sp == 0)
								break;
							cut = sp;
						}
						text(x, cy, line.substr(0, cut), size, color, max_w);
						cy += line_h;
						line.remove_prefix(std::min(line.size(), cut + 1));
					}
					if (nl == umax)
						break;
					if (s.substr(start, nl - start).empty())
						cy += line_h;
					start = nl + 1;
				}
				return cy - y;
			}
		};

		// ---- Menu data ----
		struct game_entry
		{
			std::string title;
			std::string detail; // title ID, source
			std::string path;
			std::shared_ptr<icon_image> icon; // ICON0.PNG, when the game has one
			std::string title_id;
			std::string version; // APP_VER
			std::string kind;    // "Disc game", "HDD game", "ISO", "Program", ...
			std::string source;  // where it was found
		};

		// Something the Install tab can install.
		struct install_entry
		{
			enum class kind
			{
				firmware, // PS3UPDAT.PUP
				package,  // .pkg (games, updates, DLC)
				license,  // .rap (license for a PSN package)
			} type;
			std::string path;
			u64 size = 0;
			std::string title; // from the package header, when it has one
		};

		enum class tab : u32
		{
			games,
			install,
			settings,
			count
		};

		struct setting_item
		{
			std::string label;
			cfg::_base* node; // null: a screen that A opens (on_open), showing value()
			std::vector<std::string> values; // empty: the node's own list (enums, bools)
			std::string note;
			std::function<void()> on_open;
			std::function<std::string()> value;
			std::function<void()> on_change; // set: called instead of saving config.yml
			// XAML shell only: shown in the in-game pause menu (under `section`) instead of the Settings tab.
			bool pause = false;
			std::string section;
			std::vector<std::string> shown; // what the list shows for each of `values` (empty: the values)
			// In a game: saved to config.yml but not applied until the game boots again (the live switch breaks Dozen).
			bool on_restart = false;
			std::string pending; // the saved value the running game doesn't use yet
		};

		struct modal
		{
			std::string title;
			std::string text;
			std::string accept_label; // empty: OK only
			std::function<void()> on_accept;
			std::string cancel_label; // empty: "Cancel" (or "OK" when there is no accept button)
			std::function<void()> on_cancel; // also B / Back
			bool refresh_settings = false;   // the answer changes a setting shown in Settings
		};

		// A text field (RPCN account details): typed on the system (Xbox) keyboard, or on the
		// menu's own keys when the system keyboard can't be shown.
		struct text_entry
		{
			enum class kind
			{
				username, // RPCN username: letters, digits, - and _
				password, // shown as stars unless View is pressed
				email,
				token,    // 16 characters, digits and capital letters
				host,     // server address (host or host:port)
			} type;
			std::string title;
			std::string prompt;
			std::string value;
			usz max_len;
			bool allow_empty = false; // Done with nothing typed (e.g. "enter the token later")
			std::function<void(std::string)> on_done;
			int row = 1, col = 0;
			int layer = 0; // 0 lower case, 1 upper case, 2 symbols
			bool reveal = false;
			bool system_keyboard = false; // the Xbox keyboard types into it; else the menu's own keys
			u64 serial = 0; // tells the XAML shell that a new field replaced the previous one
		};

		// A list of actions with a status text above it (the RPCN account screen).
		struct menu_item
		{
			std::string label;
			std::string detail;
			std::function<void()> on_select;
		};

		struct list_menu
		{
			std::string title;
			std::function<std::string()> header;
			std::function<std::vector<menu_item>()> build;
			int sel = 0;
		};

		// ---- State ----
		host_info g_host;
		atomic_t<bool> g_ready{false};
		atomic_t<bool> g_in_game{false};
		atomic_t<bool> g_paused_by_menu{false}; // the pause menu paused the emulator
		atomic_t<bool> g_paused_by_suspend{false}; // suspend_game() paused the emulator

		std::mutex g_data_mutex; // games, pups, messages written by worker threads
		std::vector<game_entry> g_games;
		std::vector<install_entry> g_installs;
		std::string g_fw_version;
		std::string g_status_text;
		u32 g_status_color = col_dim;
		atomic_t<u64> g_data_serial{1};
		atomic_t<bool> g_scanning{false};
		atomic_t<bool> g_scanning_usb{false}; // local entries are listed, USB drives still being read

		std::unique_ptr<modal> g_pending_modal; // from worker threads, guarded by g_data_mutex

		// Games-tab entry to select once it is listed (the last played game), guarded by g_data_mutex.
		// Empty with g_select_pending set: select the top once the scan is done.
		std::string g_select_path;
		bool g_select_pending = true;

		std::string last_game_file()
		{
			return g_host.local_dir + "rpcs3/last_game.txt";
		}

		// Running install job (firmware or package); done/total drive the progress bar.
		std::unique_ptr<uwp_firmware::progress> g_job;
		std::string g_job_title; // set on the UI thread before the job starts
		atomic_t<bool> g_installing{false};

		// UI-thread-only state.
		struct ui_state
		{
			canvas cv;
			bool fonts_loaded = false;
			bool fonts_need_reload = false;
			tab current = tab::games;
			int sel[3]{};
			int scroll[3]{};
			std::unique_ptr<modal> dialog;
			std::unique_ptr<text_entry> osk;
			std::unique_ptr<list_menu> menu;
			uwp_rpcn::account rpcn_account; // rpcn.yml as last read (refresh_rpcn_account)
			std::vector<setting_item> settings;
			u64 drawn_serial = 0;
			bool dirty = true;
			u32 held = 0;
			std::chrono::steady_clock::time_point repeat_at{};
			std::chrono::steady_clock::time_point combo_since{};
			bool combo_fired = false;
			IGamepadStatics* gamepads = nullptr;
			bool gamepads_tried = false;
			u32 last_job_done = umax;
		};

		ui_state* g_ui = nullptr;
		bool g_xaml_ui = false; // the XAML shell drives the menu logic (pump() has run); UI thread

		void set_status(std::string text, u32 color)
		{
			std::lock_guard lock(g_data_mutex);
			g_status_text = std::move(text);
			g_status_color = color;
			g_data_serial++;
		}

		// The telemetry question, once, on the first start (before other messages). g_data_mutex held.
		std::unique_ptr<modal> take_consent_prompt()
		{
			static bool asked = false;
			if (asked || !g_ready || uwp_telemetry::get_consent() != uwp_telemetry::consent::unknown)
				return nullptr;
			asked = true;
			const auto answer = [](bool yes)
			{
				uwp_telemetry::set_consent(yes);
				g_data_serial++;
			};
			auto m = std::make_unique<modal>(modal{uwp_telemetry::consent_title(), uwp_telemetry::consent_text(), "Yes, send logs", [answer]() { answer(true); }});
			m->cancel_label = "No";
			m->on_cancel = [answer]() { answer(false); };
			m->refresh_settings = true;
			return m;
		}

		void post_modal(std::string title, std::string text)
		{
			std::lock_guard lock(g_data_mutex);
			g_pending_modal = std::make_unique<modal>(modal{std::move(title), std::move(text), {}, {}});
			g_data_serial++;
		}

		// Work for the UI thread from worker threads (run by frame()), guarded by g_data_mutex.
		std::vector<std::function<void()>> g_pending_ui;

		void post_ui(std::function<void()> fn)
		{
			std::lock_guard lock(g_data_mutex);
			g_pending_ui.push_back(std::move(fn));
			g_data_serial++;
		}

		// RPCN server call in progress (worker thread); the menu waits for it.
		atomic_t<bool> g_rpcn_busy{false};
		std::string g_rpcn_busy_text; // set on the UI thread before the job starts

		// ---- Scanning ----
		bool is_program_file(const std::string& name)
		{
			std::string ext = name.substr(name.rfind('.') == umax ? name.size() : name.rfind('.'));
			std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
			return ext == ".elf" || ext == ".self" || ext == ".bin" || ext == ".iso";
		}

		std::string upper_ext(const std::string& name)
		{
			const usz dot = name.rfind('.');
			std::string ext = dot == umax ? std::string() : name.substr(dot);
			std::transform(ext.begin(), ext.end(), ext.begin(), ::toupper);
			return ext;
		}

		// A PS3 disc image: title from PS3_GAME/PARAM.SFO inside it. Encrypted (redump) images need
		// their .dkey next to them, 3k3y images work as they are.
		bool add_iso(std::vector<game_entry>& out, const std::string& path, std::string_view source)
		{
			if (!is_iso_file(path))
				return false;

			std::string title, title_id = "?", version;
			std::shared_ptr<icon_image> icon;
			iso_archive archive(path);
			if (archive.is_valid())
			{
				const psf::registry sfo = archive.open_psf("PS3_GAME/PARAM.SFO");
				title = std::string(psf::get_string(sfo, "TITLE"));
				title_id = std::string(psf::get_string(sfo, "TITLE_ID", "?"));
				version = std::string(psf::get_string(sfo, "APP_VER"));
				if (archive.is_file("PS3_GAME/ICON0.PNG"))
				{
					if (fs::file f{archive.open("PS3_GAME/ICON0.PNG")})
						icon = decode_icon(f.to_vector<u8>());
				}
			}
			const std::string name = path.substr(path.rfind('/') + 1);
			if (title.empty())
				title = name;
			std::replace(title.begin(), title.end(), '\n', ' ');
			out.push_back({title, fmt::format("%s  ·  ISO  ·  %s", title_id, source), path, std::move(icon), title_id, version, "Disc image (ISO)", std::string(source)});
			return true;
		}

		// Firmware, packages and licenses directly inside `dir`.
		void scan_install_dir(std::vector<install_entry>& out, const std::string& dir)
		{
			for (const auto& e : fs::dir(dir))
			{
				if (e.is_directory)
					continue;
				const std::string ext = upper_ext(e.name);
				const std::string path = dir + e.name;
				if (ext == ".PUP")
				{
					out.push_back({install_entry::kind::firmware, path, e.size, "PS3 system software (firmware)"});
				}
				else if (ext == ".PKG")
				{
					std::string title;
					package_reader reader(path);
					if (reader.is_valid())
					{
						title = std::string(psf::get_string(reader.get_psf(), "TITLE"));
						if (const std::string_view id = psf::get_string(reader.get_psf(), "TITLE_ID"); !id.empty())
							title = fmt::format("%s  (%s)", title.empty() ? e.name : title, id);
						std::replace(title.begin(), title.end(), '\n', ' ');
					}
					out.push_back({install_entry::kind::package, path, e.size, title.empty() ? e.name : title});
				}
				else if (ext == ".RAP")
				{
					out.push_back({install_entry::kind::license, path, e.size, "License " + e.name});
				}
			}
		}

		// A folder that RPCS3 can boot: disc layout (PS3_GAME/) or HDD layout (PARAM.SFO + USRDIR/EBOOT.BIN).
		bool add_game_folder(std::vector<game_entry>& out, const std::string& dir, std::string_view source)
		{
			std::string sfo_path;
			if (fs::is_file(dir + "/PS3_GAME/PARAM.SFO"))
				sfo_path = dir + "/PS3_GAME/PARAM.SFO";
			else if (fs::is_file(dir + "/PARAM.SFO") && fs::is_file(dir + "/USRDIR/EBOOT.BIN"))
				sfo_path = dir + "/PARAM.SFO";
			else
				return false;

			const psf::registry sfo = psf::load_object(sfo_path);
			const std::string category{psf::get_string(sfo, "CATEGORY")};
			// Patches, game data and system content can't be booted on their own.
			if (!category.empty() && category != "HG" && category != "DG" && category != "AP" && category != "AM" && category != "AV" && category != "CB" && category != "AT" && category != "HM")
				return false;

			std::string title{psf::get_string(sfo, "TITLE")};
			if (title.empty())
				title = dir.substr(dir.rfind('/') + 1);
			std::replace(title.begin(), title.end(), '\n', ' ');

			// ICON0.PNG sits next to PARAM.SFO.
			std::shared_ptr<icon_image> icon;
			if (fs::file f{sfo_path.substr(0, sfo_path.rfind('/') + 1) + "ICON0.PNG"})
				icon = decode_icon(f.to_vector<u8>());

			const std::string title_id{psf::get_string(sfo, "TITLE_ID", "?")};
			const bool disc = sfo_path.ends_with("/PS3_GAME/PARAM.SFO");
			out.push_back({title, fmt::format("%s  ·  %s", title_id, source), dir, std::move(icon), title_id,
				std::string(psf::get_string(sfo, "APP_VER")), disc ? "Disc game (folder)" : "HDD game", std::string(source)});
			return true;
		}

		// Publishes a scan result: sorted games (XMB on top), then loose and bundled programs.
		void publish_scan(std::vector<game_entry> games, const std::vector<game_entry>& programs, std::vector<install_entry> installs, bool final_pass)
		{
			std::sort(games.begin(), games.end(), [](const game_entry& a, const game_entry& b) { return a.title < b.title; });

			// The PS3 system menu (XMB) once firmware is installed, as desktop RPCS3's "Boot VSH".
			if (const std::string vsh = g_cfg_vfs.get_dev_flash() + "vsh/module/vsh.self"; fs::is_file(vsh))
				games.insert(games.begin(), {"PS3 System Menu (XMB)", "from the installed firmware", vsh, nullptr, {}, utils::get_firmware_version(), "System software", "installed firmware"});

			games.insert(games.end(), programs.begin(), programs.end());

			// Firmware first, then packages, then licenses.
			std::stable_sort(installs.begin(), installs.end(), [](const install_entry& a, const install_entry& b) { return a.type < b.type; });

			const std::string fw = utils::get_firmware_version();
			ui_log.notice("UI: %u games/programs, %u installable files, installed firmware '%s'%s", games.size(), installs.size(), fw, final_pass ? "" : " (USB drives not scanned yet)");

			std::lock_guard lock(g_data_mutex);
			g_games = std::move(games);
			g_installs = std::move(installs);
			g_fw_version = fw;
			if (final_pass)
			{
				g_scanning = false;
				g_scanning_usb = false;
			}
			g_data_serial++;
		}

		void scan_worker()
		{
			// Disk scans over brokered FromApp file APIs; keep them behind a running game.
			thread_ctrl::scoped_priority low_prio(-1);

			std::vector<game_entry> games;
			std::vector<game_entry> programs;
			std::vector<install_entry> installs;

			const std::string games_dir = g_host.local_dir + "games/";
			const std::string boot_dir = g_host.local_dir + "boot/";
			const std::string fw_dir = g_host.local_dir + "firmware/";
			const std::string pkg_dir = g_host.local_dir + "packages/";
			fs::create_path(games_dir);
			fs::create_path(boot_dir);
			fs::create_path(fw_dir);
			fs::create_path(pkg_dir);

			// Pass 1, local only (fast): game folders, ISOs and programs in LocalState/games,
			// games installed into the emulated HDD, LocalState/boot, the bundled test programs,
			// and firmware/packages/licenses in LocalState, LocalState/firmware and LocalState/packages.
			for (const auto& e : fs::dir(games_dir))
			{
				if (e.name == "." || e.name == "..")
					continue;
				if (e.is_directory)
					add_game_folder(games, games_dir + e.name, "games folder");
				else if (upper_ext(e.name) == ".ISO")
					add_iso(games, games_dir + e.name, "games folder");
				else if (is_program_file(e.name))
					programs.push_back({e.name, "program  ·  games folder", games_dir + e.name, nullptr, {}, {}, "Program", "games folder"});
			}

			const std::string hdd_game = rpcs3::utils::get_hdd0_dir() + "game/";
			for (const auto& e : fs::dir(hdd_game))
			{
				if (e.is_directory && e.name != "." && e.name != "..")
					add_game_folder(games, hdd_game + e.name, "installed");
			}

			for (const auto& e : fs::dir(boot_dir))
			{
				if (!e.is_directory && is_program_file(e.name))
					programs.push_back({e.name, "program  ·  boot folder", boot_dir + e.name, nullptr, {}, {}, "Program", "boot folder"});
			}
			for (const auto& e : fs::dir(g_host.package_dir + "test/"))
			{
				if (!e.is_directory && is_program_file(e.name))
					programs.push_back({e.name, "bundled test program", g_host.package_dir + "test/" + e.name, nullptr, {}, {}, "Test program", "bundled with the app"});
			}

			for (const std::string& dir : {g_host.local_dir, fw_dir, pkg_dir})
				scan_install_dir(installs, dir);

			g_scanning_usb = true;
			publish_scan(games, programs, installs, false);

			// Pass 2, USB drives (slow: brokered *FromApp file APIs, see Utilities/File.cpp):
			// game folders and ISOs in GAMES/ and PS3ISO/ (the multiMAN/webMAN layout) or the root,
			// firmware, packages and licenses in the root, PS3/UPDATE/, PKG/, packages/ and exdata/.
			for (char d = 'D'; d <= 'Z'; d++)
			{
				const std::string root = fmt::format("%c:/", d);
				if (!fs::is_dir(root))
					continue;
				ui_log.notice("UI: drive %s is accessible", root);
				const std::string drive = fmt::format("USB %c:", d);

				for (const std::string& dir : {root + "GAMES/", root + "PS3ISO/", root})
				{
					for (const auto& e : fs::dir(dir))
					{
						if (e.name == "." || e.name == "..")
							continue;
						if (e.is_directory && dir != root)
							add_game_folder(games, dir + e.name, drive);
						else if (!e.is_directory && upper_ext(e.name) == ".ISO")
							add_iso(games, dir + e.name, drive);
					}
				}
				for (const std::string& dir : {root, root + "PS3/UPDATE/", root + "PKG/", root + "packages/", root + "exdata/"})
					scan_install_dir(installs, dir);
			}

			publish_scan(std::move(games), programs, std::move(installs), true);
		}

		void rescan()
		{
			if (g_scanning.exchange(true))
				return;
			std::thread(scan_worker).detach();
		}

		bool firmware_installed()
		{
			return fs::is_file(g_cfg_vfs.get_dev_flash() + "sys/external/liblv2.sprx");
		}

		void save_settings()
		{
			g_host.run_on_main([]()
			{
				Emulator::SaveSettings(g_cfg.to_string(), {});
			});
		}

		// The names from the config root down to `node`, e.g. {"Core", "SPU Block Size"}: a path that is the same
		// in every cfg_root and in the YAML of config.yml and the custom configs.
		std::vector<std::string> node_path(const cfg::_base* node)
		{
			std::vector<std::string> names;
			for (const cfg::_base* n = node; n && n->get_parent(); n = n->get_parent())
				names.push_back(n->get_name());
			std::reverse(names.begin(), names.end());
			return names;
		}

		cfg::_base* find_node(cfg::_base* root, const std::vector<std::string>& names)
		{
			cfg::_base* target = root;
			for (const std::string& name : names)
			{
				if (!target || target->get_type() != cfg::type::node)
					return nullptr;
				cfg::_base* next = nullptr;
				for (cfg::_base* child : static_cast<cfg::node*>(target)->get_nodes())
				{
					if (child->get_name() == name)
						next = child;
				}
				target = next;
			}
			return target == root ? nullptr : target;
		}

		std::string global_config_path()
		{
			return fs::get_config_dir(true) + "config.yml";
		}

		bool write_config_file(const std::string& path, const std::string& text)
		{
			fs::pending_file temp(path);
			if (!temp.file || (temp.file.write(text.data(), text.size()), !temp.commit()))
			{
				ui_log.error("UI: could not save %s (%s)", path, fs::g_tls_error);
				return false;
			}
			return true;
		}

		// ---- A game's own settings (custom_configs/config_<serial>.yml) as YAML: only the keys set for the game ----
		YAML::Node load_yaml_map(const std::string& text)
		{
			auto [doc, error] = yaml_load(text);
			if (!error.empty() || !doc.IsMap())
			{
				if (!error.empty())
					ui_log.error("UI: could not read a game config: %s", error);
				return YAML::Node(YAML::NodeType::Map);
			}
			return doc;
		}

		std::string yaml_text(const YAML::Node& doc)
		{
			YAML::Emitter out;
			out << doc;
			return std::string(out.c_str(), out.size());
		}

		// Only reads: YAML::Node's operator= changes the node it refers to, so no node is reassigned here.
		bool yaml_has(const YAML::Node& node, const std::vector<std::string>& names, usz i = 0)
		{
			if (i >= names.size() || !node.IsMap())
				return false;
			const YAML::Node child = node[names[i]];
			if (!child)
				return false;
			return i + 1 == names.size() ? child.IsScalar() : yaml_has(child, names, i + 1);
		}

		void yaml_set(YAML::Node node, const std::vector<std::string>& names, usz i, const std::string& value)
		{
			if (i + 1 == names.size())
			{
				node[names[i]] = value;
				return;
			}
			if (!node[names[i]].IsMap())
				node[names[i]] = YAML::Node(YAML::NodeType::Map);
			yaml_set(node[names[i]], names, i + 1, value);
		}

		// Removes the key and the sections it leaves empty.
		bool yaml_remove(YAML::Node node, const std::vector<std::string>& names, usz i)
		{
			if (!node.IsMap() || !node[names[i]])
				return false;
			if (i + 1 == names.size())
				return node.remove(names[i]);
			YAML::Node child = node[names[i]];
			const bool removed = yaml_remove(child, names, i + 1);
			if (removed && child.IsMap() && child.size() == 0)
				node.remove(names[i]);
			return removed;
		}

		// A game with a serial that its settings can be saved for (not the XMB or a loose program).
		bool has_game_config(const std::string& title_id)
		{
			return title_id.size() == 9 && ::utils::isalpha(title_id[0]);
		}

		// Main thread: saves one setting changed while a game runs. g_cfg then also holds the game's own settings (its
		// custom config or the built-in ones), so writing all of g_cfg would make them everyone's. When the game has
		// that setting of its own it is changed there; otherwise in config.yml, on a copy read from the file.
		void persist_in_game_setting(const std::vector<std::string>& names, const std::string& value)
		{
			if (const std::string title_id = Emu.GetTitleID(); has_game_config(title_id))
			{
				const std::string path = rpcs3::utils::get_custom_config_path(title_id);
				if (fs::file f{path})
				{
					YAML::Node own = load_yaml_map(f.to_string());
					f.close();
					if (yaml_has(own, names))
					{
						yaml_set(own, names, 0, value);
						if (write_config_file(path, yaml_text(own)))
							ui_log.notice("UI: %s = %s saved for %s", fmt::merge(names, "/"), value, title_id);
						return;
					}
				}
			}

			auto copy = std::make_unique<cfg_root>();
			if (fs::file f{global_config_path()})
				copy->from_string(f.to_string());
			cfg::_base* target = find_node(copy.get(), names);
			if (!target || !target->from_string(value))
			{
				ui_log.error("UI: could not save %s = %s", fmt::merge(names, "/"), value);
				return;
			}
			write_config_file(global_config_path(), copy->to_string());
		}

		// Saves a setting the Settings tab or the pause menu changed in g_cfg.
		void save_setting(const cfg::_base* node)
		{
			if (!g_in_game)
			{
				save_settings();
				return;
			}
			g_host.run_on_main([names = node_path(node), value = node->to_string()]()
			{
				persist_in_game_setting(names, value);
			});
		}

		// Saves `node` = `value` while the running game keeps the current value (applies at the next boot).
		bool save_setting_for_next_boot(const cfg::_base* node, const std::string& value)
		{
			std::vector<std::string> names = node_path(node);
			if (names.empty())
				return false;
			g_host.run_on_main([names = std::move(names), value]()
			{
				persist_in_game_setting(names, value);
			});
			return true;
		}

		// ---- Firmware install ----
		void start_firmware_install(const std::string& path)
		{
			if (g_installing.exchange(true))
				return;

			g_job = std::make_unique<uwp_firmware::progress>();
			g_job_title = "Installing PS3 firmware...";
			set_status("Installing firmware...", col_warn);

			std::thread([path, prog = g_job.get()]()
			{
				const std::string error = uwp_firmware::install(path, *prog);
				const std::string version = prog->version;

				// Unmount and pick the firmware up for the next boot, on the main thread.
				g_host.run_on_main([error, version]()
				{
					Emu.Init();
					g_installing = false;

					if (error.empty())
					{
						set_status(fmt::format("Firmware %s installed", version), col_ok);
						post_modal("Firmware installed", fmt::format("PS3 firmware %s is installed. Games now use the firmware's libraries.", version));
						if (g_ui)
							g_ui->fonts_need_reload = true; // the firmware brings Japanese/Korean/Chinese fonts
					}
					else
					{
						set_status("Firmware installation failed", col_err);
						post_modal("Firmware installation failed", error + "\n\nDetails are in RPCS3.log (LocalState/rpcs3).");
					}
					rescan();
				});
			}).detach();
		}

		// ---- License (.rap) install: copied to dev_hdd0/home/<user>/exdata, like desktop RPCS3 ----
		std::string install_license(const std::string& path)
		{
			const std::string name = path.substr(path.rfind('/') + 1);
			const std::string exdata = rpcs3::utils::get_hdd0_dir() + "home/" + Emu.GetUsr() + "/exdata/";
			fs::file from(path);
			if (!from || from.size() < 0x10)
			{
				ui_log.error("UI: can't read license %s (%s)", path, fs::g_tls_error);
				return "The license file couldn't be read.";
			}
			fs::create_path(exdata);
			const std::string to = exdata + name.substr(0, name.find_last_of('.')) + ".rap";
			if (!fs::write_file(to, fs::rewrite, from.to_vector<u8>()))
			{
				ui_log.error("UI: can't write license %s (%s)", to, fs::g_tls_error);
				return "The license couldn't be saved.";
			}
			ui_log.success("UI: installed license %s", to);
			return {};
		}

		// ---- Package (.pkg) install ----
		void start_package_install(const std::string& path, const std::string& title)
		{
			if (!g_host.no_storage.empty())
			{
				set_status("Connect a USB drive to install games", col_err);
				post_modal("No USB drive", g_host.no_storage);
				return;
			}

			if (g_installing.exchange(true))
				return;

			g_job = std::make_unique<uwp_firmware::progress>();
			g_job->total = 1000;
			g_job_title = "Installing " + title;
			set_status(g_job_title + "...", col_warn);

			std::thread([path, title, prog = g_job.get()]()
			{
				std::string error;
				std::deque<package_reader> readers;
				readers.emplace_back(path);

				// A full drive is a fatal file::write error in the installer thread, which closes the
				// app; check the space first. The installed files take about as much as the package.
				fs::stat_t pkg{};
				fs::device_stat hdd{};
				const bool known_free = fs::get_stat(path, pkg) && fs::statfs(rpcs3::utils::get_hdd0_dir(), hdd);
				const u64 pkg_size = pkg.size;

				if (!readers[0].is_valid())
				{
					error = "The package is damaged or not a PS3 package.";
				}
				else if (known_free && hdd.avail_free < pkg_size + (64u << 20))
				{
					error = fmt::format("The USB drive is full: the package needs about %u MB, but only %u MB are free. Free some space and try again.",
						(pkg_size >> 20) + 64, hdd.avail_free >> 20);
				}
				else
				{
					package_install_result result{};
					std::deque<std::string> bootables;
					named_thread worker("PKG Installer", [&]()
					{
						result = package_reader::extract_data(readers, bootables, false);
					});
					while (worker <= thread_state::aborting)
					{
						prog->done = static_cast<u32>(readers[0].get_progress(1000));
						std::this_thread::sleep_for(std::chrono::milliseconds(50));
					}
					worker();

					if (result.error == package_install_result::error_type::app_version)
						error = fmt::format("This update is for version %s of the game, but version %s is installed. Install the game or the earlier updates first.",
							result.version.expected, result.version.installed.empty() ? "(none)" : result.version.installed);
					else if (result.error != package_install_result::error_type::no_error)
						error = "The package couldn't be installed.";
				}

				// Licenses that sit next to the package (common for PSN packages on USB drives).
				u32 licenses = 0;
				if (error.empty())
				{
					const std::string dir = path.substr(0, path.rfind('/') + 1);
					for (const auto& e : fs::dir(dir))
					{
						if (!e.is_directory && upper_ext(e.name) == ".RAP" && install_license(dir + e.name).empty())
							licenses++;
					}
				}

				g_installing = false;
				if (error.empty())
				{
					ui_log.success("UI: installed package %s", path);
					set_status(fmt::format("Installed %s", title), col_ok);
					post_modal("Package installed", licenses
						? fmt::format("%s is installed, with %u license file(s) from the same folder. Installed games are on the Games tab.", title, licenses)
						: fmt::format("%s is installed. Installed games are on the Games tab.", title));
				}
				else
				{
					ui_log.error("UI: package install failed: %s (%s)", path, error);
					set_status("Package installation failed", col_err);
					post_modal("Package installation failed", error + "\n\nDetails are in RPCS3.log (LocalState/rpcs3).");
				}
				rescan();
			}).detach();
		}

		// ---- RPCN account (in place of the Qt UI's RPCN dialogs) ----
		void refresh_rpcn_account()
		{
			g_ui->rpcn_account = uwp_rpcn::get_account();
			g_ui->dirty = true;
		}

		void show_modal(std::string title, std::string text, std::string accept_label = {}, std::function<void()> on_accept = {})
		{
			g_ui->dialog = std::make_unique<modal>(modal{std::move(title), std::move(text), std::move(accept_label), std::move(on_accept)});
			g_ui->dirty = true;
		}

		void osk_open_system_keyboard(text_entry& e);

		void ask_text(text_entry::kind type, std::string title, std::string prompt, std::string initial, std::function<void(std::string)> on_done, bool allow_empty = false)
		{
			const usz max_len = type == text_entry::kind::username || type == text_entry::kind::token ? 16 : type == text_entry::kind::host ? 128 : 64;
			auto osk = std::make_unique<text_entry>(text_entry{type, std::move(title), std::move(prompt), std::move(initial), max_len, allow_empty, std::move(on_done)});
			osk->layer = type == text_entry::kind::token ? 1 : 0;
			static u64 s_text_serial = 0;
			osk->serial = ++s_text_serial;
			// The XAML shell shows the field as its own text box (get_text_field()).
			if (!g_xaml_ui)
				osk_open_system_keyboard(*osk);
			g_ui->osk = std::move(osk);
			g_ui->dirty = true;
		}

		// Runs a blocking RPCN call on a worker; `job` returns what to do next on the UI thread.
		void run_rpcn_job(std::string text, std::function<std::function<void()>()> job)
		{
			if (g_rpcn_busy.exchange(true))
				return;
			g_rpcn_busy_text = std::move(text);
			g_data_serial++;

			std::thread([job = std::move(job)]()
			{
				std::function<void()> then = job();
				g_rpcn_busy = false;
				post_ui([then = std::move(then)]()
				{
					refresh_rpcn_account();
					if (then)
						then();
				});
			}).detach();
		}

		bool rpcn_enabled()
		{
			return g_cfg.net.psn_status == np_psn_status::psn_rpcn;
		}

		void set_rpcn_enabled(bool on)
		{
			g_cfg.net.psn_status.set(on ? np_psn_status::psn_rpcn : np_psn_status::disabled);
			ui_log.notice("UI: PSN set to %s", g_cfg.net.psn_status.get());
			save_settings();
		}

		// Logs in with the saved account and reports the result; on success games use RPCN from now on.
		void rpcn_check_login()
		{
			run_rpcn_job("Logging in to RPCN...", []() -> std::function<void()>
			{
				const uwp_rpcn::result res = uwp_rpcn::check_login();
				return [res]()
				{
					if (!res.ok)
					{
						show_modal("RPCN login failed", res.message);
						return;
					}
					std::string text = fmt::format("Logged in as %s. Games that support online play now use RPCN.", res.message);
					if (!rpcn_enabled())
						set_rpcn_enabled(true);
					if (g_cfg.net.net_active != np_internet_status::enabled)
						text += "\n\nThe PS3 network is Disconnected in Settings (or for this game); games stay offline until it is Connected.";
					show_modal("RPCN", text);
				};
			});
		}

		void rpcn_enter_token(std::string prompt)
		{
			ask_text(text_entry::kind::token, "RPCN token", std::move(prompt), {}, [](std::string token)
			{
				if (token.empty())
					return;
				if (!uwp_rpcn::valid_token(token))
				{
					show_modal("Invalid token", "The token has 16 characters: digits and capital letters.");
					return;
				}
				uwp_rpcn::save_token(token);
				refresh_rpcn_account();
				rpcn_check_login();
			}, true);
		}

		// Asks for a password twice (it can't be seen while typing).
		void ask_new_password(std::string title, std::function<void(std::string)> on_done)
		{
			ask_text(text_entry::kind::password, title, "Choose a password.", {}, [title, on_done = std::move(on_done)](std::string password)
			{
				ask_text(text_entry::kind::password, title, "Enter the password a second time.", {}, [password, on_done](std::string again)
				{
					if (again != password)
					{
						show_modal("Passwords don't match", "The two passwords were different. Nothing was changed.");
						return;
					}
					on_done(password);
				});
			});
		}

		void rpcn_create_account()
		{
			ask_text(text_entry::kind::username, "Create an RPCN account", "Choose a username: 3 to 16 letters, digits, - or _. It is case sensitive and others see it.", {}, [](std::string username)
			{
				if (!uwp_rpcn::valid_username(username))
				{
					show_modal("Invalid username", "A username has 3 to 16 characters: letters, digits, - and _.");
					return;
				}
				ask_new_password("Create an RPCN account", [username](std::string password)
				{
					ask_text(text_entry::kind::email, "Create an RPCN account",
						"Enter your email address. RPCN sends the token that activates the account there, and uses it for nothing else.", {}, [username, password](std::string email)
					{
						if (!uwp_rpcn::valid_email(email))
						{
							show_modal("Invalid email", "That doesn't look like an email address.");
							return;
						}
						show_modal("Create an RPCN account", fmt::format("Create the account\n\nUsername: %s\nEmail: %s\n\non %s?", username, email, g_ui->rpcn_account.host), "Create",
							[username, password, email]()
						{
							run_rpcn_job("Creating the RPCN account...", [username, password, email]() -> std::function<void()>
							{
								const uwp_rpcn::result res = uwp_rpcn::create_account(username, password, email);
								return [res, email]()
								{
									if (!res.ok)
									{
										show_modal("Account not created", res.message);
										return;
									}
									rpcn_enter_token(fmt::format("The account is created and saved on this console. RPCN emailed a token to %s: enter it here. "
										"(Done with nothing typed to enter it later from the RPCN account screen.)", email));
								};
							});
						});
					});
				});
			});
		}

		void rpcn_log_in()
		{
			ask_text(text_entry::kind::username, "Log in to RPCN", "Your RPCN username (case sensitive).", g_ui->rpcn_account.npid, [](std::string username)
			{
				if (!uwp_rpcn::valid_username(username))
				{
					show_modal("Invalid username", "A username has 3 to 16 characters: letters, digits, - and _.");
					return;
				}
				ask_text(text_entry::kind::password, "Log in to RPCN", fmt::format("The password of %s.", username), {}, [username](std::string password)
				{
					ask_text(text_entry::kind::token, "Log in to RPCN", "The token from the account email (16 characters). Done with nothing typed if the account doesn't need one.", {},
						[username, password](std::string token)
					{
						if (!token.empty() && !uwp_rpcn::valid_token(token))
						{
							show_modal("Invalid token", "The token has 16 characters: digits and capital letters.");
							return;
						}
						// Deriving the password takes a moment (200k PBKDF2 rounds): do it on the worker too.
						run_rpcn_job("Logging in to RPCN...", [username, password, token]() -> std::function<void()>
						{
							uwp_rpcn::save_login(username, password, token);
							return []() { rpcn_check_login(); };
						});
					}, true);
				});
			});
		}

		void rpcn_resend_token()
		{
			run_rpcn_job("Asking RPCN to send the token again...", []() -> std::function<void()>
			{
				const uwp_rpcn::result res = uwp_rpcn::resend_token();
				return [res]()
				{
					if (res.ok)
						show_modal("Token sent", "The token was sent again to the email of the account. Enter it with \"Enter the token\".");
					else
						show_modal("Token not sent", res.message);
				};
			});
		}

		void rpcn_change_password(std::string username)
		{
			ask_text(text_entry::kind::token, "Change the RPCN password", "Enter the password reset token from the email (this is not the account activation token).", {},
				[username](std::string token)
			{
				if (!uwp_rpcn::valid_token(token))
				{
					show_modal("Invalid token", "The token has 16 characters: digits and capital letters.");
					return;
				}
				ask_new_password("Change the RPCN password", [username, token](std::string password)
				{
					run_rpcn_job("Changing the RPCN password...", [username, token, password]() -> std::function<void()>
					{
						const uwp_rpcn::result res = uwp_rpcn::reset_password(username, token, password);
						return [res]()
						{
							if (res.ok)
								rpcn_check_login();
							else
								show_modal("Password not changed", res.message);
						};
					});
				});
			});
		}

		// `send_token`: email a reset token first; otherwise the user already has one.
		void rpcn_forgot_password(bool send_token)
		{
			ask_text(text_entry::kind::username, "Change the RPCN password", "Your RPCN username (case sensitive).", g_ui->rpcn_account.npid, [send_token](std::string username)
			{
				if (!uwp_rpcn::valid_username(username))
				{
					show_modal("Invalid username", "A username has 3 to 16 characters: letters, digits, - and _.");
					return;
				}
				if (!send_token)
				{
					rpcn_change_password(username);
					return;
				}
				ask_text(text_entry::kind::email, "Change the RPCN password", "The email address of the account. RPCN emails a password reset token there.", {}, [username](std::string email)
				{
					if (!uwp_rpcn::valid_email(email))
					{
						show_modal("Invalid email", "That doesn't look like an email address.");
						return;
					}
					run_rpcn_job("Asking RPCN for a password reset token...", [username, email]() -> std::function<void()>
					{
						const uwp_rpcn::result res = uwp_rpcn::send_reset_token(username, email);
						return [res, username]()
						{
							if (res.ok)
								rpcn_change_password(username);
							else
								show_modal("Token not sent", res.message);
						};
					});
				});
			});
		}

		void open_rpcn_menu()
		{
			refresh_rpcn_account();
			auto menu = std::make_unique<list_menu>();
			menu->title = "RPCN account";
			menu->header = []()
			{
				const uwp_rpcn::account& acc = g_ui->rpcn_account;
				std::string text = "RPCN is RPCS3's free replacement for the PlayStation Network: online play, rooms, scores and trophies sync between RPCS3 users.\n\n";
				if (acc.npid.empty())
					text += "Account: none on this console";
				else
					text += fmt::format("Account: %s%s", acc.npid, acc.has_token ? "" : "  (no token entered)");
				text += fmt::format("\nServer: %s\nGames use RPCN: %s", acc.host, rpcn_enabled() ? "yes" : fmt::format("no (PSN is %s)", g_cfg.net.psn_status.get()));
				return text;
			};
			menu->build = []()
			{
				const uwp_rpcn::account& acc = g_ui->rpcn_account;
				std::vector<menu_item> items;
				if (!acc.npid.empty())
				{
					items.push_back({"Check the login", "Connects to the server and logs in with the saved account.", rpcn_check_login});
					items.push_back({"Enter the token", "The activation token from the account email.", []() { rpcn_enter_token("The token from the account email (16 characters)."); }});
					items.push_back({"Send the token again", "Emails the activation token again (once per 24 hours).", rpcn_resend_token});
				}
				items.push_back({"Create a new account", "Username, password and an email address for the activation token.", rpcn_create_account});
				items.push_back({acc.npid.empty() ? "Log in with an existing account" : "Log in with another account", "An account made in RPCS3 on a PC works here too.", rpcn_log_in});
				items.push_back({"Forgot the password", "Emails a password reset token (once per 24 hours), then sets a new password.", []() { rpcn_forgot_password(true); }});
				items.push_back({"Enter a password reset token", "When the reset token email has already arrived.", []() { rpcn_forgot_password(false); }});
				items.push_back({rpcn_enabled() ? "Turn RPCN off for games" : "Turn RPCN on for games", "Same as Settings > PSN (RPCN or Disconnected).", []()
				{
					set_rpcn_enabled(!rpcn_enabled());
					g_ui->dirty = true;
				}});
				items.push_back({"Server", "np.rpcs3.net is the official server; change it only for a private one.", []()
				{
					ask_text(text_entry::kind::host, "RPCN server", "Server address, as host or host:port. Empty: the official server.", g_ui->rpcn_account.host, [](std::string host)
					{
						if (host.empty())
							host = g_cfg_rpcn.host.def;
						if (!parse_rpcn_host(host))
						{
							show_modal("Invalid server", "Enter a host name or address, optionally followed by :port.");
							return;
						}
						uwp_rpcn::save_host(host);
						refresh_rpcn_account();
					}, true);
				}});
				if (!acc.npid.empty())
				{
					items.push_back({"Remove the account from this console", "Forgets the username, password and token here. The account itself stays.", []()
					{
						show_modal("Remove the RPCN account", fmt::format("Remove %s from this console? You can log in again later.", g_ui->rpcn_account.npid), "Remove", []()
						{
							uwp_rpcn::forget_account();
							if (rpcn_enabled())
								set_rpcn_enabled(false);
							refresh_rpcn_account();
						});
					}});
				}
				return items;
			};
			g_ui->menu = std::move(menu);
			g_ui->dirty = true;
		}

		// ---- Pause menu pages: what RPCS3's home menu opened as overlays ----
		std::string current_trophy_set()
		{
			auto* current = g_fxo->try_get<current_trophy_name>();
			if (!current)
				return {};
			std::lock_guard lock(current->mtx);
			return current->name;
		}

		struct trophy_row
		{
			std::string name, detail;
			char grade = 'B';
			bool earned = false;
		};

		// The running game's trophies (as overlay_trophy_list_dialog reads them).
		bool load_trophies(const std::string& set, std::string& game, std::vector<trophy_row>& rows)
		{
			const std::string base = "/dev_hdd0/home/" + Emu.GetUsr() + "/trophy/" + set;
			TROPUSRLoader usr;
			if (!usr.Load(base + "/TROPUSR.DAT", base + "/TROPCONF.SFM").success)
				return false;
			fs::file config(vfs::get(base + "/TROPCONF.SFM"));
			trophy_xml_document doc;
			if (!config || !doc.Read(config.to_string()))
				return false;
			const std::shared_ptr<rXmlNode> root = doc.GetRoot();
			for (std::shared_ptr<rXmlNode> n = root ? root->GetChildren() : nullptr; n; n = n->GetNext())
			{
				if (n->GetName() == "title-name")
				{
					game = n->GetNodeContent();
					continue;
				}
				if (n->GetName() != "trophy")
					continue;
				trophy_row row;
				const s32 id = atoi(n->GetAttribute("id").c_str());
				row.earned = usr.GetTrophyUnlockState(id);
				row.grade = n->GetAttribute("ttype").empty() ? 'B' : n->GetAttribute("ttype")[0];
				const bool hidden = !n->GetAttribute("hidden").empty() && n->GetAttribute("hidden")[0] == 'y';
				if (hidden && !row.earned)
				{
					row.name = "Hidden trophy";
					row.detail = "Earn it to see what it is.";
				}
				else
				{
					for (std::shared_ptr<rXmlNode> c = n->GetChildren(); c; c = c->GetNext())
					{
						if (c->GetName() == "name")
							row.name = c->GetNodeContent();
						else if (c->GetName() == "detail")
							row.detail = c->GetNodeContent();
					}
				}
				rows.push_back(std::move(row));
			}
			return true;
		}

		void open_trophy_list()
		{
			const std::string set = current_trophy_set();
			std::string game;
			std::vector<trophy_row> rows;
			if (set.empty() || !load_trophies(set, game, rows))
			{
				show_modal("Trophies", "This game's trophies couldn't be read (RPCS3.log has the details).");
				return;
			}
			auto menu = std::make_unique<list_menu>();
			menu->title = game.empty() ? std::string("Trophies") : "Trophies: " + game;
			menu->header = [rows]()
			{
				u32 earned = 0;
				std::map<char, std::pair<u32, u32>> by_grade;
				for (const trophy_row& r : rows)
				{
					earned += r.earned;
					by_grade[r.grade].first += r.earned;
					by_grade[r.grade].second++;
				}
				std::string text = fmt::format("%u of %u earned", earned, rows.size());
				for (const auto& [grade, name] : std::initializer_list<std::pair<char, const char*>>{{'P', "Platinum"}, {'G', "Gold"}, {'S', "Silver"}, {'B', "Bronze"}})
				{
					if (const auto it = by_grade.find(grade); it != by_grade.end())
						text += fmt::format("   ·   %s %u/%u", name, it->second.first, it->second.second);
				}
				return text;
			};
			menu->build = [rows]()
			{
				std::vector<menu_item> items;
				for (const trophy_row& r : rows)
				{
					const char* grade = r.grade == 'P' ? "Platinum" : r.grade == 'G' ? "Gold" : r.grade == 'S' ? "Silver" : "Bronze";
					items.push_back({fmt::format("%s%s", r.earned ? "✓  " : "", r.name), fmt::format("%s · %s · %s", grade, r.earned ? "Earned" : "Not earned yet", r.detail), {}});
				}
				return items;
			};
			g_ui->menu = std::move(menu);
			g_ui->dirty = true;
		}

		// RPCN friends, as RPCS3's friends overlay lists them; reloaded after each change.
		uwp_rpcn::friends g_friends;
		std::string g_friends_error;
		bool g_friends_opening = false; // open_friends() ran: the first answer opens the screen

		void show_friends_menu();

		void reload_friends(std::string busy_text = "Loading the friends list...")
		{
			run_rpcn_job(std::move(busy_text), []() -> std::function<void()>
			{
				uwp_rpcn::friends list;
				const uwp_rpcn::result res = uwp_rpcn::get_friends(list);
				return [res, list = std::move(list)]()
				{
					g_friends = list;
					g_friends_error = res.ok ? std::string() : res.message;
					show_friends_menu();
				};
			});
		}

		void friend_action(std::string busy_text, std::function<uwp_rpcn::result()> call, std::string failed_title)
		{
			run_rpcn_job(std::move(busy_text), [call = std::move(call), failed_title = std::move(failed_title)]() -> std::function<void()>
			{
				const uwp_rpcn::result res = call();
				return [res, failed_title]()
				{
					if (!res.ok)
						show_modal(failed_title, res.message);
					reload_friends();
				};
			});
		}

		void show_friends_menu()
		{
			// Only while the friends screen is up (or opening): a late answer doesn't open it again.
			if (g_ui->menu ? g_ui->menu->title != "Friends" : !g_friends_opening)
				return;
			g_friends_opening = false;
			auto menu = std::make_unique<list_menu>();
			menu->title = "Friends";
			menu->header = []()
			{
				if (!g_friends_error.empty())
					return g_friends_error;
				u32 online = 0;
				for (const auto& f : g_friends.list)
					online += f.online;
				return fmt::format("%u friends, %u online. %u requests to answer, %u sent.", g_friends.list.size(), online, g_friends.requests_received.size(), g_friends.requests_sent.size());
			};
			menu->build = []()
			{
				std::vector<menu_item> items;
				items.push_back({"Add a friend", "Sends a friend request to an RPCN user.", []()
				{
					ask_text(text_entry::kind::username, "Add a friend", "The friend's RPCN username.", {}, [](std::string name)
					{
						if (!uwp_rpcn::valid_username(name))
						{
							show_modal("Invalid username", "A username has 3 to 16 characters: letters, digits, - and _.");
							return;
						}
						friend_action("Sending the request...", [name]() { return uwp_rpcn::add_friend(name); }, "Friend request not sent");
					});
				}});
				items.push_back({"Refresh", "Loads the list again from the server.", []() { reload_friends(); }});
				for (const std::string& name : g_friends.requests_received)
				{
					items.push_back({name, "Wants to be your friend: A to accept", [name]()
					{
						show_modal("Friend request", fmt::format("Accept %s as a friend?", name), "Accept", [name]()
						{
							friend_action("Accepting...", [name]() { return uwp_rpcn::add_friend(name); }, "Couldn't accept the request");
						});
					}});
					items.push_back({"Reject " + name, "Rejects the friend request.", [name]()
					{
						show_modal("Friend request", fmt::format("Reject %s's request?", name), "Reject", [name]()
						{
							friend_action("Rejecting...", [name]() { return uwp_rpcn::remove_friend(name); }, "Couldn't reject the request");
						});
					}});
				}
				for (const auto& f : g_friends.list)
				{
					const std::string status = !f.online ? "Offline" : f.playing.empty() ? "Online" : "Online, playing " + f.playing;
					items.push_back({f.name, status + ". A to remove.", [name = f.name]()
					{
						show_modal("Remove friend", fmt::format("Remove %s from your friends?", name), "Remove", [name]()
						{
							friend_action("Removing...", [name]() { return uwp_rpcn::remove_friend(name); }, "Couldn't remove the friend");
						});
					}});
				}
				for (const std::string& name : g_friends.requests_sent)
				{
					items.push_back({name, "Request sent, not answered yet. A to cancel it.", [name]()
					{
						show_modal("Friend request", fmt::format("Cancel your request to %s?", name), "Cancel request", [name]()
						{
							friend_action("Cancelling...", [name]() { return uwp_rpcn::remove_friend(name); }, "Couldn't cancel the request");
						});
					}});
				}
				for (const std::string& name : g_friends.blocked)
					items.push_back({name, "Blocked.", {}});
				return items;
			};
			g_ui->menu = std::move(menu);
			g_ui->dirty = true;
		}

		void open_load_state_menu()
		{
			auto menu = std::make_unique<list_menu>();
			menu->title = "Load a save state";
			menu->header = []() { return std::string("The game starts again from where the state was saved. Progress since then is lost."); };
			menu->build = []()
			{
				std::vector<menu_item> items;
				for (u32 i = 1; i <= 4 && boot_current_game_savestate(true, i); i++)
				{
					items.push_back({i == 1 ? std::string("Newest save state") : fmt::format("Save state %u (older)", i), "A to load it.", [i]()
					{
						if (g_ui)
							g_ui->menu.reset();
						g_paused_by_menu = false;
						g_host.run_on_main([i]()
						{
							ui_log.notice("UI: loading save state %u from the pause menu", i);
							boot_current_game_savestate(false, i);
						});
					}});
				}
				return items;
			};
			g_ui->menu = std::move(menu);
			g_ui->dirty = true;
		}

		// ---- Settings ----
		std::vector<setting_item> make_settings()
		{
			return {
				{"Resolution scale", &g_cfg.video.resolution_scale_percent, {"50", "75", "100", "150", "200", "300"}, "Internal rendering resolution in percent of 720p."},
				{"Frame limit", &g_cfg.video.frame_limit, {}, "Auto follows the game's refresh rate."},
				{"VSync", &g_cfg.video.vsync, {}, {}},
				// No interpreter modes: on Dozen their preload builds ~6700 pipelines of ~2 s each before the game
				// starts (72 minutes in Portal 2 on .76) and keeps 1.1 GB of D3D12 pipelines for the whole run.
				{"Shader mode", &g_cfg.video.shadermode, {"Async Recompiler (multi-threaded)", "Legacy Recompiler (single-threaded)"},
					"Async compiles shaders on several threads. Legacy uses one thread and stutters more."},
				{"Write color buffers", &g_cfg.video.write_color_buffers, {"false", "true"}, "Needed by some games for correct visuals; costs speed."},
				{"Stats overlay", &g_cfg.video.perf_overlay.enabled, {"false", "true"}, "FPS, CPU and GPU load in the top right corner while a game runs."},
				{"Stats detail", &g_cfg.video.perf_overlay.level, {"Minimal", "Low", "Medium", "High"}, "Minimal = FPS. Low adds CPU. Medium adds PPU/SPU/RSX. High adds GPU load."},
#ifdef LLVM_AVAILABLE
				{"LLVM compile threads", &g_cfg.core.llvm_threads, {"0", "1", "2", "3", "4", "6"}, "0 = automatic. Fewer threads use less memory while compiling."},
#endif
				{"Preferred SPU threads", &g_cfg.core.preferred_spu_threads, {"0", "1", "2", "3", "4", "5", "6"}, "0 = automatic."},
				{"Network", &g_cfg.net.net_active, {"Connected", "Disconnected"}, "Internet access for the XMB and games (browser, updates, online modes)."},
				{"PSN", &g_cfg.net.psn_status, {}, "RPCN: online play through RPCS3's server (needs an account). Simulated: signed in, offline."},
				{"RPCN account", nullptr, {}, "Create an account or log in for online play (A to open).", open_rpcn_menu, []()
				{
					return g_ui->rpcn_account.npid.empty() ? std::string("Not set up") : g_ui->rpcn_account.npid;
				}},
				{"System language", &g_cfg.sys.language, {}, {}},
				{"Send logs to the developer", &uwp_telemetry::setting(), {"false", "true"},
					fmt::format("Logs of game starts, exits and crashes and frame rate reports, without personal data, to help fix the emulator. Anonymous ID %s.", uwp_telemetry::install_id()),
					{}, {}, []()
				{
					uwp_telemetry::set_consent(uwp_telemetry::setting().get());
					g_data_serial++;
				}},
			};
		}

		void change_setting(setting_item& item, int dir)
		{
			if (!item.node)
			{
				if (dir > 0 && item.on_open)
					item.on_open();
				return;
			}
			std::vector<std::string> values = item.values.empty() ? item.node->to_list() : item.values;
			if (values.empty())
				return;
			const std::string cur = item.node->to_string();
			auto it = std::find(values.begin(), values.end(), cur);
			int idx = it == values.end() ? 0 : static_cast<int>(it - values.begin()) + dir;
			idx = (idx + static_cast<int>(values.size())) % static_cast<int>(values.size());
			if (item.node->from_string(values[idx]))
			{
				ui_log.notice("UI: setting '%s' = %s", item.label, values[idx]);
				if (item.on_change)
					item.on_change();
				else
					save_setting(item.node);
			}
		}

		// Boots a Games-tab entry the way the menu does and remembers it as the last played game.
		void start_game_entry(const game_entry& game)
		{
			// A second press while the first boot is still queued would boot again over the running game.
			if (g_in_game.exchange(true))
			{
				ui_log.warning("UI: not booting '%s': a game is already starting or running", game.title);
				return;
			}
			{
				std::lock_guard lock(g_data_mutex);
				g_select_path = game.path;
			}
			ui_log.notice("UI: boot '%s' (%s)", game.title, game.path);
			// Remembered so the next start of the app selects it again.
			fs::write_file(last_game_file(), fs::rewrite, game.path);

			set_status(fmt::format("Starting %s...", game.title), col_dim);
			g_host.run_on_main([game]()
			{
				if (!Emu.IsStopped(true))
				{
					ui_log.warning("UWP: not booting '%s': the emulator is not stopped", game.path);
					return;
				}
				if (!boot(game.path))
				{
					g_in_game = false;
				}
			});
		}

		// The confirmation for an Install-tab entry. Licenses install at once and answer with a result.
		std::unique_ptr<modal> make_install_prompt(const install_entry& item)
		{
			const std::string size = fmt::format("%.1f MB", item.size / 1048576.0);
			if (item.type == install_entry::kind::firmware)
			{
				const std::string installed = utils::get_firmware_version();
				std::string text = fmt::format("Install the PS3 firmware from\n%s (%s)?", item.path, size);
				if (!installed.empty())
					text += fmt::format("\n\nFirmware %s is installed now and will be replaced.", installed);
				return std::make_unique<modal>(modal{"Install firmware", text, "Install", [path = item.path]() { start_firmware_install(path); }});
			}
			if (item.type == install_entry::kind::package)
			{
				return std::make_unique<modal>(modal{"Install package", fmt::format("Install %s\nfrom %s (%s)?\n\nLicense (.rap) files in the same folder are installed too.", item.title, item.path, size),
					"Install", [path = item.path, title = item.title]() { start_package_install(path, title); }});
			}

			const std::string error = install_license(item.path);
			if (error.empty())
				return std::make_unique<modal>(modal{"License installed", fmt::format("%s is installed. The game it belongs to can now start.", item.path), {}, {}});
			return std::make_unique<modal>(modal{"License not installed", error, {}, {}});
		}

		// ---- Input ----
		u32 poll_actions(IInspectable* window, u32& raw_buttons)
		{
			ui_state& ui = *g_ui;
			u32 held = 0;
			raw_buttons = 0;

			if (!ui.gamepads_tried)
			{
				ui.gamepads_tried = true;
				HSTRING_HEADER header;
				HSTRING name;
				const wchar_t* cls = L"Windows.Gaming.Input.Gamepad";
				HRESULT hr = WindowsCreateStringReference(cls, static_cast<UINT32>(wcslen(cls)), &header, &name);
				if (SUCCEEDED(hr))
					hr = RoGetActivationFactory(name, IID_IGamepadStatics, reinterpret_cast<void**>(&ui.gamepads));
				ui_log.notice("UI: Windows.Gaming.Input gamepad statics: 0x%08x", static_cast<u32>(hr));
			}

			if (ui.gamepads)
			{
				IGamepadVectorView* list = nullptr;
				if (SUCCEEDED(ui.gamepads->get_Gamepads(&list)) && list)
				{
					UINT32 n = 0;
					list->get_Size(&n);
					for (UINT32 i = 0; i < n; i++)
					{
						IGamepad* pad = nullptr;
						if (SUCCEEDED(list->GetAt(i, &pad)) && pad)
						{
							gamepad_reading r{};
							if (SUCCEEDED(pad->GetCurrentReading(&r)))
							{
								raw_buttons |= r.buttons;
								if (r.left_y > 0.6) held |= act_up;
								if (r.left_y < -0.6) held |= act_down;
								if (r.left_x < -0.6) held |= act_left;
								if (r.left_x > 0.6) held |= act_right;
								if (r.left_trigger > 0.6) held |= act_page_up;
								if (r.right_trigger > 0.6) held |= act_page_down;
							}
							pad->Release();
						}
					}
					list->Release();
				}
			}

			if (raw_buttons & gp_up) held |= act_up;
			if (raw_buttons & gp_down) held |= act_down;
			if (raw_buttons & gp_left) held |= act_left;
			if (raw_buttons & gp_right) held |= act_right;
			if (raw_buttons & gp_a) held |= act_accept;
			if (raw_buttons & gp_b) held |= act_back;
			if (raw_buttons & gp_y) held |= act_refresh;
			if (raw_buttons & gp_lb) held |= act_prev_tab;
			if (raw_buttons & gp_rb) held |= act_next_tab;
			if (raw_buttons & gp_x) held |= act_x;
			if (raw_buttons & gp_menu) held |= act_start;
			if (raw_buttons & gp_view) held |= act_view;

			// Keyboard (PC testing): arrows, Enter, Escape/Backspace, F5, Q/E or Tab, PageUp/PageDown.
			if (window)
			{
				auto* keys = reinterpret_cast<ICoreWindowKeys*>(window);
				const auto down = [&](int vk)
				{
					UINT32 st = 0;
					return SUCCEEDED(keys->GetAsyncKeyState(vk, &st)) && (st & 1);
				};
				if (down(38)) held |= act_up;
				if (down(40)) held |= act_down;
				if (down(37)) held |= act_left;
				if (down(39)) held |= act_right;
				if (down(13) || down(32)) held |= act_accept;
				if (down(27) || down(8)) held |= act_back;
				if (down(116)) held |= act_refresh;
				if (down('Q')) held |= act_prev_tab;
				if (down('E') || down(9)) held |= act_next_tab;
				if (down(33)) held |= act_page_up;
				if (down(34)) held |= act_page_down;
			}

			// Edge-triggered presses with auto-repeat for navigation.
			const auto now = std::chrono::steady_clock::now();
			u32 pressed = held & ~ui.held;
			constexpr u32 repeatable = act_up | act_down | act_left | act_right | act_page_up | act_page_down;
			if (pressed & repeatable)
				ui.repeat_at = now + std::chrono::milliseconds(400);
			else if ((held & repeatable) && now >= ui.repeat_at)
			{
				pressed |= held & repeatable;
				ui.repeat_at = now + std::chrono::milliseconds(90);
			}
			ui.held = held;
			return pressed;
		}

		void move_selection(int& sel, int count, int delta)
		{
			if (count <= 0)
			{
				sel = 0;
				return;
			}
			sel = std::clamp(sel + delta, 0, count - 1);
		}

		// ---- On-screen keyboard ----
		// Four rows of ten characters per layer, then the action row (two columns per key).
		constexpr int osk_cols = 10;
		constexpr int osk_char_rows = 4;
		constexpr std::string_view osk_layers[3][osk_char_rows] = {
			{"1234567890", "qwertyuiop", "asdfghjkl@", "zxcvbnm-_."},
			{"1234567890", "QWERTYUIOP", "ASDFGHJKL@", "ZXCVBNM-_."},
			{"1234567890", "!#$%&'*+/=", "?^`{|}~()[", "]<>;:,\"\\@."},
		};
		constexpr std::string_view osk_actions[5] = {"Shift", "Symbols", "Space", "Delete", "Done"};

		bool osk_accepts_type(text_entry::kind type, char c)
		{
			switch (type)
			{
			case text_entry::kind::username: return utils::isalnum(c) || c == '-' || c == '_';
			case text_entry::kind::token: return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z');
			case text_entry::kind::email:
			case text_entry::kind::host: return c != ' ';
			case text_entry::kind::password: return true;
			}
			return true;
		}

		bool osk_accepts(const text_entry& e, char c)
		{
			return osk_accepts_type(e.type, c);
		}

		void osk_type(text_entry& e, char c)
		{
			if (e.type == text_entry::kind::token && c >= 'a' && c <= 'z')
				c = static_cast<char>(c - 'a' + 'A');
			if (e.value.size() < e.max_len && osk_accepts(e, c))
				e.value += c;
			uwp_textinput::set_text(e.value);
		}

		void osk_open_system_keyboard(text_entry& e)
		{
			uwp_textinput::scope sc = uwp_textinput::scope::username;
			switch (e.type)
			{
			case text_entry::kind::username: sc = uwp_textinput::scope::username; break;
			case text_entry::kind::password: sc = uwp_textinput::scope::password; break;
			case text_entry::kind::email: sc = uwp_textinput::scope::email; break;
			case text_entry::kind::token: sc = uwp_textinput::scope::code; break;
			case text_entry::kind::host: sc = uwp_textinput::scope::url; break;
			}
			const text_entry::kind type = e.type;
			// Passwords may use any character (desktop RPCS3 takes the UTF-8 bytes); the other fields are ASCII.
			e.system_keyboard = uwp_textinput::show(sc, e.value, e.max_len, [type](char16_t c)
			{
				if (type == text_entry::kind::password)
					return c >= 0x20;
				return c >= 0x20 && c < 0x7f && osk_accepts_type(type, static_cast<char>(c));
			}, [type](char16_t c)
			{
				return type == text_entry::kind::token && c >= u'a' && c <= u'z' ? static_cast<char16_t>(c - u'a' + u'A') : c;
			});
			ui_log.notice("UI: text field '%s' uses the %s keyboard", e.title, e.system_keyboard ? "system" : "menu's own");
		}

		void osk_finish()
		{
			ui_state& ui = *g_ui;
			if (ui.osk->value.empty() && !ui.osk->allow_empty)
				return;
			uwp_textinput::hide();
			// The callback may open the next entry (or a dialog), so take this one off first.
			auto e = std::move(ui.osk);
			if (e->on_done)
				e->on_done(std::move(e->value));
		}

		// Typed on the system keyboard (or a USB keyboard) since the last frame.
		void osk_poll_system_keyboard()
		{
			ui_state& ui = *g_ui;
			if (!ui.osk)
				return;
			std::string text;
			bool submitted = false;
			if (!uwp_textinput::poll(text, submitted))
				return;
			ui.osk->value = std::move(text);
			ui.dirty = true;
			if (submitted)
				osk_finish();
		}

		void osk_input(u32 pressed)
		{
			ui_state& ui = *g_ui;
			text_entry& e = *ui.osk;

			if (pressed & act_up) e.row = (e.row + osk_char_rows) % (osk_char_rows + 1);
			if (pressed & act_down) e.row = (e.row + 1) % (osk_char_rows + 1);
			if (pressed & act_left) e.col = (e.col + osk_cols - (e.row == osk_char_rows ? 2 : 1)) % osk_cols;
			if (pressed & act_right) e.col = (e.col + (e.row == osk_char_rows ? 2 : 1)) % osk_cols;
			if (e.row == osk_char_rows)
				e.col &= ~1;

			if (pressed & act_x) e.layer = e.layer == 1 ? 0 : 1;
			if (pressed & act_refresh) e.layer = e.layer == 2 ? 0 : 2;

			if (pressed & act_view) e.reveal = !e.reveal;

			if (pressed & act_start)
			{
				osk_finish();
				return;
			}

			// System keyboard: A opens it again, B cancels.
			if (e.system_keyboard)
			{
				if (pressed & act_back)
				{
					uwp_textinput::hide();
					ui.osk.reset();
				}
				else if (pressed & act_accept)
				{
					osk_open_system_keyboard(e);
				}
				return;
			}

			if (pressed & act_back)
			{
				if (e.value.empty())
				{
					uwp_textinput::hide();
					ui.osk.reset();
					return;
				}
				e.value.pop_back();
				uwp_textinput::set_text(e.value);
			}

			if (pressed & act_accept)
			{
				if (e.row < osk_char_rows)
				{
					osk_type(e, osk_layers[e.layer][e.row][e.col]);
					return;
				}
				switch (e.col / 2)
				{
				case 0: e.layer = e.layer == 1 ? 0 : 1; break;
				case 1: e.layer = e.layer == 2 ? 0 : 2; break;
				case 2: osk_type(e, ' '); break;
				case 3:
					if (!e.value.empty())
						e.value.pop_back();
					uwp_textinput::set_text(e.value);
					break;
				case 4: osk_finish(); break;
				}
			}
		}

		void menu_input(u32 pressed)
		{
			ui_state& ui = *g_ui;
			list_menu& m = *ui.menu;
			const std::vector<menu_item> items = m.build();
			if (pressed & act_back)
			{
				ui.menu.reset();
				return;
			}
			if (pressed & act_up) move_selection(m.sel, ::size32(items), -1);
			if (pressed & act_down) move_selection(m.sel, ::size32(items), 1);
			m.sel = std::clamp(m.sel, 0, std::max(static_cast<int>(items.size()) - 1, 0));
			if ((pressed & act_accept) && m.sel < static_cast<int>(items.size()) && items[m.sel].on_select)
				items[m.sel].on_select();
		}

		void handle_input(u32 pressed)
		{
			ui_state& ui = *g_ui;
			if (!pressed)
				return;
			ui.dirty = true;

			if (ui.dialog)
			{
				if (pressed & act_accept)
				{
					auto d = std::move(ui.dialog);
					if (d->on_accept)
						d->on_accept();
				}
				else if (pressed & act_back)
				{
					auto d = std::move(ui.dialog);
					if (d->on_cancel)
						d->on_cancel();
				}
				return;
			}

			if (g_installing || g_rpcn_busy)
				return;

			if (ui.osk)
			{
				// The system keyboard has the gamepad while it is up.
				if (!uwp_textinput::keyboard_visible())
					osk_input(pressed);
				return;
			}

			if (ui.menu)
			{
				menu_input(pressed);
				return;
			}

			if (pressed & act_prev_tab)
				ui.current = static_cast<tab>((static_cast<u32>(ui.current) + 2) % 3);
			if (pressed & act_next_tab)
				ui.current = static_cast<tab>((static_cast<u32>(ui.current) + 1) % 3);
			if (pressed & act_refresh)
				rescan();

			const int t = static_cast<int>(ui.current);
			int& sel = ui.sel[t];

			std::unique_lock lock(g_data_mutex);
			const int count = ui.current == tab::games ? ::size32(g_games) : ui.current == tab::install ? ::size32(g_installs) : ::size32(ui.settings);

			if (pressed & act_up) move_selection(sel, count, -1);
			if (pressed & act_down) move_selection(sel, count, 1);
			if (pressed & act_page_up) move_selection(sel, count, -8);
			if (pressed & act_page_down) move_selection(sel, count, 8);

			if (ui.current == tab::settings && count)
			{
				if (pressed & act_left) change_setting(ui.settings[sel], -1);
				if (pressed & (act_right | act_accept)) change_setting(ui.settings[sel], 1);
			}

			if (!(pressed & act_accept) || !count)
				return;

			if (ui.current == tab::games)
			{
				const game_entry game = g_games[sel];
				lock.unlock();
				start_game_entry(game);
			}
			else if (ui.current == tab::install)
			{
				const install_entry item = g_installs[sel];
				lock.unlock();
				ui.dialog = make_install_prompt(item);
			}
		}

		// ---- Drawing ----
		void draw_footer(canvas& cv, std::string_view hints)
		{
			const int y = canvas_height - margin_y - 50;
			cv.fill(0, y - 14, canvas_width, 1, 0xff2c2f38);
			cv.text(margin_x, y, hints, 26, col_dim);
		}

		void draw_list_row(canvas& cv, int y, int h, bool selected, std::string_view title, std::string_view detail, u32 detail_color = col_dim)
		{
			const int w = canvas_width - margin_x * 2;
			if (selected)
			{
				cv.fill(margin_x, y, w, h, col_sel);
				cv.fill(margin_x, y, 6, h, col_accent);
			}
			cv.text(margin_x + 30, y + 10, title, 34, col_text, w - 60);
			if (!detail.empty())
				cv.text(margin_x + 30, y + 54, detail, 24, selected ? 0xffc8d6ee : detail_color, w - 60);
		}

		// Placeholder tile for entries without ICON0.PNG: a tinted card with a short label.
		void draw_placeholder_icon(canvas& cv, int x, int y, int w, int h, const game_entry& g)
		{
			const bool xmb = g.kind == "System software";
			cv.fill(x, y, w, h, xmb ? 0xff2a3a66 : 0xff262a35);
			const std::string_view label = xmb ? "XMB" : g.kind == "Disc image (ISO)" ? "ISO" : "ELF";
			const int size = std::max(16, h * 2 / 5);
			cv.text(x + (w - cv.text_width(label, size)) / 2, y + (h - size * 5 / 4) / 2, label, size, xmb ? 0xffc8d6ee : 0xff6c7383);
		}

		void draw_icon(canvas& cv, int x, int y, int w, int h, const game_entry& g)
		{
			if (g.icon)
			{
				// Keep the icon's aspect ratio inside the box, centered on a dark backdrop.
				cv.fill(x, y, w, h, 0xff101116);
				int iw = w, ih = static_cast<int>(u64{static_cast<u32>(w)} * g.icon->h / g.icon->w);
				if (ih > h)
				{
					ih = h;
					iw = static_cast<int>(u64{static_cast<u32>(h)} * g.icon->w / g.icon->h);
				}
				cv.image(*g.icon, x + (w - iw) / 2, y + (h - ih) / 2, iw, ih);
			}
			else
			{
				draw_placeholder_icon(cv, x, y, w, h, g);
			}
		}

		// Games tab: icon list on the left, details of the selected entry on the right.
		void draw_games(canvas& cv, int top, int bottom, int& sel, int& scroll)
		{
			constexpr int row_h = 124;
			constexpr int thumb_w = 182, thumb_h = 100; // ICON0 is 320x176
			const int list_w = 1060;
			const int visible = std::max(1, (bottom - top - 60) / row_h);
			const int count = ::size32(g_games);

			sel = std::clamp(sel, 0, std::max(count - 1, 0));
			if (sel < scroll) scroll = sel;
			if (sel >= scroll + visible) scroll = sel - visible + 1;
			scroll = std::clamp(scroll, 0, std::max(count - visible, 0));

			for (int i = scroll; i < std::min(count, scroll + visible); i++)
			{
				const game_entry& g = g_games[i];
				const bool selected = i == sel;
				const int y = top + (i - scroll) * row_h;
				const int h = row_h - 12;
				if (selected)
				{
					cv.fill(margin_x, y, list_w, h, col_sel);
					cv.fill(margin_x, y, 6, h, col_accent);
				}
				else
				{
					cv.fill(margin_x, y, list_w, h, col_panel);
				}
				const int ix = margin_x + 18, iy = y + (h - thumb_h) / 2;
				draw_icon(cv, ix, iy, thumb_w, thumb_h, g);
				const int tx = ix + thumb_w + 24, tw = margin_x + list_w - tx - 20;
				cv.text(tx, y + 18, g.title, 34, col_text, tw);
				std::string sub = g.title_id.empty() || g.title_id == "?" ? g.kind : g.title_id + "  ·  " + g.kind;
				if (!g.source.empty())
					sub += "  ·  " + g.source;
				cv.text(tx, y + 64, sub, 24, selected ? 0xffc8d6ee : col_dim, tw);
			}

			// Scroll position.
			if (count > visible)
			{
				const int track_h = visible * row_h - 12;
				const int knob_h = std::max(40, track_h * visible / count);
				const int knob_y = top + (track_h - knob_h) * scroll / std::max(1, count - visible);
				cv.fill(margin_x + list_w + 10, top, 4, track_h, 0xff2c2f38);
				cv.fill(margin_x + list_w + 10, knob_y, 4, knob_h, col_accent);
			}

			// Details of the selected entry.
			if (sel >= count)
				return;
			const game_entry& g = g_games[sel];
			const int px = margin_x + list_w + 40, pw = canvas_width - margin_x - px;
			const int ph = bottom - top - 60;
			cv.fill(px, top, pw, ph, col_panel);
			const int big_w = pw - 60, big_h = big_w * 176 / 320;
			draw_icon(cv, px + 30, top + 30, big_w, big_h, g);
			int y = top + 30 + big_h + 26;
			// Up to two lines of title.
			const int title_h = cv.paragraph(px + 30, y, g.title, 34, col_text, big_w);
			y += std::min(title_h, 2 * 34 * 135 / 100) + 10;
			const auto field = [&](std::string_view label, const std::string& value)
			{
				if (value.empty() || value == "?")
					return;
				cv.text(px + 30, y, label, 24, col_dim);
				cv.text(px + 30 + 150, y, value, 24, col_text, big_w - 150);
				y += 36;
			};
			field("Title ID", g.title_id);
			field("Type", g.kind);
			field("Version", g.version);
			field("Location", g.source);
			cv.text(px + 30, top + ph - 60, "A  Start", 30, col_accent);
		}

		void draw_menu(canvas& cv, list_menu& m)
		{
			cv.fill(0, 0, canvas_width, canvas_height, col_shade);
			const int w = 1500, x = (canvas_width - w) / 2;
			const int y = margin_y + 20, h = canvas_height - y * 2;
			cv.fill(x, y, w, h, col_panel);
			cv.fill(x, y, w, 4, col_accent);
			cv.text(x + 50, y + 36, m.title, 40, col_text, w - 100);
			int iy = y + 104;
			iy += cv.paragraph(x + 50, iy, m.header(), 28, col_dim, w - 100) + 30;

			const std::vector<menu_item> items = m.build();
			m.sel = std::clamp(m.sel, 0, std::max(static_cast<int>(items.size()) - 1, 0));
			constexpr int row_h = 84;
			const int list_bottom = y + h - 90;
			const int visible = std::max(1, (list_bottom - iy) / row_h);
			const int first = std::clamp(m.sel - visible + 1, 0, std::max(static_cast<int>(items.size()) - visible, 0));
			for (int i = first; i < std::min<int>(::size32(items), first + visible); i++)
			{
				const int ry = iy + (i - first) * row_h;
				if (i == m.sel)
				{
					cv.fill(x + 30, ry, w - 60, row_h - 8, col_sel);
					cv.fill(x + 30, ry, 6, row_h - 8, col_accent);
				}
				cv.text(x + 60, ry + 6, items[i].label, 30, col_text, w - 120);
				cv.text(x + 60, ry + 46, items[i].detail, 22, i == m.sel ? 0xffc8d6ee : col_dim, w - 120);
			}
			cv.text(x + 50, y + h - 70, "A  Select      B  Back", 30, col_accent);
		}

		void draw_osk(canvas& cv, const text_entry& e)
		{
			cv.fill(0, 0, canvas_width, canvas_height, col_shade);
			constexpr int key_w = 104, key_h = 76, gap = 10;
			const int w = osk_cols * (key_w + gap) - gap + 100, x = (canvas_width - w) / 2;
			const int prompt_h = cv.paragraph(0, canvas_height, e.prompt, 28, col_dim, w - 100); // measure only (drawn off canvas)
			const int keys_h = e.system_keyboard ? 0 : (osk_char_rows + 1) * (key_h + gap);
			const int h = 40 + 60 + prompt_h + 20 + 80 + 30 + keys_h + 90;
			const int y = std::max(20, static_cast<int>(canvas_height - h) / 2);
			cv.fill(x, y, w, h, col_panel);
			cv.fill(x, y, w, 4, col_accent);
			cv.text(x + 50, y + 36, e.title, 40, col_text, w - 100);
			int cy = y + 100;
			cv.paragraph(x + 50, cy, e.prompt, 28, col_dim, w - 100);
			cy += prompt_h + 20;

			// The text so far.
			cv.fill(x + 50, cy, w - 100, 70, 0xff101116);
			std::string shown = e.type == text_entry::kind::password && !e.reveal ? std::string(e.value.size(), '*') : e.value;
			shown += '_';
			// Keep the end in view.
			while (shown.size() > 1 && cv.text_width(shown, 34) > w - 140)
				shown.erase(0, 1);
			cv.text(x + 70, cy + 14, shown, 34, col_text);
			const std::string count = fmt::format("%u/%u", e.value.size(), e.max_len);
			cv.text(x + w - 50 - cv.text_width(count, 22), cy + 76, count, 22, col_dim);
			cy += 110;

			for (int r = 0; r <= osk_char_rows && !e.system_keyboard; r++)
			{
				const int ky = cy + r * (key_h + gap);
				const int keys = r < osk_char_rows ? osk_cols : osk_cols / 2;
				for (int k = 0; k < keys; k++)
				{
					const int span = r < osk_char_rows ? 1 : 2;
					const int kx = x + 50 + k * span * (key_w + gap);
					const int kw = span * (key_w + gap) - gap;
					const bool selected = e.row == r && e.col / span == k;
					std::string label;
					bool usable = true;
					if (r < osk_char_rows)
					{
						char c = osk_layers[e.layer][r][k];
						if (e.type == text_entry::kind::token && c >= 'a' && c <= 'z')
							c = static_cast<char>(c - 'a' + 'A');
						label = std::string(1, c);
						usable = osk_accepts(e, c);
					}
					else
					{
						label = std::string(osk_actions[k]);
						usable = !(k == 2 && !osk_accepts(e, ' '));
						if ((k == 0 && e.layer == 1) || (k == 1 && e.layer == 2))
							label = "[" + label + "]";
					}
					cv.fill(kx, ky, kw, key_h, selected ? col_sel : 0xff262a35);
					if (selected)
						cv.fill(kx, ky + key_h - 5, kw, 5, col_accent);
					const int size = r < osk_char_rows ? 36 : 28;
					cv.text(kx + (kw - cv.text_width(label, size)) / 2, ky + (key_h - size * 5 / 4) / 2, label, size, usable ? col_text : 0xff4c5260);
				}
			}

			std::string hints = e.system_keyboard ? "A  Keyboard      Menu  Done      B  Cancel" : "A  Type      B  Delete (empty: cancel)      X  Shift      Y  Symbols      Menu  Done";
			if (e.type == text_entry::kind::password)
				hints += "      View  Show";
			cv.text(x + 50, y + h - 66, hints, 24, col_accent, w - 100);
		}

		void draw(canvas& cv)
		{
			ui_state& ui = *g_ui;
			cv.fill(0, 0, canvas_width, canvas_height, col_bg);

			// Header: title, firmware state, tabs.
			cv.fill(0, 0, canvas_width, margin_y + 120, col_header);
			cv.text(margin_x, margin_y, "RPCS3", 48, col_text);
			cv.text(margin_x + cv.text_width("RPCS3 ", 48), margin_y + 14, "for Xbox  (unofficial beta build)", 30, col_dim);

			std::unique_lock lock(g_data_mutex);
			const std::string fw = g_fw_version.empty() ? "Firmware: not installed (built-in libraries)" : "Firmware " + g_fw_version;
			const int fw_w = cv.text_width(fw, 28);
			cv.text(canvas_width - margin_x - fw_w, margin_y + 16, fw, 28, g_fw_version.empty() ? col_warn : col_ok);

			static constexpr std::string_view tab_names[] = {"Games", "Install", "Settings"};
			int tx = margin_x;
			for (u32 i = 0; i < 3; i++)
			{
				const bool active = static_cast<u32>(ui.current) == i;
				const int tw = cv.text_width(tab_names[i], 34);
				cv.text(tx, margin_y + 72, tab_names[i], 34, active ? col_text : col_dim);
				if (active)
					cv.fill(tx, margin_y + 116, tw, 4, col_accent);
				tx += tw + 60;
			}
			cv.text(tx, margin_y + 78, "LB / RB", 26, 0xff5c6270);
			if (g_scanning_usb)
			{
				constexpr std::string_view usb_hint = "Reading USB drives...";
				cv.text(canvas_width - margin_x - cv.text_width(usb_hint, 28), margin_y + 78, usb_hint, 28, col_warn);
			}

			const int top = margin_y + 150;
			const int bottom = canvas_height - margin_y - 80;
			const int row_h = 96;
			const int visible = (bottom - top - 60) / row_h;
			const int t = static_cast<int>(ui.current);
			int& sel = ui.sel[t];
			int& scroll = ui.scroll[t];

			const auto keep_visible = [&](int count)
			{
				sel = std::clamp(sel, 0, std::max(count - 1, 0));
				if (sel < scroll) scroll = sel;
				if (sel >= scroll + visible) scroll = sel - visible + 1;
				scroll = std::clamp(scroll, 0, std::max(count - visible, 0));
			};

			if (ui.current == tab::games)
			{
				keep_visible(::size32(g_games));
				if (g_games.empty())
				{
					cv.paragraph(margin_x, top + 20, g_scanning ? "Looking for games..." :
						"No games found.\n\n"
						"Copy a game folder (with PS3_GAME inside, or PARAM.SFO + USRDIR) or an .iso into the app's LocalState\\games folder with the Device Portal's File explorer, "
						"or put games in a GAMES folder on a USB drive. Press Y to look again.", 30, col_dim, canvas_width - margin_x * 2);
				}
				else
				{
					draw_games(cv, top, bottom, sel, scroll);
				}
				draw_footer(cv, "A  Start      Y  Refresh      LB/RB  Tabs      In game: hold View + Menu to return here");
			}
			else if (ui.current == tab::install)
			{
				keep_visible(::size32(g_installs));
				int y = top + 10;
				y += cv.paragraph(margin_x, y,
					"Firmware (PS3UPDAT.PUP), packages (.pkg) and licenses (.rap) from the app's LocalState, LocalState\\firmware or LocalState\\packages folder, "
					"or from a USB drive (root, PS3\\UPDATE, PKG, packages or exdata folder).",
					28, col_dim, canvas_width - margin_x * 2);
				y += 20;
				if (g_installs.empty())
					cv.text(margin_x, y, g_scanning ? "Looking for files..." : "Nothing to install found. Press Y to look again.", 30, col_warn);
				const int list_visible = std::max(1, (bottom - y - 20) / row_h);
				if (sel < scroll) scroll = sel;
				if (sel >= scroll + list_visible) scroll = sel - list_visible + 1;
				for (int i = scroll; i < std::min<int>(::size32(g_installs), scroll + list_visible); i++)
				{
					const install_entry& it = g_installs[i];
					const std::string_view kind = it.type == install_entry::kind::firmware ? "Firmware" : it.type == install_entry::kind::package ? "Package" : "License";
					draw_list_row(cv, y + (i - scroll) * row_h, row_h - 8, i == sel, it.title, fmt::format("%s  ·  %.1f MB  ·  %s", kind, it.size / 1048576.0, it.path));
				}
				draw_footer(cv, "A  Install      Y  Refresh      LB/RB  Tabs");
			}
			else
			{
				keep_visible(::size32(ui.settings));
				for (int i = scroll; i < std::min<int>(::size32(ui.settings), scroll + visible); i++)
				{
					const setting_item& s = ui.settings[i];
					const int y = top + (i - scroll) * row_h;
					draw_list_row(cv, y, row_h - 8, i == sel, s.label, s.note);
					std::string value = s.node ? s.node->to_string() : s.value ? s.value() : std::string();
					if (value == "true" || value == "false")
						value = value == "true" ? "On" : "Off";
					value = s.node ? "<  " + value + "  >" : value + "  >";
					const int vw = cv.text_width(value, 34);
					cv.text(canvas_width - margin_x - 30 - vw, y + 10, value, 34, i == sel ? col_text : col_accent);
				}
				draw_footer(cv, "Left / Right  Change      LB/RB  Tabs      Settings are saved right away");
			}

			// Status line above the footer.
			if (!g_status_text.empty())
				cv.text(margin_x, canvas_height - margin_y - 50 - 60, g_status_text, 28, g_status_color, canvas_width - margin_x * 2);
			lock.unlock();

			if (ui.menu)
				draw_menu(cv, *ui.menu);
			if (ui.osk)
				draw_osk(cv, *ui.osk);

			// RPCN server call in progress.
			if (g_rpcn_busy)
			{
				cv.fill(0, 0, canvas_width, canvas_height, col_shade);
				const int w = 1000, h = 200, x = (canvas_width - w) / 2, y = (canvas_height - h) / 2;
				cv.fill(x, y, w, h, col_panel);
				cv.fill(x, y, w, 4, col_accent);
				cv.text(x + 40, y + 50, g_rpcn_busy_text, 36, col_text, w - 80);
				cv.text(x + 40, y + 120, "This takes a few seconds.", 28, col_dim, w - 80);
			}

			// Firmware install progress.
			if (g_installing && g_job)
			{
				cv.fill(0, 0, canvas_width, canvas_height, col_shade);
				const int w = 1000, h = 260, x = (canvas_width - w) / 2, y = (canvas_height - h) / 2;
				cv.fill(x, y, w, h, col_panel);
				cv.fill(x, y, w, 4, col_accent);
				cv.text(x + 40, y + 36, g_job_title, 40, col_text, w - 80);
				const u32 done = g_job->done, total = g_job->total;
				const int bar_w = w - 80;
				cv.fill(x + 40, y + 120, bar_w, 28, 0xff2c2f38);
				if (total)
					cv.fill(x + 40, y + 120, static_cast<int>(bar_w * u64{done} / total), 28, col_accent);
				cv.text(x + 40, y + 170, total ? fmt::format("%u%%", static_cast<u32>(u64{std::min(done, total)} * 100 / total)) : std::string("Checking the file (this takes a moment)..."), 28, col_dim);
			}

			// Modal dialog.
			if (ui.dialog)
			{
				cv.fill(0, 0, canvas_width, canvas_height, col_shade);
				const int w = 1200, x = (canvas_width - w) / 2;
				const int text_h = 40 * 3 + static_cast<int>(std::count(ui.dialog->text.begin(), ui.dialog->text.end(), '\n') + ui.dialog->text.size() / 60 + 1) * 40;
				const int h = std::min<int>(canvas_height - 200, text_h + 200);
				const int y = (canvas_height - h) / 2;
				cv.fill(x, y, w, h, col_panel);
				cv.fill(x, y, w, 4, col_accent);
				cv.text(x + 50, y + 40, ui.dialog->title, 40, col_text, w - 100);
				cv.paragraph(x + 50, y + 110, ui.dialog->text, 30, col_dim, w - 100);
				const std::string buttons = ui.dialog->accept_label.empty() ? std::string("A  OK") :
					"A  " + ui.dialog->accept_label + "        B  " + (ui.dialog->cancel_label.empty() ? std::string("Cancel") : ui.dialog->cancel_label);
				cv.text(x + 50, y + h - 70, buttons, 30, col_accent);
			}
		}

		void load_fonts(canvas& cv)
		{
			cv.fonts.clear();
			const std::string flash_fonts = g_cfg_vfs.get_dev_flash() + "data/font/";
			const std::string candidates[] = {
				g_host.package_dir + "fonts/DejaVuSans.ttf",
				flash_fonts + "SCE-PS3-VR-R-LATIN.TTF",
				flash_fonts + "SCE-PS3-SR-R-JPN.TTF",
				flash_fonts + "SCE-PS3-YG-R-KOR.TTF",
				flash_fonts + "SCE-PS3-DH-R-CGB.TTF",
			};
			for (const auto& path : candidates)
				cv.fonts.add(path);
			if (cv.fonts.empty())
				ui_log.error("UI: no font found (expected %sfonts/DejaVuSans.ttf)", g_host.package_dir);
		}
	}

	void init(const host_info& host)
	{
		g_host = host;
		g_ui = new ui_state();
		g_ui->settings = make_settings();
		refresh_rpcn_account();
		{
			std::lock_guard lock(g_data_mutex);
			if (fs::file f{last_game_file()})
				g_select_path = f.to_string();
			g_select_pending = true;
		}
		// Remote test runs (LocalState/autoboot) boot straight away: don't scan the drives
		// alongside the boot; the menu's refresh button scans later. The scan starts before the
		// model counts as ready, so the menu's first look already says it is scanning.
		if (!fs::is_file(g_host.local_dir + "autoboot"))
			rescan();
		// Is a newer release on GitHub? The header shows the answer.
		uwp_update::start([]() { g_data_serial++; });
		g_ready = true;
	}

	bool boot(const std::string& path)
	{
		if (!g_host.no_storage.empty())
		{
			ui_log.error("UWP: not booting '%s': %s", path, g_host.no_storage);
			set_status("Connect a USB drive to play", col_err);
			post_modal("No USB drive", g_host.no_storage);
			return false;
		}

		// In App mode the Xbox gives the app 1 GB instead of 5 GB, and every game dies at boot (out of video
		// memory). A reinstall sets it back to App mode: LittleBigPlanet on 0.1.35.0 died 4 times in a row
		// at 1.25 GB "of 1.000 GB" before the player switched it.
		if (u64 limit = 0; rpcs3::utils::g_uwp_app_memory && (rpcs3::utils::g_uwp_app_memory(&limit), limit) && limit < (3ull << 30))
		{
			ui_log.error("UWP: not booting '%s': the app may use only %.2f GB (App mode)", path, limit / 1073741824.0);
			set_status("Set the app type to Game to play", col_err);
			post_modal("App type is set to App",
				fmt::format("The Xbox gives this app only %.1f GB of memory, so games can't run. On the Xbox Home screen, "
					"highlight RPCS3, press the Menu button, choose \"Manage game and add-ons\" or \"View details\", "
					"and set App type to Game. Then start the app again.", limit / 1073741824.0));
			return false;
		}

		// Without firmware every firmware library must use RPCS3's built-in (HLE) version,
		// otherwise the boot stops with "firmware missing". With firmware, use the defaults.
		if (!firmware_installed())
		{
			std::set<std::string> libs;
			for (const auto& [name, hle] : g_prx_list)
			{
				if (!hle)
					libs.emplace(std::string(name) + ":hle");
			}
			g_cfg.core.libraries_control.set_set(std::move(libs));
			ui_log.warning("UWP: no firmware installed, using HLE for all firmware libraries");
		}
		else
		{
			g_cfg.core.libraries_control.set_set({});
		}

		// BootGame reloads the global config file, so store the settings there first.
		Emulator::SaveSettings(g_cfg.to_string(), {});

		ui_log.notice("UWP: booting '%s'", path);
		const bool is_dir = fs::is_dir(path);
		const game_boot_result result = Emu.BootGame(path, "", !is_dir);
		if (result != game_boot_result::no_errors)
		{
			ui_log.error("UWP: boot failed: %s", result);
			set_status(fmt::format("Couldn't start it: %s", result), col_err);
			post_modal("Couldn't start the game", fmt::format("RPCS3 reported: %s\n\nDetails are in RPCS3.log (LocalState/rpcs3).", result));
			return false;
		}

		set_status({}, col_dim);
		return true;
	}

	void on_emulation_started()
	{
		g_in_game = true;
	}

	void on_emulation_stopped()
	{
		g_paused_by_menu = false;
		g_paused_by_suspend = false;
		if (g_in_game.exchange(false))
		{
			ui_log.notice("UI: emulation stopped, back to the menu");
			set_status("Returned from the game", col_dim);
		}
		// g_cfg still holds the game's own settings (custom or built-in config) on top of config.yml. The menu and the
		// next boot save all of g_cfg to config.yml, so they would become every game's settings: read the file again.
		g_host.run_on_main([]()
		{
			if (!Emu.IsStopped(true))
				return; // a restart booted the game again
			if (fs::file f{global_config_path()})
			{
				if (g_cfg.from_string(f.to_string()))
					g_backup_cfg.from_string(g_cfg.to_string());
				else
					ui_log.error("UI: could not read config.yml again after the game");
			}
		});
	}

	bool loading_frame(const std::string& title, const std::string& detail, u32 done, u32 total, const u32*& pixels)
	{
		if (!g_ready)
			return false;

		ui_state& ui = *g_ui;
		if (!ui.fonts_loaded || ui.fonts_need_reload)
		{
			load_fonts(ui.cv);
			ui.fonts_loaded = true;
			ui.fonts_need_reload = false;
		}

		canvas& cv = ui.cv;
		cv.fill(0, 0, canvas_width, canvas_height, col_bg);
		const int x = margin_x, w = canvas_width - margin_x * 2;
		int y = canvas_height / 2 - 120;
		cv.text(x, y, title.empty() ? "Loading" : title, 44, col_text, w);
		y += 80;
		if (total)
		{
			cv.fill(x, y, w, 16, col_header);
			cv.fill(x, y, static_cast<int>(static_cast<u64>(w) * std::min(done, total) / total), 16, col_accent);
			y += 40;
		}
		cv.paragraph(x, y, detail, 28, col_dim, w);
		ui.dirty = true; // the menu redraws when it comes back
		pixels = cv.px.data();
		return true;
	}

	bool frame(IInspectable* core_window, const u32*& pixels, bool& changed)
	{
		changed = false;
		if (!g_ready)
			return false;

		ui_state& ui = *g_ui;
		u32 raw = 0;
		const u32 pressed = poll_actions(core_window, raw);
		std::vector<std::function<void()>> pending_ui;

		if (g_in_game)
		{
			// Hold View + Menu for a second to stop the game.
			const auto now = std::chrono::steady_clock::now();
			if ((raw & (gp_view | gp_menu)) == (gp_view | gp_menu))
			{
				if (ui.combo_since == std::chrono::steady_clock::time_point{})
					ui.combo_since = now;
				else if (!ui.combo_fired && now - ui.combo_since > std::chrono::milliseconds(1000))
				{
					ui.combo_fired = true;
					ui_log.notice("UI: View + Menu held, stopping the game");
					g_host.run_on_main([]() { Emu.GracefulShutdown(false, true); });
				}
			}
			else
			{
				ui.combo_since = {};
				ui.combo_fired = false;
			}

			ui.dirty = true; // redraw when coming back
			return false;
		}

		if (!ui.fonts_loaded || ui.fonts_need_reload)
		{
			load_fonts(ui.cv);
			ui.fonts_loaded = true;
			ui.fonts_need_reload = false;
			ui.dirty = true;
		}

		{
			std::lock_guard lock(g_data_mutex);
			if (!ui.dialog)
			{
				if (auto consent = take_consent_prompt())
					ui.dialog = std::move(consent);
				else if (g_pending_modal)
					ui.dialog = std::move(g_pending_modal);
				ui.dirty = ui.dirty || ui.dialog;
			}
			pending_ui.swap(g_pending_ui);

			// Start on the last played game once the list has it, else on the top entry.
			if (g_select_pending)
			{
				const auto it = g_select_path.empty() ? g_games.end() :
					std::find_if(g_games.begin(), g_games.end(), [](const game_entry& g) { return g.path == g_select_path; });
				if (it != g_games.end() || !g_scanning)
				{
					const int idx = it == g_games.end() ? 0 : static_cast<int>(it - g_games.begin());
					ui.sel[static_cast<int>(tab::games)] = idx;
					ui.scroll[static_cast<int>(tab::games)] = std::max(0, idx - 2);
					g_select_pending = false;
					ui.dirty = true;
				}
			}
		}

		for (auto& fn : pending_ui)
			fn();
		if (!pending_ui.empty())
			ui.dirty = true;

		osk_poll_system_keyboard();

		handle_input(pressed);

		if (g_installing && g_job && g_job->done != ui.last_job_done)
		{
			ui.last_job_done = g_job->done;
			ui.dirty = true;
		}

		if (const u64 serial = g_data_serial; serial != ui.drawn_serial)
		{
			ui.drawn_serial = serial;
			ui.dirty = true;
		}

		if (ui.dirty)
		{
			draw(ui.cv);
			ui.dirty = false;
			changed = true;
		}

		pixels = ui.cv.px.data();
		return true;
	}

	// ---- Model for the XAML shell ----
	namespace
	{
		status_kind to_status_kind(u32 color)
		{
			return color == col_ok ? status_kind::ok : color == col_warn ? status_kind::warn : color == col_err ? status_kind::error : status_kind::dim;
		}

		dialog_view to_dialog_view(const modal& m)
		{
			return {m.title, m.text, m.accept_label, m.on_accept, m.cancel_label, m.on_cancel, m.refresh_settings};
		}

		// The settings list of the XAML pages; index-stable for the lifetime of the app.
		// The pause menu's settings: what RPCS3's home menu offers while a game runs (overlay_home_menu_settings.cpp),
		// minus what has no use on the Xbox (audio backend, fullscreen, mouse and camera, PS Move, recording).
		// They apply to the running game as they do in the home menu and are saved to config.yml.
		std::vector<setting_item> make_pause_settings()
		{
			std::vector<setting_item> out;
			const auto add = [&](std::string_view section, std::string label, cfg::_base* node, std::vector<std::string> values, std::string note,
				std::string_view unit = {}, std::vector<std::string> shown = {})
			{
				setting_item item{std::move(label), node, std::move(values), std::move(note)};
				item.pause = true;
				item.section = section;
				if (shown.empty() && !unit.empty())
				{
					for (const std::string& v : item.values)
						shown.push_back(v + std::string(unit));
				}
				item.shown = std::move(shown);
				out.push_back(std::move(item));
			};
			const auto range = [](s64 from, s64 to, s64 step)
			{
				std::vector<std::string> v;
				for (s64 i = from; i <= to; i += step)
					v.push_back(std::to_string(i));
				return v;
			};
			const std::vector<std::string> on_off = {"false", "true"};

			// Changing these in a running game rebuilds every render target (sync_scaling_config), and on the Xbox the next
			// frame's D3D12 command list then fails to close (E_INVALIDARG, "Out of host memory"). They apply at the next boot.
			add("Video", "Resolution scale", &g_cfg.video.resolution_scale_percent, {"50", "75", "100", "125", "150", "200", "250", "300"}, "Internal rendering resolution in percent of 720p. Applies when the game restarts (Restart game).", "%");
			out.back().on_restart = true;
			add("Video", "Resolution scale threshold", &g_cfg.video.min_scalable_dimension, {"1", "16", "32", "64", "128", "256", "512"}, "Smaller render targets are not scaled. Applies when the game restarts (Restart game).", " px");
			out.back().on_restart = true;
			add("Video", "VSync", &g_cfg.video.vsync, {}, {});
			add("Video", "Frame limit", &g_cfg.video.frame_limit, {}, "Auto follows the game's refresh rate.");
			add("Video", "Anisotropic filter", &g_cfg.video.anisotropic_level_override, {"0", "2", "4", "8", "16"}, "Sharper textures at an angle. Auto lets the game choose.", {}, {"Auto", "2x", "4x", "8x", "16x"});
			add("Video", "Output scaling", &g_cfg.video.output_scaling, {}, "How the picture is scaled to the screen. FSR sharpens it.");
			add("Video", "FSR sharpening", &g_cfg.video.rcas_sharpening_intensity, range(0, 100, 10), "Only with FSR output scaling.", "%");
			add("Video", "Stretch to the screen", &g_cfg.video.stretch_to_display_area, on_off, "Fills the screen instead of keeping the game's aspect ratio.");

			add("Audio", "Volume", &g_cfg.audio.volume, {"0", "10", "20", "30", "40", "50", "60", "70", "80", "90", "100", "125", "150", "175", "200"}, {}, "%");
			add("Audio", "Audio buffering", &g_cfg.audio.enable_buffering, on_off, "Smooths out crackling when the game runs below full speed.");
			add("Audio", "Audio buffer length", &g_cfg.audio.desired_buffer_duration, {"20", "34", "50", "75", "100", "150", "200", "250"}, "Longer is smoother, with more delay.", " ms");
			add("Audio", "Time stretching", &g_cfg.audio.enable_time_stretching, on_off, "Slows the sound down with the game instead of breaking it up (needs buffering).");
			add("Audio", "Time stretching threshold", &g_cfg.audio.time_stretching_threshold, range(0, 100, 5), "Buffer fill below which stretching starts.", "%");

			add("Stats overlay", "Stats overlay", &g_cfg.video.perf_overlay.enabled, on_off, "FPS, CPU and GPU load while a game runs.");
			add("Stats overlay", "Stats detail", &g_cfg.video.perf_overlay.level, {"Minimal", "Low", "Medium", "High"}, "Minimal = FPS. Low adds CPU. Medium adds PPU/SPU/RSX. High adds GPU load.");
			add("Stats overlay", "Framerate graph", &g_cfg.video.perf_overlay.framerate_graph_enabled, on_off, {});
			add("Stats overlay", "Frame time graph", &g_cfg.video.perf_overlay.frametime_graph_enabled, on_off, {});
			add("Stats overlay", "Framerate graph detail", &g_cfg.video.perf_overlay.framerate_graph_detail_level, {}, {});
			add("Stats overlay", "Frame time graph detail", &g_cfg.video.perf_overlay.frametime_graph_detail_level, {}, {});
			add("Stats overlay", "Framerate graph points", &g_cfg.video.perf_overlay.framerate_datapoint_count, {"20", "50", "100", "200", "400"}, {});
			add("Stats overlay", "Frame time graph points", &g_cfg.video.perf_overlay.frametime_datapoint_count, {"50", "100", "170", "300", "600"}, {});
			add("Stats overlay", "Update interval", &g_cfg.video.perf_overlay.update_interval, {"100", "200", "350", "500", "1000"}, {}, " ms");
			add("Stats overlay", "Position", &g_cfg.video.perf_overlay.position, {}, {});
			add("Stats overlay", "Center horizontally", &g_cfg.video.perf_overlay.center_x, on_off, {});
			add("Stats overlay", "Center vertically", &g_cfg.video.perf_overlay.center_y, on_off, {});
			add("Stats overlay", "Horizontal margin", &g_cfg.video.perf_overlay.margin_x, range(0, 20, 1), {}, "%");
			add("Stats overlay", "Vertical margin", &g_cfg.video.perf_overlay.margin_y, range(0, 20, 1), {}, "%");
			add("Stats overlay", "Font size", &g_cfg.video.perf_overlay.font_size, range(6, 36, 2), {}, " px");
			add("Stats overlay", "Opacity", &g_cfg.video.perf_overlay.opacity, range(0, 100, 10), {}, "%");

			add("Notifications", "Trophy popups", &g_cfg.misc.show_trophy_popups, on_off, {});
			add("Notifications", "RPCN popups", &g_cfg.misc.show_rpcn_popups, on_off, "Friend and connection messages.");
			add("Notifications", "Shader compile hint", &g_cfg.misc.show_shader_compilation_hint, on_off, {});
			add("Notifications", "PPU compile hint", &g_cfg.misc.show_ppu_compilation_hint, on_off, {});
			add("Notifications", "Auto save / load hint", &g_cfg.misc.show_autosave_autoload_hint, on_off, {});
			add("Notifications", "Pressure sensitivity hint", &g_cfg.misc.show_pressure_intensity_toggle_hint, on_off, {});
			add("Notifications", "Analog limiter hint", &g_cfg.misc.show_analog_limiter_toggle_hint, on_off, {});
			add("Notifications", "Fatal error hints", &g_cfg.misc.show_fatal_error_hints, on_off, {});
			add("Notifications", "Music during boot", &g_cfg.misc.play_music_during_boot, on_off, "The game's music while it starts.");

			add("Input", "Keep controllers connected", &g_cfg.io.keep_pads_connected, on_off, "Games keep seeing a controller that was turned off.");
			add("Input", "Controller thread mode", &g_cfg.io.pad_mode, {}, {});
			add("Input", "Controller poll sleep", &g_cfg.io.pad_sleep, {"0", "100", "250", "500", "1000", "2000", "5000"}, "Lower reacts faster and uses more CPU.", " µs");

			const std::string risky = "Changes the game's timing: wrong values can slow, speed up, hang or crash games.";
			add("Advanced (can break games)", "Preferred SPU threads", &g_cfg.core.preferred_spu_threads, range(0, 6, 1), "0 = automatic.");
			add("Advanced (can break games)", "Max SPURS threads", &g_cfg.core.max_spurs_threads, range(1, 6, 1), risky);
			add("Advanced (can break games)", "Max CPU preemptions", &g_cfg.core.max_cpu_preempt_count_per_frame, {"0", "10", "25", "50", "100", "200", "400"}, risky);
			add("Advanced (can break games)", "Accurate RSX reservation access", &g_cfg.core.rsx_accurate_res_access, on_off, "Slower; fixes some games.");
			add("Advanced (can break games)", "Sleep timers accuracy", &g_cfg.core.sleep_timers_accuracy, {}, risky);
			add("Advanced (can break games)", "PPU event wake-up delay", &g_cfg.core.ppu_event_wakeup_delay, {"0", "10", "25", "50", "100", "200"}, risky, " µs");
			add("Advanced (can break games)", "Driver wake-up delay", &g_cfg.video.driver_wakeup_delay, {"0", "1", "20", "40", "100", "200", "400", "800"}, risky, " µs");
			add("Advanced (can break games)", "Vblank rate", &g_cfg.video.vblank_rate, {"30", "50", "60", "90", "120", "150", "180", "240"}, risky, " Hz");
			add("Advanced (can break games)", "Vblank NTSC fixup", &g_cfg.video.vblank_ntsc, on_off, {});
			add("Advanced (can break games)", "RSX memory tiling", &g_cfg.video.handle_tiled_memory, on_off, "Fixes some games' graphics; slower.");

			add("Debug", "Debug overlay", &g_cfg.video.debug_overlay, on_off, {});
			add("Debug", "Input debug overlay", &g_cfg.io.pad_debug_overlay, on_off, {});
			add("Debug", "Texture LOD bias", &g_cfg.video.texture_lod_bias, {"-2", "-1.5", "-1", "-0.5", "0", "0.5", "1", "1.5", "2"}, "Negative is sharper, positive is blurrier.");
			return out;
		}

		std::vector<setting_item>& xaml_settings()
		{
			static std::vector<setting_item> items = []()
			{
				std::vector<setting_item> all = make_settings();
				for (setting_item& item : make_pause_settings())
					all.push_back(std::move(item));
				return all;
			}();
			return items;
		}

		// Empty for an action row (no node).
		std::vector<std::string> setting_values(const setting_item& item)
		{
			if (!item.node)
				return {};
			return item.values.empty() ? item.node->to_list() : item.values;
		}
	}

	bool model_ready()
	{
		return g_ready;
	}

	u64 model_serial()
	{
		return g_data_serial;
	}

	void get_snapshot(snapshot& out)
	{
		std::lock_guard lock(g_data_mutex);
		out.games.clear();
		out.games.reserve(g_games.size());
		for (const game_entry& g : g_games)
			out.games.push_back({g.title, g.title_id, g.kind, g.version, g.source, g.path, g.icon});
		out.installs.clear();
		for (const install_entry& i : g_installs)
		{
			out.installs.push_back({i.title, i.type == install_entry::kind::firmware ? "Firmware" : i.type == install_entry::kind::package ? "Package" : "License", i.path, i.size});
		}
		out.firmware = g_fw_version;
		out.status = g_status_text;
		out.status_color = to_status_kind(g_status_color);
		out.select_path = g_select_path;
		out.select_pending = g_select_pending;
		out.scanning = g_scanning;
		out.scanning_usb = g_scanning_usb;
		out.telemetry_id = uwp_telemetry::install_id();
		out.telemetry_on = uwp_telemetry::get_consent() == uwp_telemetry::consent::yes;
		out.app_version = uwp_update::app_version();
		const uwp_update::status upd = uwp_update::get_status();
		out.update_checked = upd != uwp_update::status::unknown;
		out.update_outdated = upd == uwp_update::status::outdated;
		out.update_latest = uwp_update::latest_version();
	}

	void clear_select_pending()
	{
		std::lock_guard lock(g_data_mutex);
		g_select_pending = false;
	}

	job_view get_job()
	{
		job_view v;
		if (g_rpcn_busy)
		{
			v.active = true;
			v.server = true;
			v.title = g_rpcn_busy_text;
		}
		else if (g_installing && g_job)
		{
			v.active = true;
			v.title = g_job_title;
			v.done = g_job->done;
			v.total = g_job->total;
		}
		return v;
	}

	std::optional<dialog_view> take_pending_dialog()
	{
		// Messages of the menu logic itself (the RPCN account screen), UI thread.
		if (g_ui && g_ui->dialog)
		{
			std::optional<dialog_view> out = to_dialog_view(*g_ui->dialog);
			g_ui->dialog.reset();
			return out;
		}
		std::lock_guard lock(g_data_mutex);
		if (const auto consent = take_consent_prompt())
			return to_dialog_view(*consent);
		if (!g_pending_modal)
			return std::nullopt;
		std::optional<dialog_view> out = to_dialog_view(*g_pending_modal);
		g_pending_modal.reset();
		return out;
	}

	void request_rescan()
	{
		rescan();
	}

	void start_game(const std::string& path)
	{
		game_entry game;
		{
			std::lock_guard lock(g_data_mutex);
			const auto it = std::find_if(g_games.begin(), g_games.end(), [&](const game_entry& g) { return g.path == path; });
			if (it == g_games.end())
				return;
			game = *it;
		}
		start_game_entry(game);
	}

	std::optional<dialog_view> install_prompt(const std::string& path)
	{
		if (g_installing)
			return std::nullopt;
		install_entry item;
		{
			std::lock_guard lock(g_data_mutex);
			const auto it = std::find_if(g_installs.begin(), g_installs.end(), [&](const install_entry& i) { return i.path == path; });
			if (it == g_installs.end())
				return std::nullopt;
			item = *it;
		}
		const std::unique_ptr<modal> m = make_install_prompt(item);
		return to_dialog_view(*m);
	}

	namespace
	{
		setting_view to_setting_view(setting_item& item)
		{
			setting_view v;
			v.label = item.label;
			v.note = item.note;
			if (!item.node)
			{
				v.action = true;
				v.value = item.value ? item.value() : std::string("Open");
				return v;
			}
			v.values = setting_values(item);
			v.toggle = v.values.size() == 2 && v.values[0] == "false" && v.values[1] == "true";
			for (usz j = 0; j < v.values.size(); j++)
				v.shown.push_back(v.toggle ? (v.values[j] == "true" ? "On" : "Off") : j < item.shown.size() ? item.shown[j] : v.values[j]);
			if (!g_in_game || item.pending == item.node->to_string())
				item.pending.clear();
			const std::string cur = item.pending.empty() ? item.node->to_string() : item.pending;
			auto it = std::find(v.values.begin(), v.values.end(), cur);
			if (it == v.values.end())
			{
				// Numbers: "4" in the list is "4.000000" in a float setting, and a value set elsewhere picks the nearest entry.
				char* end = nullptr;
				const double cur_num = std::strtod(cur.c_str(), &end);
				if (end != cur.c_str())
				{
					it = std::min_element(v.values.begin(), v.values.end(), [cur_num](const std::string& a, const std::string& b)
					{
						return std::abs(std::strtod(a.c_str(), nullptr) - cur_num) < std::abs(std::strtod(b.c_str(), nullptr) - cur_num);
					});
				}
			}
			v.current = it == v.values.end() ? 0 : static_cast<int>(it - v.values.begin());
			// The pause menu: what RPCS3's home menu changes while a game runs. The Settings tab shows the rest.
			v.in_game = item.pause;
			v.pause_only = item.pause;
			v.section = item.section;
			return v;
		}

		// ---- Settings of one game, from the Games tab (UI thread) ----
		// The pause menu's settings plus a few that only apply at boot, on a copy of the configuration the game boots
		// with: config.yml, then the game's own settings. Changes go to custom_configs/config_<serial>.yml, which RPCS3
		// applies on top of config.yml when the game boots; only the settings that differ from config.yml are kept there.
		struct game_settings_state
		{
			std::string title_id, title, path;
			std::unique_ptr<cfg_root> cfg;
			YAML::Node own;   // the game's own settings: its custom config, or the built-in ones until something is changed
			bool own_file = false; // the custom config exists
			bool built_in = false; // `own` came from the built-in settings (get_database_config)
			std::vector<setting_item> items;
		};

		std::unique_ptr<game_settings_state> g_game_settings;

		std::vector<setting_item> make_game_settings()
		{
			std::vector<setting_item> out = make_pause_settings();
			const auto add = [&](std::string section, std::string label, cfg::_base* node, std::vector<std::string> values, std::string note,
				std::vector<std::string> shown = {})
			{
				setting_item item{std::move(label), node, std::move(values), std::move(note)};
				item.pause = true;
				item.section = std::move(section);
				item.shown = std::move(shown);
				out.push_back(std::move(item));
			};
			const std::vector<std::string> on_off = {"false", "true"};
			add("Video", "Write color buffers", &g_cfg.video.write_color_buffers, on_off, "Needed by some games for correct visuals; costs speed.");
			add("CPU", "SPU block size", &g_cfg.core.spu_block_size, {}, "How much SPU code is compiled at once. Safe works in the most games; Mega and Giga can be a little faster.");
			add("CPU", "SPU loop detection", &g_cfg.core.spu_loop_detection, on_off, "Lets other threads run while an SPU waits in a loop. Faster in some games, slower in others.");
			add("Network", "Network", &g_cfg.net.net_active, {"Connected", "Disconnected"}, "Internet access for this game.");
			add("Network", "PSN", &g_cfg.net.psn_status, {}, "RPCN: online play through RPCS3's server (needs an account). Simulated: signed in, offline.");

			// The game's sections in the order of the pause menu, the new ones before Debug.
			std::stable_sort(out.begin(), out.end(), [](const setting_item& a, const setting_item& b)
			{
				const auto rank = [](const std::string& sec) { return sec == "Debug" ? 2 : sec == "CPU" || sec == "Network" ? 1 : 0; };
				return rank(a.section) < rank(b.section);
			});

			for (setting_item& item : out)
			{
				item.on_restart = false; // nothing runs: it all applies at the game's next boot
				item.node = find_node(g_game_settings->cfg.get(), node_path(item.node));
			}
			std::erase_if(out, [](const setting_item& item) { return !item.node; });
			return out;
		}

		// Reads the game's own settings (`from_file`: its custom config, when there is one) and builds the copy they apply to.
		void load_game_settings(bool from_file)
		{
			game_settings_state& gs = *g_game_settings;
			gs.own_file = false;
			gs.built_in = false;
			gs.own.reset(YAML::Node(YAML::NodeType::Map));
			fs::file f;
			if (from_file && f.open(gs.path))
			{
				gs.own.reset(load_yaml_map(f.to_string()));
				gs.own_file = true;
			}
			else if (const std::string db = g_emu_callbacks.get_database_config ? g_emu_callbacks.get_database_config(gs.title_id) : std::string(); !db.empty())
			{
				// A custom config replaces the built-in settings at boot, so the first save keeps them in it.
				gs.own.reset(load_yaml_map(db));
				gs.built_in = true;
			}
			gs.cfg = std::make_unique<cfg_root>();
			gs.cfg->from_string(g_cfg.to_string());
			if (gs.own.size() && !gs.cfg->from_string(yaml_text(gs.own)))
				ui_log.error("UI: could not apply the settings of %s", gs.title_id);
			gs.items = make_game_settings();
		}

		void save_game_settings()
		{
			game_settings_state& gs = *g_game_settings;
			const std::string text = gs.own.size() ? yaml_text(gs.own) : std::string();
			gs.built_in = false;
			gs.own_file = !text.empty();
			g_host.run_on_main([path = gs.path, text]()
			{
				if (text.empty())
				{
					if (fs::is_file(path) && !fs::remove_file(path))
						ui_log.error("UI: could not remove %s (%s)", path, fs::g_tls_error);
					return;
				}
				fs::create_path(rpcs3::utils::get_custom_config_dir());
				write_config_file(path, text);
			});
		}
	}

	std::vector<setting_view> get_settings()
	{
		std::vector<setting_view> out;
		for (setting_item& item : xaml_settings())
			out.push_back(to_setting_view(item));
		return out;
	}

	bool open_game_settings(const std::string& path)
	{
		if (g_in_game || !g_ready)
			return false;
		game_entry game;
		{
			std::lock_guard lock(g_data_mutex);
			const auto it = std::find_if(g_games.begin(), g_games.end(), [&](const game_entry& g) { return g.path == path; });
			if (it == g_games.end())
				return false;
			game = *it;
		}
		if (!has_game_config(game.title_id))
			return false;

		g_game_settings = std::make_unique<game_settings_state>();
		g_game_settings->title_id = game.title_id;
		g_game_settings->title = game.title;
		g_game_settings->path = rpcs3::utils::get_custom_config_path(game.title_id);
		load_game_settings(true);
		ui_log.notice("UI: settings of %s (%s)%s", game.title, game.title_id, g_game_settings->own_file ? ", from its custom config" : g_game_settings->built_in ? ", with built-in ones" : "");
		return true;
	}

	void close_game_settings()
	{
		g_game_settings.reset();
	}

	std::string game_settings_title()
	{
		return g_game_settings ? g_game_settings->title : std::string();
	}

	bool game_settings_own()
	{
		return g_game_settings && (g_game_settings->own_file || g_game_settings->built_in);
	}

	bool game_settings_saved()
	{
		return g_game_settings && g_game_settings->own_file;
	}

	std::vector<setting_view> get_game_settings()
	{
		std::vector<setting_view> out;
		if (g_game_settings)
		{
			for (setting_item& item : g_game_settings->items)
				out.push_back(to_setting_view(item));
		}
		return out;
	}

	void set_game_setting(u32 index, u32 value_index)
	{
		if (!g_game_settings || index >= g_game_settings->items.size())
			return;
		game_settings_state& gs = *g_game_settings;
		setting_item& item = gs.items[index];
		const std::vector<std::string> values = setting_values(item);
		if (!item.node || value_index >= values.size() || item.node->to_string() == values[value_index])
			return;
		if (!item.node->from_string(values[value_index]))
			return;

		const std::vector<std::string> names = node_path(item.node);
		const std::string value = item.node->to_string();
		// Only what differs from config.yml (g_cfg out of a game) is the game's own.
		const cfg::_base* global = find_node(&g_cfg, names);
		if (global && global->to_string() == value)
			yaml_remove(gs.own, names, 0);
		else
			yaml_set(gs.own, names, 0, value);
		ui_log.notice("UI: %s setting '%s' = %s", gs.title_id, item.label, value);
		save_game_settings();
	}

	void reset_game_settings()
	{
		if (!g_game_settings)
			return;
		ui_log.notice("UI: %s back to the global settings", g_game_settings->title_id);
		g_game_settings->own.reset(YAML::Node(YAML::NodeType::Map));
		save_game_settings(); // removes the custom config
		load_game_settings(false); // the built-in settings apply again without it
	}

	void set_setting(u32 index, u32 value_index)
	{
		auto& items = xaml_settings();
		if (index >= items.size())
			return;
		setting_item& item = items[index];
		const std::vector<std::string> values = setting_values(item);
		if (!item.node || value_index >= values.size())
			return;
		if (item.on_restart && g_in_game)
		{
			const std::string& shown = item.pending.empty() ? item.node->to_string() : item.pending;
			if (shown != values[value_index] && save_setting_for_next_boot(item.node, values[value_index]))
			{
				ui_log.notice("UI: setting '%s' = %s (applies at the next boot)", item.label, values[value_index]);
				item.pending = item.node->to_string() == values[value_index] ? std::string() : values[value_index];
			}
			return;
		}
		if (item.node->to_string() == values[value_index])
			return;
		if (item.node->from_string(values[value_index]))
		{
			ui_log.notice("UI: setting '%s' = %s", item.label, values[value_index]);
			if (item.on_change)
				item.on_change();
			else
				save_setting(item.node);
		}
	}

	void open_setting(u32 index)
	{
		auto& items = xaml_settings();
		if (index < items.size() && !items[index].node && items[index].on_open && !g_in_game)
			items[index].on_open();
	}

	void pump()
	{
		g_xaml_ui = true;
		std::vector<std::function<void()>> pending;
		{
			std::lock_guard lock(g_data_mutex);
			pending.swap(g_pending_ui);
		}
		for (auto& fn : pending)
			fn();
	}

	std::optional<screen_view> get_screen()
	{
		if (!g_ui || !g_ui->menu)
			return std::nullopt;
		const list_menu& m = *g_ui->menu;
		screen_view v;
		v.title = m.title;
		if (m.header)
			v.header = m.header();
		for (const menu_item& item : m.build())
			v.items.push_back({item.label, item.detail});
		return v;
	}

	void screen_select(u32 index)
	{
		if (!g_ui || !g_ui->menu || g_rpcn_busy)
			return;
		const std::vector<menu_item> items = g_ui->menu->build();
		if (index < items.size() && items[index].on_select)
			items[index].on_select();
	}

	void screen_close()
	{
		if (g_ui && !g_rpcn_busy)
			g_ui->menu.reset();
	}

	std::optional<text_view> get_text_field()
	{
		if (!g_ui || !g_ui->osk)
			return std::nullopt;
		const text_entry& e = *g_ui->osk;
		text_view v;
		v.serial = e.serial;
		v.title = e.title;
		v.prompt = e.prompt;
		v.value = e.value;
		v.password = e.type == text_entry::kind::password;
		v.max_len = static_cast<u32>(e.max_len);
		switch (e.type)
		{
		case text_entry::kind::username: v.input_scope = "Default"; break;
		case text_entry::kind::password: v.input_scope = "Password"; break;
		case text_entry::kind::email: v.input_scope = "EmailSmtpAddress"; break;
		case text_entry::kind::token: v.input_scope = "Default"; break;
		case text_entry::kind::host: v.input_scope = "Url"; break;
		}
		return v;
	}

	std::string text_field_done(const std::string& typed)
	{
		if (!g_ui || !g_ui->osk)
			return {};
		text_entry& e = *g_ui->osk;
		std::string value = typed;
		if (e.type != text_entry::kind::password)
		{
			// The text box takes any character; the fields take what the menu's own keys offer.
			while (!value.empty() && value.back() == ' ')
				value.pop_back();
			while (!value.empty() && value.front() == ' ')
				value.erase(value.begin());
			for (char& c : value)
			{
				if (e.type == text_entry::kind::token && c >= 'a' && c <= 'z')
					c = static_cast<char>(c - 'a' + 'A');
				if (static_cast<u8>(c) < 0x20 || static_cast<u8>(c) >= 0x7f || !osk_accepts_type(e.type, c))
				{
					switch (e.type)
					{
					case text_entry::kind::username: return "Use only letters, digits, - and _.";
					case text_entry::kind::token: return "The token has only letters and digits.";
					default: return "Remove the spaces and special characters.";
					}
				}
			}
		}
		if (value.size() > e.max_len)
			return fmt::format("At most %u characters.", e.max_len);
		if (value.empty() && !e.allow_empty)
			return "Type something first.";
		e.value = std::move(value);
		osk_finish();
		return {};
	}

	void text_field_cancel()
	{
		if (g_ui)
			g_ui->osk.reset();
	}

	bool in_game()
	{
		return g_in_game;
	}

	bool poll_pause_combo()
	{
		static IGamepadStatics* statics = nullptr;
		static bool tried = false;
		static std::chrono::steady_clock::time_point since{};
		static bool fired = false;

		if (!g_in_game)
		{
			since = {};
			fired = false;
			return false;
		}

		if (!tried)
		{
			tried = true;
			HSTRING_HEADER header;
			HSTRING name;
			const wchar_t* cls = L"Windows.Gaming.Input.Gamepad";
			HRESULT hr = WindowsCreateStringReference(cls, static_cast<UINT32>(wcslen(cls)), &header, &name);
			if (SUCCEEDED(hr))
				hr = RoGetActivationFactory(name, IID_IGamepadStatics, reinterpret_cast<void**>(&statics));
			ui_log.notice("UI: Windows.Gaming.Input gamepad statics for the pause menu: 0x%08x", static_cast<u32>(hr));
		}

		u32 raw = 0;
		IGamepadVectorView* list = nullptr;
		if (statics && SUCCEEDED(statics->get_Gamepads(&list)) && list)
		{
			UINT32 n = 0;
			list->get_Size(&n);
			for (UINT32 i = 0; i < n; i++)
			{
				IGamepad* pad = nullptr;
				if (SUCCEEDED(list->GetAt(i, &pad)) && pad)
				{
					gamepad_reading r{};
					if (SUCCEEDED(pad->GetCurrentReading(&r)))
						raw |= r.buttons;
					pad->Release();
				}
			}
			list->Release();
		}

		if ((raw & (gp_view | gp_menu)) != (gp_view | gp_menu))
		{
			since = {};
			fired = false;
			return false;
		}
		const auto now = std::chrono::steady_clock::now();
		if (since == std::chrono::steady_clock::time_point{})
			since = now;
		if (!fired && now - since > std::chrono::milliseconds(1000))
		{
			fired = true;
			ui_log.notice("UI: View + Menu held, opening the pause menu");
			return true;
		}
		return false;
	}

	void set_paused(bool paused)
	{
		g_host.run_on_main([paused]()
		{
			if (paused)
			{
				if (Emu.IsRunning() && Emu.Pause(false, false))
					g_paused_by_menu = true;
			}
			else if (g_paused_by_menu.exchange(false))
			{
				Emu.Resume();
			}
		});
	}

	// Both 50 Cent runs that came back after 37 s and 55 s in Xbox Home (.76/.77) found the GPU device
	// removed and died with a Vulkan out-of-memory error; the emulator had kept submitting frames into the
	// suspend. Stop it first so no RSX work is in flight while the app is frozen.
	void suspend_game()
	{
		std::shared_ptr<atomic_t<bool>> done = std::make_shared<atomic_t<bool>>(false);
		g_host.run_on_main([done]()
		{
			if (Emu.IsRunning() && Emu.Pause(false, false))
				g_paused_by_suspend = true;
			*done = true;
		});
		for (int i = 0; i < 50 && !*done; i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		ui_log.notice("UWP: suspending (%s)", g_paused_by_suspend ? "game paused" : *done ? "nothing to pause" : "pause still pending");
	}

	void resume_game()
	{
		ui_log.notice("UWP: resuming");
		g_host.run_on_main([]()
		{
			if (g_paused_by_suspend.exchange(false) && Emu.IsPaused())
				Emu.Resume();
		});
	}

	void stop_game()
	{
		g_host.run_on_main([]()
		{
			g_paused_by_menu = false;
			ui_log.notice("UI: stopping the game from the pause menu");
			Emu.GracefulShutdown(false, true);
		});
	}

	bool has_trophies()
	{
		return !current_trophy_set().empty();
	}

	void open_trophies()
	{
		if (g_ui && g_in_game)
			open_trophy_list();
	}

	void open_friends()
	{
		if (!g_ui || !g_in_game)
			return;
		g_ui->menu.reset();
		g_friends = {};
		g_friends_error = "Loading...";
		g_friends_opening = true;
		show_friends_menu();
		reload_friends();
	}

	void save_state()
	{
		g_host.run_on_main([]()
		{
			g_paused_by_menu = false;
			const bool suspend = g_cfg.savestate.suspend_emu.get();
			ui_log.notice("UI: save state from the pause menu (%s)", suspend ? "and quit" : "and go on");
			// As RPCS3's home menu does it: save, then boot the state again and go on playing.
			if (!suspend)
			{
				Emu.after_kill_callback = []() { Emu.Restart(true, false); };
				Emu.SetContinuousMode(true);
			}
			Emu.Kill(false, true);
		});
	}

	bool has_save_states()
	{
		return boot_current_game_savestate(true, 1);
	}

	void open_load_state()
	{
		if (g_ui && g_in_game)
			open_load_state_menu();
	}

	void take_screenshot()
	{
		ui_log.notice("UI: screenshot from the pause menu");
		g_user_asked_for_screenshot = true;
	}

	void restart_game()
	{
		g_host.run_on_main([]()
		{
			g_paused_by_menu = false;
			ui_log.notice("UI: restarting the game from the pause menu");
			// As RPCS3's home menu does it: keep the game screen up across the stop and the new boot.
			Emu.SetContinuousMode(true);
			Emu.Restart(true);
		});
	}
}
