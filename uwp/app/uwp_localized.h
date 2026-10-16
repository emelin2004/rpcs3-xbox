#pragma once

#include "Emu/localized_string_id.h"
#include "util/types.hpp"

#include <string>

namespace cfg
{
	class _base;
}

std::string uwp_localized_string(localized_string_id id, const char* args);
std::u32string uwp_localized_u32string(localized_string_id id, const char* args);
std::string uwp_localized_setting(const cfg::_base* node, u32 enum_index);
