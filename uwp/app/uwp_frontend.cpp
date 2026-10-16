// Controller-driven menu for the RPCS3 UWP shell (no Qt, no XAML).
//
// Everything is drawn on the CPU into a 1920x1080 BGRA canvas with stb_truetype; the shell
// uploads it to its D3D11 swapchain the same way it shows the emulator's frames. Input comes
// from Windows.Gaming.Input (gamepad) and CoreWindow key states (keyboard, for PC testing).
//
// Tabs: Games (boot folders, ISOs, programs), Install (firmware PUP, PKG, RAP), Settings.
// While a game runs, holding View + Menu for a second stops it and returns here.

#include "stdafx.h"
#include "uwp_frontend.h"
#include "uwp_firmware.h"

#include "Emu/System.h"
#include "Emu/system_config.h"
#include "Emu/system_utils.hpp"
#include "Emu/vfs_config.h"
#include "Loader/ISO.h"
#include "Loader/PSF.h"
#include "Crypto/unpkg.h"
#include "Utilities/Thread.h"
#include "Utilities/File.h"
#include "util/sysinfo.hpp"

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
		struct icon_image
		{
			u32 w = 0, h = 0;
			std::vector<u32> px;
		};

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
			cfg::_base* node;
			std::vector<std::string> values; // empty: the node's own list (enums, bools)
			std::string note;
		};

		struct modal
		{
			std::string title;
			std::string text;
			std::string accept_label; // empty: OK only
			std::function<void()> on_accept;
		};

		// ---- State ----
		host_info g_host;
		atomic_t<bool> g_ready{false};
		atomic_t<bool> g_in_game{false};

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

		void set_status(std::string text, u32 color)
		{
			std::lock_guard lock(g_data_mutex);
			g_status_text = std::move(text);
			g_status_color = color;
			g_data_serial++;
		}

		void post_modal(std::string title, std::string text)
		{
			std::lock_guard lock(g_data_mutex);
			g_pending_modal = std::make_unique<modal>(modal{std::move(title), std::move(text), {}, {}});
			g_data_serial++;
		}

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

				if (!readers[0].is_valid())
				{
					error = "The package is damaged or not a PS3 package.";
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

		// ---- Settings ----
		std::vector<setting_item> make_settings()
		{
			return {
				{"Renderer", &g_cfg.video.renderer, {"Vulkan", "Null"}, "Vulkan runs through Mesa Dozen on D3D12. Null draws nothing (CPU tests)."},
				{"Resolution scale", &g_cfg.video.resolution_scale_percent, {"50", "75", "100", "150", "200", "300"}, "Internal rendering resolution in percent of 720p."},
				{"Frame limit", &g_cfg.video.frame_limit, {}, "Auto follows the game's refresh rate."},
				{"VSync", &g_cfg.video.vsync, {}, {}},
				{"Shader mode", &g_cfg.video.shadermode, {}, "Async with interpreter avoids stutter while shaders compile."},
				{"Write color buffers", &g_cfg.video.write_color_buffers, {"false", "true"}, "Needed by some games for correct visuals; costs speed."},
				{"Stats overlay", &g_cfg.video.perf_overlay.enabled, {"false", "true"}, "FPS, CPU and GPU load in the top right corner while a game runs."},
				{"Stats detail", &g_cfg.video.perf_overlay.level, {"Minimal", "Low", "Medium", "High"}, "Minimal = FPS. Low adds CPU. Medium adds PPU/SPU/RSX. High adds GPU load."},
#ifdef LLVM_AVAILABLE
				{"LLVM compile threads", &g_cfg.core.llvm_threads, {"0", "1", "2", "3", "4", "6"}, "0 = automatic. Fewer threads use less memory while compiling."},
#endif
				{"Preferred SPU threads", &g_cfg.core.preferred_spu_threads, {"0", "1", "2", "3", "4", "5", "6"}, "0 = automatic."},
				{"Network", &g_cfg.net.net_active, {"Connected", "Disconnected"}, "Internet access for the XMB and games (browser, updates, online modes)."},
				{"System language", &g_cfg.sys.language, {}, {}},
			};
		}

		void change_setting(setting_item& item, int dir)
		{
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
				save_settings();
			}
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
					ui.dialog.reset();
				}
				return;
			}

			if (g_installing)
				return;

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
				g_select_path = game.path;
				lock.unlock();
				ui_log.notice("UI: boot '%s' (%s)", game.title, game.path);
				// Remembered so the next start of the app selects it again.
				fs::write_file(last_game_file(), fs::rewrite, game.path);

				set_status(fmt::format("Starting %s...", game.title), col_dim);
				g_in_game = true;
				g_host.run_on_main([game]()
				{
					if (!boot(game.path))
					{
						g_in_game = false;
					}
				});
			}
			else if (ui.current == tab::install)
			{
				const install_entry item = g_installs[sel];
				lock.unlock();
				const std::string size = fmt::format("%.1f MB", item.size / 1048576.0);
				if (item.type == install_entry::kind::firmware)
				{
					const std::string installed = utils::get_firmware_version();
					std::string text = fmt::format("Install the PS3 firmware from\n%s (%s)?", item.path, size);
					if (!installed.empty())
						text += fmt::format("\n\nFirmware %s is installed now and will be replaced.", installed);
					ui.dialog = std::make_unique<modal>(modal{"Install firmware", text, "Install", [path = item.path]() { start_firmware_install(path); }});
				}
				else if (item.type == install_entry::kind::package)
				{
					ui.dialog = std::make_unique<modal>(modal{"Install package", fmt::format("Install %s\nfrom %s (%s)?\n\nLicense (.rap) files in the same folder are installed too.", item.title, item.path, size),
						"Install", [path = item.path, title = item.title]() { start_package_install(path, title); }});
				}
				else
				{
					const std::string error = install_license(item.path);
					if (error.empty())
						ui.dialog = std::make_unique<modal>(modal{"License installed", fmt::format("%s is installed. The game it belongs to can now start.", item.path), {}, {}});
					else
						ui.dialog = std::make_unique<modal>(modal{"License not installed", error, {}, {}});
				}
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

		void draw(canvas& cv)
		{
			ui_state& ui = *g_ui;
			cv.fill(0, 0, canvas_width, canvas_height, col_bg);

			// Header: title, firmware state, tabs.
			cv.fill(0, 0, canvas_width, margin_y + 120, col_header);
			cv.text(margin_x, margin_y, "RPCS3", 48, col_text);
			cv.text(margin_x + cv.text_width("RPCS3 ", 48), margin_y + 14, "for Xbox  (unofficial test build)", 30, col_dim);

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
					std::string value = s.node->to_string();
					if (value == "true" || value == "false")
						value = value == "true" ? "On" : "Off";
					value = "<  " + value + "  >";
					const int vw = cv.text_width(value, 34);
					cv.text(canvas_width - margin_x - 30 - vw, y + 10, value, 34, i == sel ? col_text : col_accent);
				}
				draw_footer(cv, "Left / Right  Change      LB/RB  Tabs      Settings are saved right away");
			}

			// Status line above the footer.
			if (!g_status_text.empty())
				cv.text(margin_x, canvas_height - margin_y - 50 - 60, g_status_text, 28, g_status_color, canvas_width - margin_x * 2);
			lock.unlock();

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
				const std::string buttons = ui.dialog->accept_label.empty() ? std::string("A  OK") : "A  " + ui.dialog->accept_label + "        B  Cancel";
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
		{
			std::lock_guard lock(g_data_mutex);
			if (fs::file f{last_game_file()})
				g_select_path = f.to_string();
			g_select_pending = true;
		}
		g_ready = true;

		// Remote test runs (LocalState/autoboot) boot straight away: don't scan the drives
		// alongside the boot; the menu's refresh button scans later.
		if (!fs::is_file(g_host.local_dir + "autoboot"))
			rescan();
	}

	bool boot(const std::string& path)
	{
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
		if (g_in_game.exchange(false))
		{
			ui_log.notice("UI: emulation stopped, back to the menu");
			set_status("Returned from the game", col_dim);
		}
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
			if (g_pending_modal && !ui.dialog)
			{
				ui.dialog = std::move(g_pending_modal);
				ui.dirty = true;
			}

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
}
