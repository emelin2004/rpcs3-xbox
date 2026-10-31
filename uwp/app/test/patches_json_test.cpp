// Host test for the patch server's JSON reader:
//   c++ -std=c++20 -I uwp/app uwp/app/uwp_patches_json.cpp uwp/app/test/patches_json_test.cpp -o /tmp/patches_json_test && /tmp/patches_json_test [answer.json]
// With a saved answer of https://rpcs3.net/compatibility?patch&api=v1&v=1.2 it also prints the patch's size
// (compare its SHA-256 with the answer's "sha256").

#include "uwp_patches.h"

#include <cstdio>
#include <fstream>
#include <sstream>

int main(int argc, char** argv)
{
	int failed = 0;
	const auto check = [&](bool ok, const char* what)
	{
		if (!ok)
		{
			std::printf("FAIL: %s\n", what);
			failed++;
		}
	};

	int code = 0;
	std::string version, sha, patch;
	check(uwp_patches::parse_answer("{\n  \"return_code\": 0,\n  \"version\": \"1.2\",\n  \"sha256\": \"ab\",\n  \"patch\": \"Version: 1.2\\n\\nA:\\n  \\\"x\\\": [ be32, 0x0 ] # \\/ \\u00e9 \\ud83d\\ude00 \\\\\"\n}", code, version, sha, patch), "full answer");
	check(code == 0 && version == "1.2" && sha == "ab", "fields");
	check(patch == "Version: 1.2\n\nA:\n  \"x\": [ be32, 0x0 ] # / \xc3\xa9 \xf0\x9f\x98\x80 \\", "escapes");

	version.clear();
	patch.clear();
	check(uwp_patches::parse_answer("{\"return_code\":1}", code, version, sha, patch) && code == 1 && patch.empty(), "up to date");
	check(uwp_patches::parse_answer("{\"return_code\": -2 , \"x\": null}", code, version, sha, patch) && code == -2, "negative code");
	check(!uwp_patches::parse_answer("<html>Bad gateway</html>", code, version, sha, patch), "not JSON");
	check(!uwp_patches::parse_answer("{\"patch\": \"cut", code, version, sha, patch), "cut string");
	check(!uwp_patches::parse_answer("{\"patch\": \"\\q\"}", code, version, sha, patch), "bad escape");

	if (argc > 1)
	{
		std::ifstream in(argv[1], std::ios::binary);
		std::stringstream ss;
		ss << in.rdbuf();
		check(uwp_patches::parse_answer(ss.str(), code, version, sha, patch), "saved answer");
		std::printf("saved answer: code %d, version %s, sha256 %s, patch %zu bytes\n", code, version.c_str(), sha.c_str(), patch.size());
		std::ofstream("patch_from_test.yml", std::ios::binary) << patch;
	}

	std::printf(failed ? "%d failed\n" : "all passed\n", failed);
	return failed ? 1 : 0;
}
