#include "modarchive.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace modarchive {
namespace {

std::string to_lower(std::string s) {
    for (char& c : s) {
        c = char(std::tolower(unsigned(c)));
    }
    return s;
}

std::string url_decode(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int v = 0;
            if (std::sscanf(s.c_str() + i + 1, "%2x", &v) == 1) {
                out.push_back(char(v));
                i += 2;
                continue;
            }
        }
        if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::string extension_of(const std::string& name) {
    const auto pos = name.find_last_of('.');
    if (pos == std::string::npos) {
        return {};
    }
    return to_lower(name.substr(pos));
}

bool supported_ext(const std::string& ext) {
    static const char* kOk[] = {".mod", ".xm", ".s3m", ".it", ".mmd0", ".mmd1", ".mmd2", ".mmd3",
                                ".med", ".hsq"};
    for (const char* e : kOk) {
        if (ext == e) {
            return true;
        }
    }
    return false;
}

std::string sanitize_filename(std::string name) {
    name = url_decode(name);
    for (char& c : name) {
        if (c < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' ||
            c == '|' || c == '?' || c == '*') {
            c = '_';
        }
    }
    if (name.empty()) {
        name = "random.mod";
    }
    return name;
}

#ifdef _WIN32

std::wstring widen(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

struct UrlParts {
    bool https = true;
    std::wstring host;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    std::wstring path;
};

UrlParts parse_url(const std::string& url) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{};
    wchar_t path[2048]{};
    wchar_t extra[1024]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 1024;
    const std::wstring wurl = widen(url);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        throw std::runtime_error("bad URL: " + url);
    }
    UrlParts p;
    p.https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    p.host.assign(host, uc.dwHostNameLength);
    p.port = uc.nPort;
    p.path.assign(path, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength) {
        p.path.append(extra, uc.dwExtraInfoLength);
    }
    return p;
}

std::string http_get_win(const std::string& url, std::vector<uint8_t>* binary) {
    const UrlParts parts = parse_url(url);
    HINTERNET session = WinHttpOpen(L"ImTrakker/1.0 (+https://modarchive.org)",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        throw std::runtime_error("WinHttpOpen failed");
    }
    HINTERNET connect =
        WinHttpConnect(session, parts.host.c_str(), parts.port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        throw std::runtime_error("WinHttpConnect failed");
    }
    DWORD flags = WINHTTP_FLAG_REFRESH;
    if (parts.https) {
        flags |= WINHTTP_FLAG_SECURE;
    }
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", parts.path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        throw std::runtime_error("WinHttpOpenRequest failed");
    }

    // Follow redirects (downloads.php may bounce).
    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));

    BOOL ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                                 0, 0, 0);
    if (ok) {
        ok = WinHttpReceiveResponse(request, nullptr);
    }
    if (!ok) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        throw std::runtime_error("HTTP request failed for " + url);
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    if (status < 200 || status >= 300) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        throw std::runtime_error("HTTP " + std::to_string(status) + " for " + url);
    }

    std::vector<uint8_t> body;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail)) {
            break;
        }
        if (avail == 0) {
            break;
        }
        const size_t at = body.size();
        body.resize(at + avail);
        DWORD read = 0;
        if (!WinHttpReadData(request, body.data() + at, avail, &read)) {
            break;
        }
        body.resize(at + read);
        if (read == 0) {
            break;
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (binary) {
        *binary = body;
        return {};
    }
    return std::string(body.begin(), body.end());
}

#else

std::string http_get_win(const std::string&, std::vector<uint8_t>*) {
    throw std::runtime_error("ModArchive download requires Windows WinHTTP in this build");
}

#endif

struct ParsedMod {
    int id = 0;
    std::string filename;
    std::string title;
};

ParsedMod parse_random_html(const std::string& html) {
    ParsedMod out;
    // downloads.php?moduleid=12345#file.mod
    static const std::regex re_dl(
        R"(downloads\.php\?moduleid=(\d+)#([A-Za-z0-9%._+\-]+))", std::regex::icase);
    std::smatch m;
    if (std::regex_search(html, m, re_dl)) {
        out.id = std::stoi(m[1].str());
        out.filename = sanitize_filename(m[2].str());
    } else {
        static const std::regex re_id(R"(downloads\.php\?moduleid=(\d+))", std::regex::icase);
        if (std::regex_search(html, m, re_id)) {
            out.id = std::stoi(m[1].str());
        }
    }
    // Title from page heading / view_by_moduleid link text — best effort
    static const std::regex re_title(R"(view_by_moduleid&query=\d+[^>]*>([^<]{1,80})<)",
                                    std::regex::icase);
    if (std::regex_search(html, m, re_title)) {
        out.title = m[1].str();
        while (!out.title.empty() && (out.title.back() == ' ' || out.title.back() == '\n')) {
            out.title.pop_back();
        }
    }
    if (out.filename.empty() && out.id) {
        out.filename = "module_" + std::to_string(out.id) + ".mod";
    }
    return out;
}

}  // namespace

std::string http_get(const std::string& url, std::vector<uint8_t>* binary) {
    return http_get_win(url, binary);
}

FetchResult fetch_random(const std::filesystem::path& cache_dir, int max_tries) {
    FetchResult result;
    try {
        std::filesystem::create_directories(cache_dir);
    } catch (const std::exception& e) {
        result.error = std::string("cache dir: ") + e.what();
        return result;
    }

    max_tries = std::clamp(max_tries, 1, 20);
    for (int attempt = 0; attempt < max_tries; ++attempt) {
        try {
            const std::string html =
                http_get("https://modarchive.org/index.php?request=view_random");
            const ParsedMod parsed = parse_random_html(html);
            if (parsed.id <= 0) {
                continue;
            }
            const std::string ext = extension_of(parsed.filename);
            if (!ext.empty() && !supported_ext(ext)) {
                // Skip formats we cannot load (AHX, MO3, …)
                continue;
            }

            std::vector<uint8_t> bytes;
            const std::string dl =
                "https://api.modarchive.org/downloads.php?moduleid=" + std::to_string(parsed.id);
            http_get(dl, &bytes);
            if (bytes.size() < 64) {
                continue;
            }

            std::string fname = parsed.filename;
            if (extension_of(fname).empty()) {
                // Sniff
                if (bytes.size() >= 17 && std::memcmp(bytes.data(), "Extended Module: ", 17) == 0) {
                    fname += ".xm";
                } else if (bytes.size() >= 0x30 &&
                           std::memcmp(bytes.data() + 0x2C, "SCRM", 4) == 0) {
                    fname += ".s3m";
                } else if (bytes.size() >= 4 && std::memcmp(bytes.data(), "IMPM", 4) == 0) {
                    fname += ".it";
                } else {
                    fname += ".mod";
                }
            }
            if (!supported_ext(extension_of(fname))) {
                continue;
            }

            const auto out_path = cache_dir / fname;
            {
                std::ofstream out(out_path, std::ios::binary);
                if (!out) {
                    result.error = "cannot write " + out_path.string();
                    return result;
                }
                out.write(reinterpret_cast<const char*>(bytes.data()),
                          std::streamsize(bytes.size()));
            }

            result.ok = true;
            result.path = out_path;
            result.filename = fname;
            result.module_id = parsed.id;
            result.title = parsed.title.empty() ? fname : parsed.title;
            return result;
        } catch (const std::exception& e) {
            result.error = e.what();
            // try again
        }
    }

    if (result.error.empty()) {
        result.error = "no supported random module after " + std::to_string(max_tries) + " tries";
    }
    return result;
}

}  // namespace modarchive
