#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace modarchive {

struct FetchResult {
    bool ok = false;
    std::filesystem::path path;
    std::string filename;
    int module_id = 0;
    std::string error;
    std::string title;
};

// Download HTTPS body as text (or binary into `binary` if non-null).
// Throws std::runtime_error on hard failure.
std::string http_get(const std::string& url, std::vector<uint8_t>* binary = nullptr);

// Hit Mod Archive "Random Pick!", download a module we can load.
// Retries until extension matches preferred_exts (default MOD/XM/S3M/IT) or tries exhausted.
FetchResult fetch_random(const std::filesystem::path& cache_dir, int max_tries = 10);

}  // namespace modarchive
