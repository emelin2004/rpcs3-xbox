// The rpcs3.net patch server's answer (see uwp_patches.h): one JSON object of strings and numbers, the
// patch file being one ~1 MB string. No JSON library in the UWP build, so a small reader.

#include "uwp_patches.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace uwp_patches
{
	namespace
	{
		void skip_ws(std::string_view s, std::size_t& i)
		{
			while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n'))
				i++;
		}

		void put_utf8(std::string& out, std::uint32_t cp)
		{
			if (cp < 0x80)
				out += static_cast<char>(cp);
			else if (cp < 0x800)
			{
				out += static_cast<char>(0xc0 | (cp >> 6));
				out += static_cast<char>(0x80 | (cp & 0x3f));
			}
			else if (cp < 0x10000)
			{
				out += static_cast<char>(0xe0 | (cp >> 12));
				out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
				out += static_cast<char>(0x80 | (cp & 0x3f));
			}
			else
			{
				out += static_cast<char>(0xf0 | (cp >> 18));
				out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
				out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
				out += static_cast<char>(0x80 | (cp & 0x3f));
			}
		}

		bool read_hex4(std::string_view s, std::size_t& i, std::uint32_t& out)
		{
			if (i + 4 > s.size())
				return false;
			out = 0;
			for (std::size_t k = 0; k < 4; k++)
			{
				const char c = s[i++];
				out <<= 4;
				if (c >= '0' && c <= '9')
					out |= c - '0';
				else if (c >= 'a' && c <= 'f')
					out |= c - 'a' + 10;
				else if (c >= 'A' && c <= 'F')
					out |= c - 'A' + 10;
				else
					return false;
			}
			return true;
		}

		// s[i] is the opening quote.
		bool read_string(std::string_view s, std::size_t& i, std::string& out)
		{
			out.clear();
			if (i >= s.size() || s[i] != '"')
				return false;
			i++;
			while (i < s.size())
			{
				const char c = s[i++];
				if (c == '"')
					return true;
				if (c != '\\')
				{
					out += c;
					continue;
				}
				if (i >= s.size())
					return false;
				switch (s[i++])
				{
				case '"': out += '"'; break;
				case '\\': out += '\\'; break;
				case '/': out += '/'; break;
				case 'b': out += '\b'; break;
				case 'f': out += '\f'; break;
				case 'n': out += '\n'; break;
				case 'r': out += '\r'; break;
				case 't': out += '\t'; break;
				case 'u':
				{
					std::uint32_t cp = 0;
					if (!read_hex4(s, i, cp))
						return false;
					if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u')
					{
						std::size_t j = i + 2;
						std::uint32_t low = 0;
						if (read_hex4(s, j, low) && low >= 0xdc00 && low < 0xe000)
						{
							cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
							i = j;
						}
					}
					put_utf8(out, cp);
					break;
				}
				default:
					return false;
				}
			}
			return false;
		}

	}

	bool parse_answer(const std::string& json, int& return_code, std::string& version, std::string& sha256, std::string& patch)
	{
		return_code = -255;
		const std::string_view s = json;
		std::size_t i = 0;
		skip_ws(s, i);
		if (i >= s.size() || s[i++] != '{')
			return false;
		std::string key, value;
		while (true)
		{
			skip_ws(s, i);
			if (i < s.size() && s[i] == '}')
				return true;
			if (!read_string(s, i, key))
				return false;
			skip_ws(s, i);
			if (i >= s.size() || s[i++] != ':')
				return false;
			skip_ws(s, i);
			if (i < s.size() && s[i] == '"')
			{
				if (!read_string(s, i, value))
					return false;
				if (key == "version")
					version = value;
				else if (key == "sha256")
					sha256 = value;
				else if (key == "patch")
					patch = std::move(value);
			}
			else
			{
				// A number (or true / false / null).
				const std::size_t start = i;
				while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ' ' && s[i] != '\r' && s[i] != '\n' && s[i] != '\t')
					i++;
				const std::string_view raw = s.substr(start, i - start);
				if (key == "return_code")
				{
					int n = 0;
					bool neg = false;
					std::size_t k = 0;
					if (k < raw.size() && raw[k] == '-')
						neg = true, k++;
					if (k >= raw.size())
						return false;
					for (; k < raw.size(); k++)
					{
						if (raw[k] < '0' || raw[k] > '9')
							return false;
						n = n * 10 + (raw[k] - '0');
					}
					return_code = neg ? -n : n;
				}
			}
			skip_ws(s, i);
			if (i < s.size() && s[i] == ',')
			{
				i++;
				continue;
			}
			if (i < s.size() && s[i] == '}')
				return true;
			return false;
		}
	}
}
