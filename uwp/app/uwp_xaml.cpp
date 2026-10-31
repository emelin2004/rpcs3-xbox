// The UWP XAML front end of the Xbox port (see uwp_xaml.h).
//
// Structure:
//  - small COM helpers (smart pointer, HSTRING, delegates built from lambdas);
//  - a `shell` object, UI thread only, that parses uwp/app/xaml/*.xaml with XamlReader, finds the named
//    elements, fills the game / install / settings lists from the menu model in uwp_frontend, and
//    reacts to the controller through XAML's own focus navigation (D-pad, A, LB/RB on the Pivot);
//  - the Application subclass (composed with Windows.UI.Xaml.Application) whose OnLaunched builds all of it.
// A 33 ms DispatcherTimer pulls changes from the model (scan results, install progress, dialogs from
// worker threads, boot progress) so no other thread touches XAML objects.

#include "stdafx.h"
#include "uwp_xaml.h"
#include "uwp_frontend.h"
#include "uwp_update.h"
#include "winrt_xaml_abi.h"

#include <dxgi1_3.h>

#include <atomic>
#include <deque>
#include <optional>
#include <unordered_map>

LOG_CHANNEL(xaml_log, "UWPXAML");

namespace winrt_abi
{
	// {af86e2e0-b12d-4c6a-9c5a-d7aa65101e90}: for com<IInspectable>::as().
	template <>
	struct iid_traits<IInspectable>
	{
		static constexpr GUID value = {0xaf86e2e0, 0xb12d, 0x4c6a, {0x9c, 0x5a, 0xd7, 0xaa, 0x65, 0x10, 0x1e, 0x90}};
	};
}

namespace uwp_xaml
{
	namespace
	{
		using namespace winrt_abi;

#include "uwp_xaml_pages.inc"

		// {f92f19d2-3ade-45a6-a20c-f6f1ea90554b} (windows.ui.xaml.media.dxinterop.h)
		struct ISwapChainPanelNative : IUnknown
		{
			virtual HRESULT STDMETHODCALLTYPE SetSwapChain(IDXGISwapChain* swap_chain) = 0;
		};
		constexpr GUID IID_ISwapChainPanelNative{0xf92f19d2, 0x3ade, 0x45a6, {0xa2, 0x0c, 0xf6, 0xf1, 0xea, 0x90, 0x55, 0x4b}};

		// {905a0fef-bc53-11df-8c49-001e4fc686da} (robuffer.h): raw access to a Windows.Storage.Streams.IBuffer.
		struct IBufferByteAccess : IUnknown
		{
			virtual HRESULT STDMETHODCALLTYPE Buffer(BYTE** value) = 0;
		};
		constexpr GUID IID_IBufferByteAccess{0x905a0fef, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};

		// Windows.UI.Xaml.Markup.IXamlMetadataProvider with its whole signature (the generated header can't
		// express the array that GetXmlnsDefinitions returns). XAML asks the Application for it to resolve
		// the types that aren't Windows.UI.Xaml's own: here those of WinUI 2 (Microsoft.UI.Xaml).
		struct xaml_metadata_provider : IInspectable
		{
			virtual HRESULT STDMETHODCALLTYPE GetXamlType(TypeName type, IInspectable** result) = 0;
			virtual HRESULT STDMETHODCALLTYPE GetXamlTypeByFullName(HSTRING full_name, IInspectable** result) = 0;
			virtual HRESULT STDMETHODCALLTYPE GetXmlnsDefinitions(UINT32* count, void** definitions) = 0;
		};

		// IVector<T> (Items, Children) up to Clear; the elements are passed as interface pointers
		// (IInspectable for Items, IUIElement for Children).
		struct IVectorObject : IInspectable
		{
			virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32 index, IInspectable** item) = 0;
			virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32* size) = 0;
			virtual HRESULT STDMETHODCALLTYPE GetView(void** view) = 0;
			virtual HRESULT STDMETHODCALLTYPE IndexOf(IInspectable* value, UINT32* index, boolean* found) = 0;
			virtual HRESULT STDMETHODCALLTYPE SetAt(UINT32 index, IInspectable* value) = 0;
			virtual HRESULT STDMETHODCALLTYPE InsertAt(UINT32 index, IInspectable* value) = 0;
			virtual HRESULT STDMETHODCALLTYPE RemoveAt(UINT32 index) = 0;
			virtual HRESULT STDMETHODCALLTYPE Append(IInspectable* value) = 0;
			virtual HRESULT STDMETHODCALLTYPE RemoveAtEnd() = 0;
			virtual HRESULT STDMETHODCALLTYPE Clear() = 0;
		};

		// ---- COM / WinRT helpers ----
		template <class T>
		class com
		{
			T* m_ptr = nullptr;

		public:
			com() = default;
			explicit com(T* owned) : m_ptr(owned) {}
			com(const com& other) : m_ptr(other.m_ptr)
			{
				if (m_ptr)
					m_ptr->AddRef();
			}
			com(com&& other) noexcept : m_ptr(other.m_ptr) { other.m_ptr = nullptr; }
			com& operator=(com other) noexcept
			{
				std::swap(m_ptr, other.m_ptr);
				return *this;
			}
			~com() { reset(); }

			void reset()
			{
				if (m_ptr)
				{
					m_ptr->Release();
					m_ptr = nullptr;
				}
			}
			T* get() const { return m_ptr; }
			T* operator->() const { return m_ptr; }
			explicit operator bool() const { return m_ptr != nullptr; }
			// For out parameters.
			T** put()
			{
				reset();
				return &m_ptr;
			}

			template <class U>
			com<U> as() const
			{
				U* out = nullptr;
				if (m_ptr)
					m_ptr->QueryInterface(iid_v<U>, reinterpret_cast<void**>(&out));
				return com<U>(out);
			}
		};

		std::wstring widen(std::string_view s)
		{
			std::wstring out;
			if (s.empty())
				return out;
			const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
			out.resize(n);
			MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
			return out;
		}

		std::string narrow(std::wstring_view s)
		{
			std::string out;
			if (s.empty())
				return out;
			const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
			out.resize(n);
			WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
			return out;
		}

		// Owning HSTRING; a null handle is the empty string.
		class hstr
		{
			HSTRING m_h = nullptr;

		public:
			hstr() = default;
			explicit hstr(std::wstring_view s)
			{
				if (!s.empty())
					WindowsCreateString(s.data(), static_cast<UINT32>(s.size()), &m_h);
			}
			explicit hstr(std::string_view utf8) : hstr(std::wstring_view(widen(utf8))) {}
			hstr(const hstr&) = delete;
			hstr& operator=(const hstr&) = delete;
			~hstr() { WindowsDeleteString(m_h); }
			HSTRING get() const { return m_h; }
		};

		template <class T>
		com<T> get_factory(const wchar_t* class_name)
		{
			HSTRING_HEADER header;
			HSTRING name = nullptr;
			if (FAILED(WindowsCreateStringReference(class_name, static_cast<UINT32>(wcslen(class_name)), &header, &name)))
				return {};
			T* out = nullptr;
			const HRESULT hr = RoGetActivationFactory(name, iid_v<T>, reinterpret_cast<void**>(&out));
			if (FAILED(hr))
				xaml_log.error("RoGetActivationFactory(%s): 0x%08x", narrow(class_name), static_cast<u32>(hr));
			return com<T>(out);
		}

		com<IInspectable> activate(const wchar_t* class_name)
		{
			HSTRING_HEADER header;
			HSTRING name = nullptr;
			if (FAILED(WindowsCreateStringReference(class_name, static_cast<UINT32>(wcslen(class_name)), &header, &name)))
				return {};
			IInspectable* out = nullptr;
			const HRESULT hr = RoActivateInstance(name, &out);
			if (FAILED(hr))
				xaml_log.error("RoActivateInstance(%s): 0x%08x", narrow(class_name), static_cast<u32>(hr));
			return com<IInspectable>(out);
		}

		bool same_object(IUnknown* a, IUnknown* b)
		{
			if (!a || !b)
				return false;
			IUnknown* ua = nullptr;
			IUnknown* ub = nullptr;
			a->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&ua));
			b->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&ub));
			const bool same = ua && ua == ub;
			if (ua)
				ua->Release();
			if (ub)
				ub->Release();
			return same;
		}

		// A delegate (event handler) object around a lambda. Invoked on the thread that registered it.
		template <class Delegate, class... Args>
		struct handler final : Delegate
		{
			LONG refs = 1;
			std::function<void(Args...)> fn;

			explicit handler(std::function<void(Args...)> f) : fn(std::move(f)) {}

			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
			{
				if (riid == __uuidof(IUnknown) || riid == iid_v<Delegate> || riid == __uuidof(IAgileObject))
				{
					*out = static_cast<Delegate*>(this);
					AddRef();
					return S_OK;
				}
				*out = nullptr;
				return E_NOINTERFACE;
			}
			ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
			ULONG STDMETHODCALLTYPE Release() override
			{
				const LONG r = InterlockedDecrement(&refs);
				if (!r)
					delete this;
				return r;
			}
			HRESULT STDMETHODCALLTYPE Invoke(Args... args) override
			{
				fn(args...);
				return S_OK;
			}
		};

		using routed_handler = handler<RoutedEventHandler, IInspectable*, IRoutedEventArgs*>;
		using key_handler = handler<KeyEventHandler, IInspectable*, IKeyRoutedEventArgs*>;
		using selection_handler = handler<SelectionChangedEventHandler, IInspectable*, ISelectionChangedEventArgs*>;
		using click_handler = handler<ItemClickEventHandler, IInspectable*, IItemClickEventArgs*>;
		using tick_handler = handler<IEventHandlerObject, IInspectable*, IInspectable*>;
		using back_handler = handler<IEventHandlerBackRequested, IInspectable*, IBackRequestedEventArgs*>;
		using suspending_handler = handler<SuspendingEventHandler, IInspectable*, IInspectable*>;

		template <class H, class F>
		com<H> make_handler(F&& f)
		{
			return com<H>(new H(std::forward<F>(f)));
		}

		// ---- Markup helpers ----
		com<IInspectable> load_xaml(std::wstring_view markup, const char* what)
		{
			static com<IXamlReaderStatics> reader = get_factory<IXamlReaderStatics>(L"Windows.UI.Xaml.Markup.XamlReader");
			if (!reader)
				return {};
			const hstr text(markup);
			IInspectable* out = nullptr;
			const HRESULT hr = reader->Load(text.get(), &out);
			if (FAILED(hr) || !out)
			{
				xaml_log.error("XamlReader.Load(%s) failed: 0x%08x", what, static_cast<u32>(hr));
				return {};
			}
			return com<IInspectable>(out);
		}

		// Text for XAML element content (not attribute values: those would treat a leading '{' as a markup extension).
		std::wstring xml_text(std::string_view utf8)
		{
			std::wstring out;
			for (const wchar_t c : widen(utf8))
			{
				switch (c)
				{
				case L'&': out += L"&amp;"; break;
				case L'<': out += L"&lt;"; break;
				case L'>': out += L"&gt;"; break;
				case L'\n': case L'\r': out += L' '; break;
				default:
					if ((c >= 0x20 || c == L'\t') && c != 0xFFFE && c != 0xFFFF)
						out += c;
					break;
				}
			}
			return out;
		}

		void replace_all(std::wstring& s, std::wstring_view token, std::wstring_view value)
		{
			for (usz pos = s.find(token); pos != std::wstring::npos; pos = s.find(token, pos + value.size()))
				s.replace(pos, token.size(), value);
		}

		template <class T>
		com<T> find_el(const com<IFrameworkElement>& scope, const wchar_t* name)
		{
			if (!scope)
				return {};
			const hstr n{std::wstring_view(name)};
			IInspectable* obj = nullptr;
			if (FAILED(scope->FindName(n.get(), &obj)) || !obj)
			{
				xaml_log.error("XAML element '%s' not found", narrow(name));
				return {};
			}
			com<IInspectable> owned(obj);
			com<T> out = owned.as<T>();
			if (!out)
				xaml_log.error("XAML element '%s' has an unexpected type", narrow(name));
			return out;
		}

		// Takes the HSTRING (a WinRT out parameter) and returns it as UTF-8.
		std::string take_string(HSTRING h)
		{
			UINT32 n = 0;
			const wchar_t* p = WindowsGetStringRawBuffer(h, &n);
			std::string out = p ? narrow(std::wstring_view(p, n)) : std::string();
			WindowsDeleteString(h);
			return out;
		}

		void set_text(const com<ITextBlock>& tb, std::string_view utf8)
		{
			if (tb)
			{
				const hstr s(utf8);
				tb->put_Text(s.get());
			}
		}

		void set_visible(const com<IUIElement>& e, bool visible)
		{
			if (e)
				e->put_Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
		}

		void set_enabled(const com<IControl>& c, bool enabled)
		{
			if (c)
				c->put_IsEnabled(enabled);
		}

		com<IInspectable> box_string(std::string_view utf8)
		{
			static com<IPropertyValueStatics> values = get_factory<IPropertyValueStatics>(L"Windows.Foundation.PropertyValue");
			IInspectable* out = nullptr;
			if (values)
			{
				const hstr s(utf8);
				values->CreateString(s.get(), &out);
			}
			return com<IInspectable>(out);
		}

		void set_button_text(const com<IContentControl>& b, std::string_view utf8)
		{
			if (b)
			{
				const com<IInspectable> boxed = box_string(utf8);
				b->put_Content(boxed.get());
			}
		}

		// The IVector<T> behind a collection property. The property's own type is a runtime class whose
		// default interface isn't necessarily IVector (ItemCollection's is IObservableVector), so ask for it.
		com<IVectorObject> vector_of(IInspectable* collection, const GUID& iid)
		{
			if (!collection)
				return {};
			const com<IInspectable> owned(collection);
			IVectorObject* out = nullptr;
			if (FAILED(owned->QueryInterface(iid, reinterpret_cast<void**>(&out))))
				xaml_log.error("A collection doesn't implement IVector");
			return com<IVectorObject>(out);
		}

		com<IVectorObject> items_of(const com<IItemsControl>& list)
		{
			IInspectable* out = nullptr;
			if (list)
				list->get_Items(&out);
			return vector_of(out, IID_IVectorObject);
		}

		com<IVectorObject> children_of(const com<IPanel>& panel)
		{
			IInspectable* out = nullptr;
			if (panel)
				panel->get_Children(&out);
			return vector_of(out, IID_IVectorUIElement);
		}

		// ---- Game icons ----
		// A WriteableBitmap with the icon's pixels (premultiplied alpha, which is what XAML expects).
		com<IImageSource> make_bitmap(const uwp_frontend::icon_data& icon)
		{
			static com<IWriteableBitmapFactory> factory = get_factory<IWriteableBitmapFactory>(L"Windows.UI.Xaml.Media.Imaging.WriteableBitmap");
			if (!factory || !icon.w || !icon.h || icon.px.size() < usz{icon.w} * icon.h)
				return {};

			com<IWriteableBitmap> bitmap;
			if (FAILED(factory->CreateInstanceWithDimensions(static_cast<int32_t>(icon.w), static_cast<int32_t>(icon.h), bitmap.put())) || !bitmap)
				return {};

			com<IBuffer> buffer;
			bitmap->get_PixelBuffer(buffer.put());
			IBufferByteAccess* access = nullptr;
			if (!buffer || FAILED(buffer->QueryInterface(IID_IBufferByteAccess, reinterpret_cast<void**>(&access))) || !access)
				return {};
			BYTE* dst = nullptr;
			const HRESULT hr = access->Buffer(&dst);
			access->Release();
			UINT32 capacity = 0;
			buffer->get_Capacity(&capacity);
			if (FAILED(hr) || !dst || capacity < usz{icon.w} * icon.h * 4)
				return {};

			for (usz i = 0; i < usz{icon.w} * icon.h; i++)
			{
				const u32 px = icon.px[i];
				const u32 a = px >> 24;
				dst[i * 4 + 0] = static_cast<BYTE>((px & 0xff) * a / 255);
				dst[i * 4 + 1] = static_cast<BYTE>(((px >> 8) & 0xff) * a / 255);
				dst[i * 4 + 2] = static_cast<BYTE>(((px >> 16) & 0xff) * a / 255);
				dst[i * 4 + 3] = static_cast<BYTE>(a);
			}
			bitmap->Invalidate();
			return bitmap.as<IImageSource>();
		}

		std::atomic<bool> g_pause_requested{false};

		// ---- The shell ----
		struct shell
		{
			host h;

			// Tree.
			com<IFrameworkElement> root;
			// The game picture's swap chain and the panel size (DIPs) its transform was set for.
			IDXGISwapChain2* game_sc = nullptr;
			double game_panel_w = 0, game_panel_h = 0;
			com<IUIElement> game_panel, menu_layer, loading_layer, loading_backdrop, job_layer, dialog_layer, pause_layer, error_layer;
			com<IControl> pivot_ctl, pause_scroll_ctl;
			com<IPivot> pivot;
			com<ISelector> games_sel, install_sel;
			com<IItemsControl> games_items, install_items;
			com<IListViewBase> games_lv, install_lv;
			com<IPanel> settings_panel, pause_settings_panel;
			com<IUIElement> games_list_el, games_empty_el, details_el, install_list_el, install_empty_el, usb_hint_el, update_el, games_busy_el, install_busy_el;
			com<ITextBlock> t_firmware, t_version, t_update, t_telemetry, t_status, t_games_empty, t_install_empty;
			com<ITextBlock> t_detail_title, t_detail_id, t_detail_kind, t_detail_version, t_detail_source, t_detail_placeholder;
			com<IImage> detail_icon;
			com<ITextBlock> t_loading_title, t_loading_detail, t_job_title, t_job_detail, t_dialog_title, t_dialog_text, t_error;
			com<IRangeBase> loading_bar_range, job_bar_range;
			com<IProgressBar> loading_bar, job_bar;
			com<IContentControl> dialog_accept, dialog_cancel;
			com<IUIElement> screen_layer, text_layer, text_error_el;
			com<IControl> screen_scroll_ctl, text_ok_ctl, text_cancel_ctl;
			com<IPanel> screen_items_panel, text_field_host;
			com<ITextBlock> t_screen_title, t_screen_header, t_text_title, t_text_prompt, t_text_error;
			com<ITextBlock> t_pause_title, t_pause_subtitle;
			com<IBrush> brush[4];

			// Model mirror.
			uwp_frontend::snapshot snap;
			u64 seen_serial = 0;
			std::string games_sig, installs_sig;
			std::vector<com<IInspectable>> game_items, install_items_v;
			std::unordered_map<const uwp_frontend::icon_data*, com<IImageSource>> icon_cache;
			std::string selected_game_path;
			bool initial_select_done = false;
			bool rebuilding = false;
			bool settings_built = false;
			com<IControl> first_setting;

			// State.
			bool menu_visible = true;
			bool pause_open = false;
			// The pause pane, opened from the Games tab with Menu, showing one game's own settings (no game runs).
			bool game_cfg_open = false;
			u64 game_cfg_serial = 0; // uwp_frontend::game_settings_serial() the page was built at
			std::optional<u32> game_cfg_row; // the row last used on the game's page, focused again when it is rebuilt
			bool game_cfg_rebuild = false;   // show_pause_page() builds the same page again
			bool job_visible = false;
			std::optional<uwp_frontend::dialog_view> dialog;
			std::deque<uwp_frontend::dialog_view> dialog_queue;
			com<IInspectable> dialog_return_focus;
			com<IInspectable> pending_focus;
			int focus_tries = 0;
			bool loading_shown = false, error_shown = false;
			std::string loading_title_shown, loading_detail_shown, error_shown_text;
			// The menu logic's own screen (RPCN account) and text field.
			bool screen_open = false;
			std::string screen_sig;
			std::vector<com<IInspectable>> screen_buttons;
			u32 screen_last = 0; // the entry chosen last: it gets the focus back
			u64 text_serial = 0;
			bool text_taken = false; // submitted; the next tick replaces or closes the field
			com<ITextBox> text_box;
			com<IPasswordBox> pass_box;
			com<IControl> text_field_ctl;
			com<IDispatcherTimer> timer;
			bool launched = false;

			// ---- Tree ----
			bool build()
			{
				const com<IInspectable> loaded = load_xaml(k_xaml_shell, "shell");
				root = loaded.as<IFrameworkElement>();
				if (!root)
					return false;

				game_panel = find_el<IUIElement>(root, L"GamePanel");
				menu_layer = find_el<IUIElement>(root, L"MenuLayer");
				loading_layer = find_el<IUIElement>(root, L"LoadingLayer");
				loading_backdrop = find_el<IUIElement>(root, L"LoadingBackdrop");
				job_layer = find_el<IUIElement>(root, L"JobLayer");
				dialog_layer = find_el<IUIElement>(root, L"DialogLayer");
				pause_layer = find_el<IUIElement>(root, L"PauseLayer");
				error_layer = find_el<IUIElement>(root, L"ErrorLayer");
				pivot = find_el<IPivot>(root, L"MenuPivot");
				pivot_ctl = find_el<IControl>(root, L"MenuPivot");
				pause_scroll_ctl = find_el<IControl>(root, L"PauseScroll");

				games_sel = find_el<ISelector>(root, L"GamesList");
				games_items = find_el<IItemsControl>(root, L"GamesList");
				games_lv = find_el<IListViewBase>(root, L"GamesList");
				games_list_el = find_el<IUIElement>(root, L"GamesList");
				install_sel = find_el<ISelector>(root, L"InstallList");
				install_items = find_el<IItemsControl>(root, L"InstallList");
				install_lv = find_el<IListViewBase>(root, L"InstallList");
				install_list_el = find_el<IUIElement>(root, L"InstallList");
				settings_panel = find_el<IPanel>(root, L"SettingsStack");
				pause_settings_panel = find_el<IPanel>(root, L"PauseSettingsStack");

				games_empty_el = find_el<IUIElement>(root, L"GamesEmpty");
				t_games_empty = find_el<ITextBlock>(root, L"GamesEmpty");
				details_el = find_el<IUIElement>(root, L"DetailsPanel");
				install_empty_el = find_el<IUIElement>(root, L"InstallEmpty");
				t_install_empty = find_el<ITextBlock>(root, L"InstallEmpty");
				usb_hint_el = find_el<IUIElement>(root, L"UsbHint");
				games_busy_el = find_el<IUIElement>(root, L"GamesBusy");
				install_busy_el = find_el<IUIElement>(root, L"InstallBusy");
				t_firmware = find_el<ITextBlock>(root, L"FirmwareText");
				t_version = find_el<ITextBlock>(root, L"VersionText");
				t_update = find_el<ITextBlock>(root, L"UpdateText");
				update_el = find_el<IUIElement>(root, L"UpdateText");
				t_telemetry = find_el<ITextBlock>(root, L"TelemetryIdText");
				t_status = find_el<ITextBlock>(root, L"StatusText");
				t_detail_title = find_el<ITextBlock>(root, L"DetailTitle");
				t_detail_id = find_el<ITextBlock>(root, L"DetailId");
				t_detail_kind = find_el<ITextBlock>(root, L"DetailKind");
				t_detail_version = find_el<ITextBlock>(root, L"DetailVersion");
				t_detail_source = find_el<ITextBlock>(root, L"DetailSource");
				t_detail_placeholder = find_el<ITextBlock>(root, L"DetailPlaceholder");
				detail_icon = find_el<IImage>(root, L"DetailIcon");

				t_loading_title = find_el<ITextBlock>(root, L"LoadingTitle");
				t_loading_detail = find_el<ITextBlock>(root, L"LoadingDetail");
				loading_bar_range = find_el<IRangeBase>(root, L"LoadingBar");
				loading_bar = find_el<IProgressBar>(root, L"LoadingBar");
				t_job_title = find_el<ITextBlock>(root, L"JobTitle");
				t_job_detail = find_el<ITextBlock>(root, L"JobDetail");
				job_bar_range = find_el<IRangeBase>(root, L"JobBar");
				job_bar = find_el<IProgressBar>(root, L"JobBar");
				t_dialog_title = find_el<ITextBlock>(root, L"DialogTitle");
				t_dialog_text = find_el<ITextBlock>(root, L"DialogText");
				dialog_accept = find_el<IContentControl>(root, L"DialogAccept");
				dialog_cancel = find_el<IContentControl>(root, L"DialogCancel");
				t_error = find_el<ITextBlock>(root, L"ErrorText");

				screen_layer = find_el<IUIElement>(root, L"ScreenLayer");
				screen_scroll_ctl = find_el<IControl>(root, L"ScreenScroll");
				screen_items_panel = find_el<IPanel>(root, L"ScreenItems");
				t_screen_title = find_el<ITextBlock>(root, L"ScreenTitle");
				t_pause_title = find_el<ITextBlock>(root, L"PauseTitle");
				t_pause_subtitle = find_el<ITextBlock>(root, L"PauseSubtitle");
				t_screen_header = find_el<ITextBlock>(root, L"ScreenHeader");
				text_layer = find_el<IUIElement>(root, L"TextLayer");
				text_field_host = find_el<IPanel>(root, L"TextFieldHost");
				t_text_title = find_el<ITextBlock>(root, L"TextTitle");
				t_text_prompt = find_el<ITextBlock>(root, L"TextPrompt");
				t_text_error = find_el<ITextBlock>(root, L"TextError");
				text_error_el = find_el<IUIElement>(root, L"TextError");
				text_ok_ctl = find_el<IControl>(root, L"TextOk");
				text_cancel_ctl = find_el<IControl>(root, L"TextCancel");

				if (!menu_layer || !games_sel || !games_items || !install_sel || !install_items || !settings_panel || !pivot || !game_panel)
				{
					xaml_log.error("The shell page is missing elements");
					return false;
				}

				static constexpr const wchar_t* colors[4] = {L"#FF9AA0AB", L"#FF4CC27A", L"#FFE8B04D", L"#FFE85D5D"};
				for (u32 i = 0; i < 4; i++)
					brush[i] = load_xaml(std::wstring(L"<SolidColorBrush xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" Color=\"") + colors[i] + L"\"/>", "brush").as<IBrush>();

				wire();
				return true;
			}

			void wire()
			{
				int64_t token = 0;

				games_sel->add_SelectionChanged(make_handler<selection_handler>([this](IInspectable*, ISelectionChangedEventArgs*) { on_game_selected(); }).get(), &token);
				games_lv->add_ItemClick(make_handler<click_handler>([this](IInspectable*, IItemClickEventArgs* e) { on_game_clicked(e); }).get(), &token);
				games_lv->put_IsItemClickEnabled(true);
				install_lv->add_ItemClick(make_handler<click_handler>([this](IInspectable*, IItemClickEventArgs* e) { on_install_clicked(e); }).get(), &token);
				install_lv->put_IsItemClickEnabled(true);

				pivot->add_SelectionChanged(make_handler<selection_handler>([this](IInspectable*, ISelectionChangedEventArgs*)
				{
					if (menu_visible && !modal_open())
						focus_menu();
				}).get(), &token);

				if (const auto accept = dialog_accept.as<IButtonBase>())
					accept->add_Click(make_handler<routed_handler>([this](IInspectable*, IRoutedEventArgs*) { close_dialog(true); }).get(), &token);
				if (const auto cancel = dialog_cancel.as<IButtonBase>())
					cancel->add_Click(make_handler<routed_handler>([this](IInspectable*, IRoutedEventArgs*) { close_dialog(false); }).get(), &token);
				if (const auto ok = find_el<IButtonBase>(root, L"TextOk"))
					ok->add_Click(make_handler<routed_handler>([this](IInspectable*, IRoutedEventArgs*) { submit_text(); }).get(), &token);
				if (const auto cancel = find_el<IButtonBase>(root, L"TextCancel"))
					cancel->add_Click(make_handler<routed_handler>([this](IInspectable*, IRoutedEventArgs*) { cancel_text(); }).get(), &token);
				if (const auto resume = find_el<IButtonBase>(root, L"PauseResume"))
					resume->add_Click(make_handler<routed_handler>([this](IInspectable*, IRoutedEventArgs*) { close_pause(); }).get(), &token);
				if (const auto restart = find_el<IButtonBase>(root, L"PauseRestart"))
				{
					restart->add_Click(make_handler<routed_handler>([this](IInspectable*, IRoutedEventArgs*)
					{
						show_dialog({"Restart the game?", "The game starts again from the beginning. Progress that wasn't saved in the game is lost.", "Restart", [this]()
						{
							after_restart_chosen();
						}});
					}).get(), &token);
				}
				const auto on_click = [&](const wchar_t* name, std::function<void()> fn)
				{
					if (const auto button = find_el<IButtonBase>(root, name))
						button->add_Click(make_handler<routed_handler>([fn = std::move(fn)](IInspectable*, IRoutedEventArgs*) { fn(); }).get(), &token);
				};
				// The rest of RPCS3's home menu: lists open as screens over the pause menu (B goes back to it).
				on_click(L"PauseSettings", [this]() { show_pause_page(pause_page::groups); });
				on_click(L"PauseTrophies", [this]() { screen_last = 0; uwp_frontend::open_trophies(); });
				on_click(L"PauseFriends", [this]() { screen_last = 0; uwp_frontend::open_friends(); });
				on_click(L"PauseLoadState", [this]() { screen_last = 0; uwp_frontend::open_load_state(); });
				on_click(L"PauseScreenshot", [this]()
				{
					// Taken from the next frame the game draws, after the pause menu closes (it isn't in it).
					uwp_frontend::take_screenshot();
					close_pause();
				});
				on_click(L"PauseSaveState", [this]()
				{
					show_dialog({"Save state?", "Saves the whole game as it is now, to load it again later from this menu. The game restarts from the state right away. Save states don't work in every game; the game's own saves are safer.", "Save state", [this]()
					{
						pause_open = false;
						set_visible(pause_layer, false);
						uwp_frontend::screen_close();
						update_gating();
						pending_focus.reset();
						uwp_frontend::save_state();
					}});
				});
				if (const auto stop = find_el<IButtonBase>(root, L"PauseStop"))
				{
					stop->add_Click(make_handler<routed_handler>([this](IInspectable*, IRoutedEventArgs*)
					{
						show_dialog({"Stop the game?", "The game is closed and you return to the app menu. Progress that wasn't saved in the game is lost.", "Stop game", [this]()
						{
							after_stop_chosen();
						}});
					}).get(), &token);
				}

				// Y refreshes the lists (F5 on a keyboard).
				if (const auto root_el = root.as<IUIElement>())
				{
					root_el->add_KeyDown(make_handler<key_handler>([this](IInspectable*, IKeyRoutedEventArgs* e)
					{
						VirtualKey key{};
						if (!e || FAILED(e->get_Key(&key)) || !menu_visible || modal_open())
							return;
						if (key == VirtualKey::GamepadY || key == VirtualKey::F5)
							uwp_frontend::request_rescan();
						// Menu on the Games tab: the selected game's own settings (F2 on a keyboard).
						else if (key == VirtualKey::GamepadMenu || key == VirtualKey::F2)
						{
							int32_t page = -1;
							pivot->get_SelectedIndex(&page);
							if (page == 0)
							{
								e->put_Handled(true);
								open_game_cfg();
							}
						}
					}).get(), &token);
				}
			}

			// ---- Focus ----
			void request_focus(const com<IInspectable>& target)
			{
				pending_focus = target;
				focus_tries = 0;
			}

			void service_focus()
			{
				if (!pending_focus)
					return;
				boolean ok = false;
				if (const auto ctl = pending_focus.as<IControl>())
					ctl->Focus(FocusState::Programmatic, &ok);
				if (ok || ++focus_tries > 30)
					pending_focus.reset();
			}

			com<IInspectable> focused_element()
			{
				static com<IFocusManagerStatics> fm = get_factory<IFocusManagerStatics>(L"Windows.UI.Xaml.Input.FocusManager");
				IInspectable* out = nullptr;
				if (fm)
					fm->GetFocusedElement(&out);
				return com<IInspectable>(out);
			}

			// Puts the focus on the page the menu shows: the selected game or install entry, or the first setting.
			void focus_menu()
			{
				int32_t page = 0;
				pivot->get_SelectedIndex(&page);
				com<IInspectable> target;
				if (page == 0)
				{
					int32_t idx = -1;
					games_sel->get_SelectedIndex(&idx);
					if (idx >= 0 && idx < static_cast<int32_t>(game_items.size()))
						target = game_items[idx];
				}
				else if (page == 1)
				{
					int32_t idx = -1;
					install_sel->get_SelectedIndex(&idx);
					if (idx >= 0 && idx < static_cast<int32_t>(install_items_v.size()))
						target = install_items_v[idx];
				}
				else if (first_setting)
				{
					target = first_setting.as<IInspectable>();
				}
				if (!target)
					target = pivot_ctl.as<IInspectable>();
				request_focus(target);
			}

			bool text_open() const { return text_serial != 0; }

			bool modal_open() const { return dialog.has_value() || job_visible || pause_open || game_cfg_open || loading_shown || error_shown || screen_open || text_open(); }

			void update_gating()
			{
				// Controls behind an overlay must not take the focus: disabled controls can't have it.
				set_enabled(pivot_ctl, menu_visible && !dialog && !job_visible && !pause_open && !game_cfg_open && !loading_shown && !error_shown && !screen_open && !text_open());
				set_enabled(pause_scroll_ctl, !dialog && !screen_open && !text_open());
				set_enabled(screen_scroll_ctl, !dialog && !job_visible && !text_open());
				const bool text_enabled = !dialog && !job_visible;
				set_enabled(text_field_ctl, text_enabled);
				set_enabled(text_ok_ctl, text_enabled);
				set_enabled(text_cancel_ctl, text_enabled);
			}

			// Focus for the top layer once an overlay over it has gone: the text field, the screen, or the menu.
			void focus_top()
			{
				if (text_open())
					request_focus(text_field_ctl.as<IInspectable>());
				else if (screen_open && !screen_buttons.empty())
					request_focus(screen_buttons[std::min<usz>(screen_last, screen_buttons.size() - 1)]);
				else if (pause_open)
					request_focus(find_el<IInspectable>(root, L"PauseResume"));
				else if (game_cfg_open)
					show_pause_page(pause_at, pause_group);
				else if (menu_visible)
					focus_menu();
			}

			// ---- Lists ----
			std::string subtitle(const uwp_frontend::game_view& g)
			{
				std::string sub = g.title_id.empty() || g.title_id == "?" ? g.kind : g.title_id + "  ·  " + g.kind;
				if (!g.source.empty())
					sub += "  ·  " + g.source;
				return sub;
			}

			static std::string placeholder_label(const uwp_frontend::game_view& g)
			{
				return g.kind == "System software" ? "XMB" : g.kind == "Disc image (ISO)" ? "ISO" : "ELF";
			}

			com<IImageSource> icon_source(const std::shared_ptr<const uwp_frontend::icon_data>& icon)
			{
				if (!icon)
					return {};
				if (const auto it = icon_cache.find(icon.get()); it != icon_cache.end())
					return it->second;
				com<IImageSource> src = make_bitmap(*icon);
				icon_cache[icon.get()] = src;
				return src;
			}

			com<IInspectable> make_game_item(const uwp_frontend::game_view& g)
			{
				std::wstring markup = k_xaml_game_item;
				replace_all(markup, L"$TITLE$", xml_text(g.title));
				replace_all(markup, L"$SUBTITLE$", xml_text(subtitle(g)));
				replace_all(markup, L"$PLACEHOLDER$", g.icon ? std::wstring() : xml_text(placeholder_label(g)));
				com<IInspectable> item = load_xaml(markup, "game item");
				if (item && g.icon)
				{
					const auto fe = item.as<IFrameworkElement>();
					const auto img = find_el<IImage>(fe, L"Icon");
					if (const auto src = icon_source(g.icon); src && img)
						img->put_Source(src.get());
				}
				return item;
			}

			com<IInspectable> make_install_item(const uwp_frontend::install_view& i)
			{
				std::wstring markup = k_xaml_install_item;
				replace_all(markup, L"$TITLE$", xml_text(i.title));
				replace_all(markup, L"$SUBTITLE$", xml_text(fmt::format("%s  ·  %.1f MB  ·  %s", i.kind, i.size / 1048576.0, i.path)));
				return load_xaml(markup, "install item");
			}

			void update_details(const uwp_frontend::game_view& g)
			{
				set_text(t_detail_title, g.title);
				set_text(t_detail_id, g.title_id == "?" ? std::string() : g.title_id);
				set_text(t_detail_kind, g.kind);
				set_text(t_detail_version, g.version);
				set_text(t_detail_source, g.source);
				const com<IImageSource> src = icon_source(g.icon);
				if (detail_icon)
					detail_icon->put_Source(src.get());
				set_text(t_detail_placeholder, src ? std::string() : placeholder_label(g));
			}

			void on_game_selected()
			{
				if (rebuilding)
					return;
				int32_t idx = -1;
				games_sel->get_SelectedIndex(&idx);
				if (idx >= 0 && idx < static_cast<int32_t>(snap.games.size()))
				{
					selected_game_path = snap.games[idx].path;
					update_details(snap.games[idx]);
				}
			}

			static int index_of(const std::vector<com<IInspectable>>& items, IInspectable* clicked)
			{
				for (usz i = 0; i < items.size(); i++)
				{
					if (same_object(items[i].get(), clicked))
						return static_cast<int>(i);
				}
				return -1;
			}

			void on_game_clicked(IItemClickEventArgs* e)
			{
				int idx = -1;
				IInspectable* clicked = nullptr;
				if (e && SUCCEEDED(e->get_ClickedItem(&clicked)) && clicked)
				{
					idx = index_of(game_items, clicked);
					clicked->Release();
				}
				if (idx < 0)
				{
					int32_t sel = -1;
					games_sel->get_SelectedIndex(&sel);
					idx = sel;
				}
				if (idx >= 0 && idx < static_cast<int>(snap.games.size()))
					uwp_frontend::start_game(snap.games[idx].path);
			}

			void on_install_clicked(IItemClickEventArgs* e)
			{
				int idx = -1;
				IInspectable* clicked = nullptr;
				if (e && SUCCEEDED(e->get_ClickedItem(&clicked)) && clicked)
				{
					idx = index_of(install_items_v, clicked);
					clicked->Release();
				}
				if (idx < 0)
				{
					int32_t sel = -1;
					install_sel->get_SelectedIndex(&sel);
					idx = sel;
				}
				if (idx >= 0 && idx < static_cast<int>(snap.installs.size()))
				{
					if (auto prompt = uwp_frontend::install_prompt(snap.installs[idx].path))
						show_dialog(std::move(*prompt));
				}
			}

			void sync_games()
			{
				std::string sig;
				for (const auto& g : snap.games)
				{
					sig += g.path;
					sig += '\x1f';
					sig += g.title;
					sig += '\x1e';
				}

				if (sig != games_sig)
				{
					games_sig = std::move(sig);
					rebuilding = true;
					const auto items = items_of(games_items);
					if (items)
						items->Clear();
					game_items.clear();

					// Keep only the icons of the listed games.
					std::unordered_map<const uwp_frontend::icon_data*, com<IImageSource>> kept;
					for (const auto& g : snap.games)
					{
						if (g.icon)
						{
							if (const auto it = icon_cache.find(g.icon.get()); it != icon_cache.end())
								kept.emplace(it->first, it->second);
						}
					}
					icon_cache = std::move(kept);

					for (const auto& g : snap.games)
					{
						com<IInspectable> item = make_game_item(g);
						if (item && items)
							items->Append(item.get());
						game_items.push_back(std::move(item));
					}
					rebuilding = false;

					// Select the same game again, else the last played one (first listing only), else the top entry.
					int idx = -1;
					for (usz i = 0; i < snap.games.size() && !selected_game_path.empty(); i++)
					{
						if (snap.games[i].path == selected_game_path)
							idx = static_cast<int>(i);
					}
					const bool first_selection = !initial_select_done;
					if (!initial_select_done)
					{
						for (usz i = 0; i < snap.games.size() && !snap.select_path.empty(); i++)
						{
							if (snap.games[i].path == snap.select_path)
								idx = static_cast<int>(i);
						}
						// Wait for the scan to list the last played game before settling on the top entry.
						if (idx >= 0 || !snap.scanning)
						{
							initial_select_done = true;
							if (idx < 0 && !snap.games.empty())
								idx = 0;
						}
					}
					if (idx >= 0)
					{
						games_sel->put_SelectedIndex(idx);
						if (games_lv && game_items[idx])
							games_lv->ScrollIntoView(game_items[idx].get());
						on_game_selected();
						if (first_selection && menu_visible && !modal_open())
							focus_menu();
					}
				}

				const bool have = !snap.games.empty();
				set_visible(games_list_el, have);
				set_visible(details_el, have);
				// While the first scan runs, a spinning ring stands in for the empty list.
				set_visible(games_busy_el, !have && snap.scanning);
				set_visible(games_empty_el, !have && !snap.scanning);
				if (!have && !snap.scanning)
				{
					set_text(t_games_empty,
						"No games found.\n\n"
						"Copy a game folder (with PS3_GAME inside, or PARAM.SFO + USRDIR) or an .iso into the app's LocalState\\games folder with the Device Portal's File explorer, "
						"or put games in a GAMES folder on a USB drive. Press Y to look again.");
				}
			}

			void sync_installs()
			{
				std::string sig;
				for (const auto& i : snap.installs)
				{
					sig += i.path;
					sig += '\x1e';
				}
				if (sig != installs_sig)
				{
					installs_sig = std::move(sig);
					rebuilding = true;
					const auto items = items_of(install_items);
					if (items)
						items->Clear();
					install_items_v.clear();
					for (const auto& i : snap.installs)
					{
						com<IInspectable> item = make_install_item(i);
						if (item && items)
							items->Append(item.get());
						install_items_v.push_back(std::move(item));
					}
					rebuilding = false;
					if (!snap.installs.empty())
						install_sel->put_SelectedIndex(0);
				}

				const bool have = !snap.installs.empty();
				set_visible(install_list_el, have);
				set_visible(install_busy_el, !have && snap.scanning);
				set_visible(install_empty_el, !have && !snap.scanning);
				if (!have && !snap.scanning)
					set_text(t_install_empty, "Nothing to install found. Press Y to look again.");
			}

			void sync_header()
			{
				set_text(t_firmware, snap.firmware.empty() ? "Firmware: not installed (built-in libraries)" : "Firmware " + snap.firmware);
				set_text(t_telemetry, snap.telemetry_id.empty() ? std::string() : "Anonymous ID " + snap.telemetry_id + (snap.telemetry_on ? "" : " (log sending off)"));
				if (t_firmware)
					t_firmware->put_Foreground(brush[snap.firmware.empty() ? 2 : 1].get());
				set_text(t_version, "Version " + snap.app_version);
				if (t_version)
					t_version->put_Foreground(brush[!snap.update_checked ? 0 : snap.update_outdated ? 2 : 1].get());
				set_text(t_update, snap.update_outdated ? fmt::format("Update %s available:\n%s", snap.update_latest, uwp_update::releases_page) : std::string());
				set_visible(update_el, snap.update_outdated);
				set_visible(usb_hint_el, snap.scanning_usb);
				set_text(t_status, snap.status);
				if (t_status)
					t_status->put_Foreground(brush[static_cast<u32>(snap.status_color)].get());
			}

			// ---- Settings ----
			// One row per setting: a label with its note, and a switch (on / off) or a drop-down list.
			// `per_game`: the settings of the game open with Menu on the Games tab (game_cfg_open).
			// `focus_row`: the setting whose control is handed back in `first` (a page rebuilt under the user's hands).
			void build_settings(const com<IPanel>& panel, bool in_game_only, com<IControl>* first, const std::string* only_section = nullptr, bool per_game = false,
				std::optional<u32> focus_row = std::nullopt)
			{
				const auto children = children_of(panel);
				if (!children)
					return;
				children->Clear();
				if (first)
					first->reset();

				const std::vector<uwp_frontend::setting_view> settings = per_game ? uwp_frontend::get_game_settings() : uwp_frontend::get_settings();
				const auto set = per_game ? &uwp_frontend::set_game_setting : &uwp_frontend::set_setting;
				std::string section;
				for (u32 i = 0; i < settings.size(); i++)
				{
					const auto& s = settings[i];
					if (in_game_only ? !s.in_game : s.pause_only)
						continue;
					if (only_section && s.section != *only_section)
						continue;
					if (in_game_only && !only_section && !s.section.empty() && s.section != section)
					{
						section = s.section;
						std::wstring heading = k_xaml_setting_section;
						replace_all(heading, L"$LABEL$", xml_text(section));
						if (const auto el = load_xaml(heading, "setting section").as<IUIElement>())
							children->Append(el.get());
					}

					std::wstring control;
					if (s.action)
					{
						control = k_xaml_setting_button;
						replace_all(control, L"$VALUE$", xml_text(s.value));
					}
					else if (s.toggle)
					{
						control = k_xaml_setting_toggle;
						replace_all(control, L"$ON$", s.current == 1 ? L"True" : L"False");
					}
					else
					{
						std::wstring items;
						for (const auto& v : s.shown)
							items += L"<ComboBoxItem>" + xml_text(v) + L"</ComboBoxItem>";
						control = k_xaml_setting_combo;
						replace_all(control, L"$SELECTED$", std::to_wstring(s.current));
						replace_all(control, L"$ITEMS$", items);
					}
					std::wstring markup = k_xaml_setting_row;
					replace_all(markup, L"$LABEL$", xml_text(s.label));
					replace_all(markup, L"$NOTE$", xml_text(s.note));
					replace_all(markup, L"$CONTROL$", control);

					const com<IInspectable> row = load_xaml(markup, "setting row");
					const auto row_fe = row.as<IFrameworkElement>();
					const auto row_el = row.as<IUIElement>();
					if (!row_fe || !row_el)
						continue;

					int64_t token = 0;
					if (s.action)
					{
						if (const auto button = find_el<IButtonBase>(row_fe, L"Control"))
						{
							button->add_Click(make_handler<routed_handler>([this, i, per_game](IInspectable*, IRoutedEventArgs*)
							{
								if (per_game)
								{
									// The game's own action rows (patch download): the next rebuild keeps the focus here.
									game_cfg_row = i;
									uwp_frontend::open_game_setting(i);
									return;
								}
								// The next tick shows the screen (not from inside this handler: it rebuilds the page).
								screen_last = 0;
								uwp_frontend::open_setting(i);
							}).get(), &token);
							if (first && (!*first || focus_row == i))
								*first = button.as<IControl>();
						}
					}
					else if (s.toggle)
					{
						if (const auto toggle = find_el<IToggleSwitch>(row_fe, L"Control"))
						{
							toggle->add_Toggled(make_handler<routed_handler>([this, i, set, per_game, toggle = toggle.get()](IInspectable*, IRoutedEventArgs*)
							{
								boolean on = false;
								toggle->get_IsOn(&on);
								if (per_game)
									game_cfg_row = i;
								set(i, on ? 1 : 0);
							}).get(), &token);
							if (first && (!*first || focus_row == i))
								*first = toggle.as<IControl>();
						}
					}
					else if (const auto combo = find_el<ISelector>(row_fe, L"Control"))
					{
						combo->add_SelectionChanged(make_handler<selection_handler>([this, i, set, per_game, combo = combo.get()](IInspectable*, ISelectionChangedEventArgs*)
						{
							int32_t idx = -1;
							combo->get_SelectedIndex(&idx);
							if (per_game)
								game_cfg_row = i;
							if (idx >= 0)
								set(i, static_cast<u32>(idx));
						}).get(), &token);
						if (first && (!*first || focus_row == i))
							*first = combo.as<IControl>();
					}
					children->Append(row_el.get());
				}
			}

			// ---- Dialogs ----
			void show_dialog(uwp_frontend::dialog_view d)
			{
				dialog_queue.push_back(std::move(d));
				show_next_dialog();
			}

			void show_next_dialog()
			{
				if (dialog || dialog_queue.empty())
					return;
				dialog = std::move(dialog_queue.front());
				dialog_queue.pop_front();

				dialog_return_focus = focused_element();
				set_text(t_dialog_title, dialog->title);
				set_text(t_dialog_text, dialog->text);
				const bool confirm = !dialog->accept_label.empty();
				set_button_text(dialog_accept, dialog->accept_label);
				set_button_text(dialog_cancel, !confirm ? "OK" : dialog->cancel_label.empty() ? "Cancel" : dialog->cancel_label);
				if (const auto accept_el = dialog_accept.as<IUIElement>())
					accept_el->put_Visibility(confirm ? Visibility::Visible : Visibility::Collapsed);
				set_visible(dialog_layer, true);
				update_gating();
				request_focus(confirm ? dialog_accept.as<IInspectable>() : dialog_cancel.as<IInspectable>());
			}

			void close_dialog(bool accepted)
			{
				if (!dialog)
					return;
				uwp_frontend::dialog_view d = std::move(*dialog);
				dialog.reset();
				set_visible(dialog_layer, false);
				update_gating();
				if (accepted && d.on_accept)
					d.on_accept();
				else if (!accepted && d.on_cancel)
					d.on_cancel();
				// The first-start telemetry question changes a setting, and the Settings page was built
				// on the first tick, before it was answered.
				if (d.refresh_settings && settings_built)
					build_settings(settings_panel, false, &first_setting);
				if (!dialog_queue.empty())
				{
					show_next_dialog();
					return;
				}
				if (dialog_return_focus && (screen_open || text_open()))
					focus_top(); // the screen may have been rebuilt meanwhile
				else if (pause_open)
					request_focus(find_el<IInspectable>(root, L"PauseResume"));
				else if (game_cfg_open && pending_focus)
				{
					// The answer rebuilt the page, which picked its focus (the old button is gone).
				}
				else if (dialog_return_focus)
					request_focus(dialog_return_focus);
				else
					focus_top();
				dialog_return_focus.reset();
			}

			// ---- The menu logic's screen (RPCN account) ----
			void sync_screen()
			{
				std::optional<uwp_frontend::screen_view> sv = menu_visible || pause_open ? uwp_frontend::get_screen() : std::nullopt;
				const bool open = sv.has_value();
				if (open != screen_open)
				{
					screen_open = open;
					set_visible(screen_layer, open);
					if (!open)
					{
						screen_sig.clear();
						screen_buttons.clear();
						if (const auto children = children_of(screen_items_panel))
							children->Clear();
						// The screen changes what its settings row shows (the account name).
						if (settings_built)
							build_settings(settings_panel, false, &first_setting);
						update_gating();
						if (!dialog && !job_visible && !text_open())
							focus_top();
						return;
					}
					update_gating();
				}
				if (!open)
					return;

				std::string sig = sv->title + '\x1f' + sv->header;
				for (const auto& item : sv->items)
					sig += '\x1f' + item.label + '\x1e' + item.detail;
				if (sig == screen_sig)
					return;
				screen_sig = std::move(sig);

				set_text(t_screen_title, sv->title);
				set_text(t_screen_header, sv->header);
				const auto children = children_of(screen_items_panel);
				if (!children)
					return;
				children->Clear();
				screen_buttons.clear();
				for (u32 i = 0; i < sv->items.size(); i++)
				{
					std::wstring markup = k_xaml_screen_item;
					replace_all(markup, L"$LABEL$", xml_text(sv->items[i].label));
					replace_all(markup, L"$DETAIL$", xml_text(sv->items[i].detail));
					const com<IInspectable> item = load_xaml(markup, "screen item");
					const auto el = item.as<IUIElement>();
					const auto button = item.as<IButtonBase>();
					if (!el || !button)
						continue;
					int64_t token = 0;
					button->add_Click(make_handler<routed_handler>([this, i](IInspectable*, IRoutedEventArgs*)
					{
						// The next tick shows what the entry opened (the list may be rebuilt, this button with it).
						screen_last = i;
						uwp_frontend::screen_select(i);
					}).get(), &token);
					children->Append(el.get());
					screen_buttons.push_back(item);
				}
				if (!dialog && !job_visible && !text_open())
					focus_top();
			}

			// ---- Text field ----
			void sync_text()
			{
				const std::optional<uwp_frontend::text_view> tv = menu_visible || pause_open ? uwp_frontend::get_text_field() : std::nullopt;
				const u64 serial = tv ? tv->serial : 0;
				if (serial == text_serial)
					return;
				text_serial = serial;
				text_taken = false;
				text_box.reset();
				pass_box.reset();
				text_field_ctl.reset();
				if (const auto children = children_of(text_field_host))
					children->Clear();
				set_visible(text_error_el, false);

				if (tv)
				{
					std::wstring markup = tv->password ? k_xaml_password_field : k_xaml_text_field;
					replace_all(markup, L"$MAX$", std::to_wstring(tv->max_len));
					replace_all(markup, L"$SCOPE$", widen(tv->input_scope));
					const com<IInspectable> field = load_xaml(markup, "text field");
					text_box = field.as<ITextBox>();
					pass_box = field.as<IPasswordBox>();
					text_field_ctl = field.as<IControl>();
					if (text_box && !tv->value.empty())
					{
						const hstr v(tv->value);
						text_box->put_Text(v.get());
					}
					// Enter on the keyboard submits the field.
					if (const auto el = field.as<IUIElement>())
					{
						int64_t token = 0;
						el->add_KeyDown(make_handler<key_handler>([this](IInspectable*, IKeyRoutedEventArgs* e)
						{
							VirtualKey key{};
							if (e && SUCCEEDED(e->get_Key(&key)) && key == VirtualKey::Enter)
							{
								e->put_Handled(true);
								submit_text();
							}
						}).get(), &token);
						if (const auto children = children_of(text_field_host))
							children->Append(el.get());
					}
					set_text(t_text_title, tv->title);
					set_text(t_text_prompt, tv->prompt);
				}
				set_visible(text_layer, tv.has_value());
				update_gating();
				if (!dialog && !job_visible)
					focus_top();
			}

			void submit_text()
			{
				if (!text_open() || text_taken || dialog || job_visible)
					return;
				HSTRING h = nullptr;
				if (text_box)
					text_box->get_Text(&h);
				else if (pass_box)
					pass_box->get_Password(&h);
				const std::string error = uwp_frontend::text_field_done(take_string(h));
				if (!error.empty())
				{
					set_text(t_text_error, error);
					set_visible(text_error_el, true);
					return;
				}
				// The field is taken; the next tick shows what follows (another field, a message, a server call).
				// Not from here: this may run in the field's own key handler, and that would remove the field.
				text_taken = true;
			}

			void cancel_text()
			{
				if (!text_open() || text_taken)
					return;
				uwp_frontend::text_field_cancel();
				text_taken = true;
			}

			// ---- Pause menu ----
			void open_pause()
			{
				if (pause_open || !uwp_frontend::in_game() || loading_shown || error_shown)
					return;
				pause_open = true;
				// Entries for what the game has.
				set_visible(find_el<IUIElement>(root, L"PauseTrophies"), uwp_frontend::has_trophies());
				set_visible(find_el<IUIElement>(root, L"PauseLoadState"), uwp_frontend::has_save_states());
				show_pause_page(pause_page::main);
				set_visible(pause_layer, true);
				update_gating();
				uwp_frontend::set_paused(true);
				request_focus(find_el<IInspectable>(root, L"PauseResume"));
				xaml_log.notice("Pause menu opened");
			}

			// The pause pane's pages: the actions, the settings groups, one group's settings.
			enum class pause_page { main, groups, group };
			pause_page pause_at = pause_page::main;
			std::string pause_group;

			void show_pause_page(pause_page page, std::string group = {})
			{
				const std::string from = pause_group;
				pause_at = page;
				pause_group = std::move(group);
				set_visible(find_el<IUIElement>(root, L"PauseMain"), page == pause_page::main);
				set_visible(pause_settings_panel.as<IUIElement>(), page != pause_page::main);
				const bool rebuild = std::exchange(game_cfg_rebuild, false);
				if (const auto scroll = pause_scroll_ctl.as<IScrollViewer>(); scroll && !rebuild)
					scroll->ScrollToVerticalOffset(0);

				if (page == pause_page::main)
				{
					set_text(t_pause_title, "Paused");
					set_text(t_pause_subtitle, {});
					if (const auto children = children_of(pause_settings_panel))
						children->Clear();
					return;
				}

				if (page == pause_page::group)
				{
					if (game_cfg_open)
						game_cfg_serial = uwp_frontend::game_settings_serial();
					set_text(t_pause_title, pause_group);
					set_text(t_pause_subtitle, pause_group.starts_with("Advanced")
						? "These change the game's timing. Wrong values can slow it down, speed it up, hang or crash it."
						: game_cfg_open ? "For " + uwp_frontend::game_settings_title() + " only. Applies the next time it starts."
						: "Changes apply right away and are kept for later games.");
					com<IControl> first;
					build_settings(pause_settings_panel, true, &first, &pause_group, game_cfg_open, rebuild ? game_cfg_row : std::nullopt);
					game_cfg_row.reset();
					request_focus(first.as<IInspectable>());
					return;
				}

				if (game_cfg_open)
				{
					set_text(t_pause_title, uwp_frontend::game_settings_title());
					set_text(t_pause_subtitle, uwp_frontend::game_settings_own()
						? "This game's own settings. What isn't changed here comes from the Settings tab and the pause menu."
						: "This game uses the app's settings. What you change here is kept for this game only.");
				}
				else
				{
					set_text(t_pause_title, "Settings");
					set_text(t_pause_subtitle, "The running game's settings, as in RPCS3's own menu. The rest are on the Settings tab of the app menu.");
				}
				const auto children = children_of(pause_settings_panel);
				if (!children)
					return;
				children->Clear();
				std::vector<std::string> groups;
				for (const auto& v : game_cfg_open ? uwp_frontend::get_game_settings() : uwp_frontend::get_settings())
				{
					if (v.in_game && !v.section.empty() && std::find(groups.begin(), groups.end(), v.section) == groups.end())
						groups.push_back(v.section);
				}
				com<IInspectable> focus;
				for (const std::string& g : groups)
				{
					std::wstring markup = k_xaml_pause_entry;
					replace_all(markup, L"$LABEL$", xml_text(g));
					// Segoe MDL2 Assets glyphs, as Windows' own Settings app uses them.
					const wchar_t* glyph = g == "Video" ? L"&#xE7F4;" : g == "Audio" ? L"&#xE767;" : g == "Stats overlay" ? L"&#xE9D9;"
						: g == "Notifications" ? L"&#xEA8F;" : g == "Input" ? L"&#xE7FC;" : g == "Debug" ? L"&#xEBE8;" : g.starts_with("Advanced") ? L"&#xE950;"
						: g == "CPU" ? L"&#xE964;" : g == "Network" ? L"&#xE968;" : g == "Patches" ? L"&#xE90F;" : L"&#xE713;";
					replace_all(markup, L"$GLYPH$", glyph);
					const com<IInspectable> entry = load_xaml(markup, "pause entry");
					const auto button = entry.as<IButtonBase>();
					const auto el = entry.as<IUIElement>();
					if (!button || !el)
						continue;
					int64_t token = 0;
					button->add_Click(make_handler<routed_handler>([this, g](IInspectable*, IRoutedEventArgs*)
					{
						show_pause_page(pause_page::group, g);
					}).get(), &token);
					children->Append(el.get());
					if (!focus || g == from)
						focus = entry;
				}
				// A game's own settings can be dropped: it then boots with the app's settings again.
				if (game_cfg_open && uwp_frontend::game_settings_saved())
				{
					std::wstring markup = k_xaml_pause_entry;
					replace_all(markup, L"$LABEL$", L"Use the app's settings");
					replace_all(markup, L"$GLYPH$", L"&#xE777;");
					const com<IInspectable> entry = load_xaml(markup, "pause entry");
					if (const auto button = entry.as<IButtonBase>(); button && entry.as<IUIElement>())
					{
						int64_t token = 0;
						button->add_Click(make_handler<routed_handler>([this](IInspectable*, IRoutedEventArgs*)
						{
							show_dialog({"Use the app's settings?", "This game's own settings are removed. It starts with the settings of the Settings tab and the pause menu again.", "Remove", [this]()
							{
								uwp_frontend::reset_game_settings();
								show_pause_page(pause_page::groups);
							}});
						}).get(), &token);
						children->Append(entry.as<IUIElement>().get());
					}
				}
				request_focus(focus);
			}

			// ---- One game's settings (Menu on the Games tab) ----
			void open_game_cfg()
			{
				if (game_cfg_open || pause_open || !menu_visible || uwp_frontend::in_game())
					return;
				int32_t idx = -1;
				games_sel->get_SelectedIndex(&idx);
				if (idx < 0 || idx >= static_cast<int32_t>(snap.games.size()))
					return;
				if (!uwp_frontend::open_game_settings(snap.games[idx].path))
				{
					show_dialog({"No settings for this entry", "Only games have settings of their own. The PS3 system menu and programs use the settings of the Settings tab.", {}, {}});
					return;
				}
				game_cfg_open = true;
				show_pause_page(pause_page::groups);
				set_visible(pause_layer, true);
				update_gating();
				xaml_log.notice("Game settings opened for %s", snap.games[idx].title_id);
			}

			void close_game_cfg()
			{
				if (!game_cfg_open)
					return;
				game_cfg_open = false;
				set_visible(pause_layer, false);
				if (const auto children = children_of(pause_settings_panel))
					children->Clear();
				uwp_frontend::close_game_settings();
				update_gating();
				if (menu_visible)
					focus_menu();
				xaml_log.notice("Game settings closed");
			}

			void close_pause()
			{
				if (!pause_open)
					return;
				pause_open = false;
				set_visible(pause_layer, false);
				uwp_frontend::screen_close(); // a trophies / friends / save state list over it
				update_gating();
				uwp_frontend::set_paused(false);
				pending_focus.reset();
				xaml_log.notice("Pause menu closed");
			}

			// The user confirmed "Stop game": drop the pause menu, stop the emulation.
			void after_stop_chosen()
			{
				pause_open = false;
				set_visible(pause_layer, false);
				uwp_frontend::screen_close(); // a trophies / friends / save state list over it
				update_gating();
				pending_focus.reset();
				uwp_frontend::stop_game();
			}

			// The user confirmed "Restart game": drop the pause menu, stop and boot the game again.
			void after_restart_chosen()
			{
				pause_open = false;
				set_visible(pause_layer, false);
				uwp_frontend::screen_close(); // a trophies / friends / save state list over it
				update_gating();
				pending_focus.reset();
				uwp_frontend::restart_game();
			}

			void on_back()
			{
				if (dialog)
					close_dialog(false);
				else if (job_visible)
					return;
				else if (text_open())
					cancel_text();
				else if (screen_open)
					uwp_frontend::screen_close();
				else if (game_cfg_open && pause_at == pause_page::group)
					show_pause_page(pause_page::groups, pause_group);
				else if (game_cfg_open)
					close_game_cfg();
				else if (pause_open && pause_at == pause_page::group)
					show_pause_page(pause_page::groups, pause_group);
				else if (pause_open && pause_at == pause_page::groups)
				{
					show_pause_page(pause_page::main);
					request_focus(find_el<IInspectable>(root, L"PauseSettings"));
				}
				else if (pause_open)
					close_pause();
			}

			// ---- Overlays ----
			void sync_job()
			{
				const uwp_frontend::job_view job = uwp_frontend::get_job();
				if (job.active != job_visible)
				{
					job_visible = job.active;
					set_visible(job_layer, job.active);
					update_gating();
					// The install is over: the controls behind the progress card get the focus back
					// (a result dialog may follow and takes it).
					if (!job.active && !dialog && !pause_open && !loading_shown && !error_shown)
						focus_top();
				}
				if (!job.active)
					return;
				set_text(t_job_title, job.title);
				if (job_bar_range && job_bar)
				{
					job_bar->put_IsIndeterminate(job.total == 0);
					job_bar_range->put_Value(job.total ? 100.0 * std::min(job.done, job.total) / job.total : 0.0);
				}
				set_text(t_job_detail, job.server ? std::string("Waiting for the server...") : job.total ? fmt::format("%u%%", static_cast<u32>(u64{std::min(job.done, job.total)} * 100 / job.total)) : std::string("Checking the file (this takes a moment)..."));
			}

			void sync_overlay()
			{
				overlay_state ov;
				if (h.poll_overlay)
					h.poll_overlay(ov);

				if (ov.error != error_shown)
				{
					error_shown = ov.error;
					set_visible(error_layer, ov.error);
					update_gating();
					if (ov.error && pause_open)
						close_pause();
				}
				if (ov.error && ov.error_text != error_shown_text)
				{
					error_shown_text = ov.error_text;
					set_text(t_error, ov.error_text);
				}

				const bool loading = ov.loading && !ov.error;
				if (loading != loading_shown)
				{
					loading_shown = loading;
					set_visible(loading_layer, loading);
					update_gating();
					if (!loading)
					{
						loading_title_shown.clear();
						loading_detail_shown.clear();
					}
				}
				if (loading)
				{
					if (loading_backdrop)
						loading_backdrop->put_Opacity(ov.loading_over_game ? 0.85 : 1.0);
					if (ov.loading_title != loading_title_shown)
					{
						loading_title_shown = ov.loading_title;
						set_text(t_loading_title, ov.loading_title.empty() ? "Loading" : ov.loading_title);
					}
					if (ov.loading_detail != loading_detail_shown)
					{
						loading_detail_shown = ov.loading_detail;
						set_text(t_loading_detail, ov.loading_detail);
					}
					if (loading_bar && loading_bar_range)
					{
						loading_bar->put_IsIndeterminate(ov.loading_total == 0);
						loading_bar_range->put_Value(ov.loading_total ? 100.0 * std::min(ov.loading_done, ov.loading_total) / ov.loading_total : 0.0);
					}
				}
			}

			// A swap chain in a SwapChainPanel is shown one buffer pixel per DIP, and the Xbox lays XAML out with
			// a scale factor (200% on a 1080p screen: 960x540 DIPs), so a 1920x1080 buffer showed only its top-left quarter.
			// Scale the buffer to the panel's size instead.
			void fit_game_picture()
			{
				const auto fe = game_panel ? game_panel.as<IFrameworkElement>() : com<IFrameworkElement>{};
				if (!game_sc || !fe)
					return;
				double w = 0, h = 0;
				fe->get_ActualWidth(&w);
				fe->get_ActualHeight(&h);
				if (w <= 0 || h <= 0 || (w == game_panel_w && h == game_panel_h))
					return;
				DXGI_SWAP_CHAIN_DESC1 desc{};
				if (FAILED(game_sc->GetDesc1(&desc)) || !desc.Width || !desc.Height)
					return;
				game_panel_w = w;
				game_panel_h = h;
				DXGI_MATRIX_3X2_F m{};
				m._11 = static_cast<float>(w / desc.Width);
				m._22 = static_cast<float>(h / desc.Height);
				const HRESULT hr = game_sc->SetMatrixTransform(&m);
				xaml_log.notice("Game panel %.0fx%.0f DIPs, swap chain %ux%u: scale %.3f x %.3f (0x%08x)", w, h, desc.Width, desc.Height, m._11, m._22, static_cast<u32>(hr));
			}

			// ---- Tick ----
			void tick()
			{
				fit_game_picture();

				if (uwp_frontend::model_ready())
				{
					if (!settings_built)
					{
						settings_built = true;
						build_settings(settings_panel, false, &first_setting);
					}

					const u64 serial = uwp_frontend::model_serial();
					if (serial != seen_serial || seen_serial == 0)
					{
						seen_serial = serial ? serial : 1;
						uwp_frontend::get_snapshot(snap);
						sync_games();
						sync_installs();
						sync_header();
					}

					// Results of RPCN server calls arrive here; they may open a field or a message.
					uwp_frontend::pump();
					sync_job();

					if (!dialog)
					{
						if (auto d = uwp_frontend::take_pending_dialog())
							show_dialog(std::move(*d));
					}
				}

				const bool game = uwp_frontend::in_game();
				if (game == menu_visible)
				{
					menu_visible = !game;
					set_visible(menu_layer, menu_visible);
					update_gating();
					if (menu_visible)
					{
						// Back from a game: settings may have changed in the pause menu.
						if (settings_built)
							build_settings(settings_panel, false, &first_setting);
						if (!modal_open())
							focus_menu();
					}
					else
					{
						pending_focus.reset();
					}
				}
				if (!game && pause_open)
					close_pause();
				if (game && game_cfg_open)
					close_game_cfg();
				// The game's page changed (a patch download ended, a patch switched others of its group off): build it again.
				if (game_cfg_open && pause_at == pause_page::group && !dialog && game_cfg_serial != uwp_frontend::game_settings_serial())
				{
					game_cfg_rebuild = true;
					show_pause_page(pause_page::group, pause_group);
				}

				if (uwp_frontend::model_ready())
				{
					sync_screen();
					sync_text();
				}

				sync_overlay();

				if (g_pause_requested.exchange(false))
					open_pause();

				service_focus();
			}
		};

		shell* g_shell = nullptr;

		// ---- Application ----
		// The app's Application subclass: composed with Windows.UI.Xaml.Application (the inner object),
		// overriding OnLaunched, and the XAML type provider that hands XAML the WinUI 2 types.
		struct app_object final : IApplicationOverrides, xaml_metadata_provider
		{
			LONG refs = 1;
			IInspectable* inner = nullptr;
			com<xaml_metadata_provider> winui_types; // WinUI 2's own provider, created on first use
			bool winui_types_tried = false;

			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
			{
				if (riid == __uuidof(IUnknown) || riid == __uuidof(IInspectable) || riid == iid_v<IApplicationOverrides>)
				{
					*out = static_cast<IApplicationOverrides*>(this);
					AddRef();
					return S_OK;
				}
				if (riid == iid_v<IXamlMetadataProvider>)
				{
					*out = static_cast<xaml_metadata_provider*>(this);
					AddRef();
					return S_OK;
				}
				if (inner)
					return inner->QueryInterface(riid, out);
				*out = nullptr;
				return E_NOINTERFACE;
			}
			ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
			ULONG STDMETHODCALLTYPE Release() override
			{
				const LONG r = InterlockedDecrement(&refs);
				if (!r)
					delete this;
				return r;
			}
			HRESULT STDMETHODCALLTYPE GetIids(ULONG* count, IID** iids) override
			{
				*count = 0;
				*iids = nullptr;
				return S_OK;
			}
			HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* name) override
			{
				return WindowsCreateString(L"RPCS3Xbox.App", 13, name);
			}
			HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* level) override
			{
				*level = BaseTrust;
				return S_OK;
			}

			HRESULT STDMETHODCALLTYPE OnActivated(IInspectable*) override { return S_OK; }
			HRESULT STDMETHODCALLTYPE OnLaunched(IInspectable*) override { return launch(); }
			HRESULT STDMETHODCALLTYPE OnFileActivated(void*) override { return S_OK; }
			HRESULT STDMETHODCALLTYPE OnSearchActivated(IInspectable*) override { return S_OK; }
			HRESULT STDMETHODCALLTYPE OnShareTargetActivated(IInspectable*) override { return S_OK; }
			HRESULT STDMETHODCALLTYPE OnFileOpenPickerActivated(void*) override { return S_OK; }
			HRESULT STDMETHODCALLTYPE OnFileSavePickerActivated(void*) override { return S_OK; }
			HRESULT STDMETHODCALLTYPE OnCachedFileUpdaterActivated(void*) override { return S_OK; }
			HRESULT STDMETHODCALLTYPE OnWindowCreated(IInspectable*) override { return S_OK; }

			// IXamlMetadataProvider: everything goes to WinUI 2's provider (UI thread only).
			xaml_metadata_provider* winui()
			{
				if (!winui_types_tried && g_shell && g_shell->h.winui_styles)
				{
					winui_types_tried = true;
					if (const com<IInspectable> p = activate(L"Microsoft.UI.Xaml.XamlTypeInfo.XamlControlsXamlMetaDataProvider"))
						p->QueryInterface(iid_v<IXamlMetadataProvider>, reinterpret_cast<void**>(winui_types.put()));
					xaml_log.notice("WinUI 2 type provider: %s", winui_types ? "yes" : "no");
				}
				return winui_types.get();
			}
			HRESULT STDMETHODCALLTYPE GetXamlType(TypeName type, IInspectable** result) override
			{
				*result = nullptr;
				const auto p = winui();
				return p ? p->GetXamlType(type, result) : S_OK;
			}
			HRESULT STDMETHODCALLTYPE GetXamlTypeByFullName(HSTRING full_name, IInspectable** result) override
			{
				*result = nullptr;
				const auto p = winui();
				return p ? p->GetXamlTypeByFullName(full_name, result) : S_OK;
			}
			HRESULT STDMETHODCALLTYPE GetXmlnsDefinitions(UINT32* count, void** definitions) override
			{
				*count = 0;
				*definitions = nullptr;
				const auto p = winui();
				return p ? p->GetXmlnsDefinitions(count, definitions) : S_OK;
			}

			// The Windows 11 look: WinUI 2's XamlControlsResources as the application resources restyles the
			// standard controls (rounded buttons, list items, toggles, combo boxes, text boxes, focus visuals).
			// Without it (framework package missing, LocalState/system_styles) the pages keep the system styles.
			static void apply_winui_styles(IApplication* app)
			{
				const com<IInspectable> resources = activate(L"Microsoft.UI.Xaml.Controls.XamlControlsResources");
				const auto dictionary = resources.as<IResourceDictionary>();
				if (!dictionary)
				{
					xaml_log.error("WinUI 2 styles unavailable: the standard control styles stay");
					return;
				}
				const HRESULT hr = app->put_Resources(dictionary.get());
				xaml_log.notice("WinUI 2 styles: 0x%08x", static_cast<u32>(hr));
			}

			static HRESULT launch()
			{
				shell& sh = *g_shell;
				if (sh.launched)
					return S_OK; // a second activation: the window exists
				sh.launched = true;

				// No mouse pointer emulation: the app is driven by the controller.
				if (const auto statics = get_factory<IApplicationStatics>(L"Windows.UI.Xaml.Application"))
				{
					IApplication* current = nullptr;
					if (SUCCEEDED(statics->get_Current(&current)) && current)
					{
						const com<IApplication> app(current);
						if (const auto app2 = app.as<IApplication2>())
							app2->put_RequiresPointerMode(ApplicationRequiresPointerMode::WhenRequested);
						if (sh.h.winui_styles)
							apply_winui_styles(app.get());
						int64_t token = 0;
						app->add_Suspending(make_handler<suspending_handler>([](IInspectable*, IInspectable*)
						{
							if (g_shell->h.suspending)
								g_shell->h.suspending();
						}).get(), &token);
						app->add_Resuming(make_handler<tick_handler>([](IInspectable*, IInspectable*)
						{
							if (g_shell->h.resuming)
								g_shell->h.resuming();
						}).get(), &token);
					}
				}

				// Use the whole screen (the default is the TV-safe area); the pages keep their own margins.
				if (const auto views = get_factory<IApplicationViewStatics2>(L"Windows.UI.ViewManagement.ApplicationView"))
				{
					com<IInspectable> view;
					if (SUCCEEDED(views->GetForCurrentView(view.put())) && view)
					{
						boolean ok = false;
						if (const auto view2 = view.as<IApplicationView2>())
							view2->SetDesiredBoundsMode(ApplicationViewBoundsMode::UseCoreWindow, &ok);
						xaml_log.notice("Window bounds: whole screen %d", static_cast<int>(ok));
					}
				}

				if (!sh.build())
				{
					// Say so on the screen (the emulator still starts, for its log).
					xaml_log.error("Couldn't build the XAML shell");
					const com<IInspectable> notice = load_xaml(
						L"<TextBlock xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" Margin=\"48\" FontSize=\"20\" TextWrapping=\"Wrap\">"
						L"The XAML menu couldn't be built; see RPCS3.log in LocalState/rpcs3. "
						L"Create an empty file named legacy_ui in LocalState and restart the app to use the earlier menu.</TextBlock>", "notice");
					const auto windows = get_factory<IWindowStatics>(L"Windows.UI.Xaml.Window");
					com<IWindow> window;
					if (windows)
						windows->get_Current(window.put());
					if (window && notice)
					{
						window->put_Content(notice.as<IUIElement>().get());
						window->Activate();
					}
					if (sh.h.launched)
						sh.h.launched();
					return S_OK;
				}

				// Game picture: the shell's composition swap chain goes into the panel.
				if (sh.h.create_swap_chain)
				{
					if (IDXGISwapChain1* swap_chain = sh.h.create_swap_chain())
					{
						ISwapChainPanelNative* native = nullptr;
						if (const auto panel = sh.game_panel.as<IInspectable>(); panel && SUCCEEDED(panel->QueryInterface(IID_ISwapChainPanelNative, reinterpret_cast<void**>(&native))) && native)
						{
							const HRESULT hr = native->SetSwapChain(swap_chain);
							xaml_log.notice("SwapChainPanel.SetSwapChain: 0x%08x", static_cast<u32>(hr));
							if (SUCCEEDED(hr))
								swap_chain->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void**>(&sh.game_sc));
							native->Release();
						}
						else
						{
							xaml_log.error("The game panel has no ISwapChainPanelNative");
						}
						swap_chain->Release();
					}
				}

				const auto windows = get_factory<IWindowStatics>(L"Windows.UI.Xaml.Window");
				com<IWindow> window;
				if (windows)
					windows->get_Current(window.put());
				if (!window)
				{
					xaml_log.error("No current XAML window");
					return S_OK;
				}
				window->put_Content(sh.root.as<IUIElement>().get());
				window->Activate();

				// B: the console leaves the app unless the back request is handled. It closes dialogs and the pause menu.
				if (const auto nav_statics = get_factory<ISystemNavigationManagerStatics>(L"Windows.UI.Core.SystemNavigationManager"))
				{
					ISystemNavigationManager* nav = nullptr;
					if (SUCCEEDED(nav_statics->GetForCurrentView(&nav)) && nav)
					{
						int64_t token = 0;
						const HRESULT hr = nav->add_BackRequested(make_handler<back_handler>([](IInspectable*, IBackRequestedEventArgs* args)
						{
							if (args)
								args->put_Handled(true);
							if (g_shell)
								g_shell->on_back();
						}).get(), &token);
						xaml_log.notice("Back button handler: 0x%08x", static_cast<u32>(hr));
						nav->Release();
					}
				}

				// The model is polled from here, on the UI thread.
				com<IInspectable> timer_obj = activate(L"Windows.UI.Xaml.DispatcherTimer");
				sh.timer = timer_obj.as<IDispatcherTimer>();
				if (sh.timer)
				{
					int64_t token = 0;
					sh.timer->put_Interval(TimeSpan{330000}); // 33 ms in 100 ns units
					sh.timer->add_Tick(make_handler<tick_handler>([](IInspectable*, IInspectable*)
					{
						if (g_shell)
							g_shell->tick();
					}).get(), &token);
					sh.timer->Start();
				}

				xaml_log.notice("XAML shell is up");
				if (sh.h.launched)
					sh.h.launched();
				return S_OK;
			}
		};

		struct init_callback final : ApplicationInitializationCallback
		{
			LONG refs = 1;

			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
			{
				if (riid == __uuidof(IUnknown) || riid == iid_v<ApplicationInitializationCallback> || riid == __uuidof(IAgileObject))
				{
					*out = static_cast<ApplicationInitializationCallback*>(this);
					AddRef();
					return S_OK;
				}
				*out = nullptr;
				return E_NOINTERFACE;
			}
			ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
			ULONG STDMETHODCALLTYPE Release() override
			{
				const LONG r = InterlockedDecrement(&refs);
				if (!r)
					delete this;
				return r;
			}

			HRESULT STDMETHODCALLTYPE Invoke(IApplicationInitializationCallbackParams*) override
			{
				const auto factory = get_factory<IApplicationFactory>(L"Windows.UI.Xaml.Application");
				if (!factory)
					return E_FAIL;
				auto* app = new app_object();
				IInspectable* inner = nullptr;
				IApplication* instance = nullptr;
				const HRESULT hr = factory->CreateInstance(static_cast<IApplicationOverrides*>(app), &inner, &instance);
				xaml_log.notice("Application.CreateInstance: 0x%08x", static_cast<u32>(hr));
				if (FAILED(hr))
				{
					app->Release();
					return hr;
				}
				// The application lives as long as the process: both references stay.
				app->inner = inner;
				return S_OK;
			}
		};
	}

	int run(const host& h)
	{
		g_shell = new shell();
		g_shell->h = h;

		const auto statics = get_factory<IApplicationStatics>(L"Windows.UI.Xaml.Application");
		if (!statics)
			return 1;
		auto* callback = new init_callback();
		const HRESULT hr = statics->Start(callback);
		callback->Release();
		xaml_log.notice("Application.Start returned 0x%08x", static_cast<u32>(hr));
		return SUCCEEDED(hr) ? 0 : 1;
	}

	void request_pause_menu()
	{
		g_pause_requested = true;
	}
}
