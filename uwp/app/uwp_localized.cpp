// English strings for the emulator's own overlays (home menu, dialogs, trophies).
// The Qt UI gets these from rpcs3qt/localized_emu.h through tr(); without them every label
// in the home menu was empty. The table is generated from that header at build time.
#include "stdafx.h"
#include "uwp_localized.h"
#include "Emu/RSX/Overlays/overlay_utils.h"
#include "Utilities/Config.h"

namespace
{
	struct localized_entry
	{
		bool has_arg;
		std::string_view text;
	};

	localized_entry find_entry(localized_string_id id)
	{
		switch (id)
		{
#define X(name, has_arg, text) case localized_string_id::name: return { has_arg, reinterpret_cast<const char*>(text) };
#include "uwp_localized_strings.inc"
#undef X
		default: return { false, "Unknown" };
		}
	}
}

std::string uwp_localized_string(localized_string_id id, const char* args)
{
	const localized_entry entry = find_entry(id);
	std::string result(entry.text);

	// Same as QString::arg with one argument: every "%0" becomes the argument.
	if (entry.has_arg)
	{
		const std::string_view arg = args ? args : "";
		for (usz pos = result.find("%0"); pos != std::string::npos; pos = result.find("%0", pos + arg.size()))
		{
			result.replace(pos, 2, arg);
		}
	}

	return result;
}

std::u32string uwp_localized_u32string(localized_string_id id, const char* args)
{
	return utf8_to_u32string(uwp_localized_string(id, args));
}

std::string uwp_localized_setting(const cfg::_base* node, u32 enum_index)
{
	// The Qt UI has nicer names for some values; the config's own names are readable too.
	if (node)
	{
		if (const std::vector<std::string> list = node->to_list(); enum_index < list.size())
		{
			return list[enum_index];
		}
	}

	return {};
}
