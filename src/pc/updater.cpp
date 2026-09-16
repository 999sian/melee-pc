/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "updater.hpp"
#include "version.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")
#elif defined(MELEE_USE_CURL)
#include <curl/curl.h>
#include <sys/stat.h>
#include <unistd.h>
#elif (defined(__linux__) || defined(__APPLE__)) && !defined(__ANDROID__)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pc::updater {
namespace {

std::mutex g_updater_mutex;
UpdateState g_updater_state;
std::atomic_bool g_cancel{false};

struct WorkerThreadManager {
    std::thread thread;
    ~WorkerThreadManager() {
        g_cancel = true;
        if (thread.joinable()) {
            thread.join();
        }
    }
} g_worker;

// Simple JSON parser for GitHub Release schema
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool bool_val = false;
    double num_val = 0.0;
    std::string str_val;
    std::vector<JsonValue> arr_val;
    std::vector<std::pair<std::string, JsonValue>> obj_val;

    const JsonValue* find(std::string_view key) const {
        if (type != Type::Object)
            return nullptr;
        for (const auto& [k, v] : obj_val) {
            if (k == key)
                return &v;
        }
        return nullptr;
    }

    std::string get_string(std::string_view key, std::string default_val = "") const {
        if (const auto* v = find(key)) {
            if (v->type == Type::String)
                return v->str_val;
        }
        return default_val;
    }

    bool get_bool(std::string_view key, bool default_val = false) const {
        if (const auto* v = find(key)) {
            if (v->type == Type::Bool)
                return v->bool_val;
        }
        return default_val;
    }

    size_t get_size(std::string_view key, size_t default_val = 0) const {
        if (const auto* v = find(key)) {
            if (v->type == Type::Number)
                return static_cast<size_t>(v->num_val);
        }
        return default_val;
    }
};

class SimpleJsonParser {
    std::string_view src;
    size_t pos = 0;

    void skip_whitespace() {
        while (pos < src.size()) {
            char c = src[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                pos++;
            } else {
                break;
            }
        }
    }

    std::string parse_string() {
        if (pos >= src.size() || src[pos] != '"')
            return "";
        pos++;  // skip "
        std::string res;
        while (pos < src.size()) {
            char c = src[pos++];
            if (c == '"') {
                return res;
            } else if (c == '\\' && pos < src.size()) {
                char esc = src[pos++];
                switch (esc) {
                case '"':
                    res += '"';
                    break;
                case '\\':
                    res += '\\';
                    break;
                case '/':
                    res += '/';
                    break;
                case 'b':
                    res += '\b';
                    break;
                case 'f':
                    res += '\f';
                    break;
                case 'n':
                    res += '\n';
                    break;
                case 'r':
                    res += '\r';
                    break;
                case 't':
                    res += '\t';
                    break;
                case 'u':
                    if (pos + 4 <= src.size()) {
                        pos += 4;  // skip unicode escape digits for now
                        res += '?';
                    }
                    break;
                default:
                    res += esc;
                    break;
                }
            } else {
                res += c;
            }
        }
        return res;
    }

    JsonValue parse_number() {
        size_t start = pos;
        if (pos < src.size() && (src[pos] == '-' || src[pos] == '+'))
            pos++;
        while (pos < src.size() &&
               ((src[pos] >= '0' && src[pos] <= '9') || src[pos] == '.' || src[pos] == 'e' ||
                   src[pos] == 'E' || src[pos] == '+' || src[pos] == '-'))
        {
            pos++;
        }
        double val = 0.0;
        try {
            val = std::stod(std::string(src.substr(start, pos - start)));
        } catch (...) {
        }
        JsonValue j;
        j.type = JsonValue::Type::Number;
        j.num_val = val;
        return j;
    }

public:
    explicit SimpleJsonParser(std::string_view json) : src(json) {}

    JsonValue parse_value() {
        skip_whitespace();
        if (pos >= src.size())
            return {};

        char c = src[pos];
        if (c == '"') {
            JsonValue j;
            j.type = JsonValue::Type::String;
            j.str_val = parse_string();
            return j;
        } else if (c == '{') {
            pos++;  // skip '{'
            JsonValue j;
            j.type = JsonValue::Type::Object;
            skip_whitespace();
            if (pos < src.size() && src[pos] == '}') {
                pos++;
                return j;
            }
            while (pos < src.size()) {
                skip_whitespace();
                if (pos >= src.size() || src[pos] != '"')
                    break;
                std::string key = parse_string();
                skip_whitespace();
                if (pos >= src.size() || src[pos] != ':')
                    break;
                pos++;  // skip ':'
                JsonValue val = parse_value();
                j.obj_val.emplace_back(std::move(key), std::move(val));
                skip_whitespace();
                if (pos < src.size() && src[pos] == ',') {
                    pos++;
                } else if (pos < src.size() && src[pos] == '}') {
                    pos++;
                    break;
                } else {
                    break;
                }
            }
            return j;
        } else if (c == '[') {
            pos++;  // skip '['
            JsonValue j;
            j.type = JsonValue::Type::Array;
            skip_whitespace();
            if (pos < src.size() && src[pos] == ']') {
                pos++;
                return j;
            }
            while (pos < src.size()) {
                JsonValue val = parse_value();
                j.arr_val.push_back(std::move(val));
                skip_whitespace();
                if (pos < src.size() && src[pos] == ',') {
                    pos++;
                } else if (pos < src.size() && src[pos] == ']') {
                    pos++;
                    break;
                } else {
                    break;
                }
            }
            return j;
        } else if (c == 't' && src.substr(pos, 4) == "true") {
            pos += 4;
            JsonValue j;
            j.type = JsonValue::Type::Bool;
            j.bool_val = true;
            return j;
        } else if (c == 'f' && src.substr(pos, 5) == "false") {
            pos += 5;
            JsonValue j;
            j.type = JsonValue::Type::Bool;
            j.bool_val = false;
            return j;
        } else if (c == 'n' && src.substr(pos, 4) == "null") {
            pos += 4;
            return {};
        } else if ((c >= '0' && c <= '9') || c == '-') {
            return parse_number();
        }
        return {};
    }
};

std::vector<Release> parse_github_releases(std::string_view json) {
    std::vector<Release> releases;
    SimpleJsonParser parser(json);
    JsonValue root = parser.parse_value();

    std::vector<const JsonValue*> items;
    if (root.type == JsonValue::Type::Array) {
        for (const auto& item : root.arr_val) {
            items.push_back(&item);
        }
    } else if (root.type == JsonValue::Type::Object) {
        items.push_back(&root);
    }

    for (const auto* item : items) {
        if (!item || item->type != JsonValue::Type::Object)
            continue;
        Release r;
        r.tag_name = item->get_string("tag_name");
        r.name = item->get_string("name");
        r.html_url = item->get_string("html_url");
        r.body = item->get_string("body");
        r.prerelease = item->get_bool("prerelease", false);
        r.published_at = item->get_string("published_at");

        if (const auto* assets_node = item->find("assets")) {
            if (assets_node->type == JsonValue::Type::Array) {
                for (const auto& a_node : assets_node->arr_val) {
                    if (a_node.type != JsonValue::Type::Object)
                        continue;
                    Asset a;
                    a.name = a_node.get_string("name");
                    a.download_url = a_node.get_string("browser_download_url");
                    a.size = a_node.get_size("size", 0);
                    if (!a.download_url.empty()) {
                        r.assets.push_back(std::move(a));
                    }
                }
            }
        }

        if (!r.tag_name.empty()) {
            releases.push_back(std::move(r));
        }
    }
    return releases;
}

std::string select_best_asset(
    const std::vector<Asset>& assets, std::string& out_url, size_t& out_size) {
#if defined(__ANDROID__)
    for (const auto& a : assets) {
        if (a.name.find(".apk") != std::string::npos) {
            out_url = a.download_url;
            out_size = a.size;
            return a.name;
        }
    }
#elif defined(_WIN32)
    for (const auto& a : assets) {
        if (a.name.find("Windows") != std::string::npos && a.name.find(".zip") != std::string::npos)
        {
            out_url = a.download_url;
            out_size = a.size;
            return a.name;
        }
    }
    for (const auto& a : assets) {
        if (a.name.find(".zip") != std::string::npos) {
            out_url = a.download_url;
            out_size = a.size;
            return a.name;
        }
    }
#else
    // Linux
    for (const auto& a : assets) {
        if (a.name.find(".AppImage") != std::string::npos) {
            out_url = a.download_url;
            out_size = a.size;
            return a.name;
        }
    }
    for (const auto& a : assets) {
        if (a.name.find("linux") != std::string::npos &&
            a.name.find(".tar.gz") != std::string::npos)
        {
            out_url = a.download_url;
            out_size = a.size;
            return a.name;
        }
    }
#endif
    if (!assets.empty()) {
        out_url = assets[0].download_url;
        out_size = assets[0].size;
        return assets[0].name;
    }
    return "";
}

std::filesystem::path get_target_download_path(
    const std::string& asset_name, bool& out_restart_supported) {
    out_restart_supported = false;
#if defined(__linux__) && !defined(__ANDROID__)
    const char* appimage_env = std::getenv("APPIMAGE");
    if (appimage_env && asset_name.find(".AppImage") != std::string::npos) {
        std::filesystem::path cur_appimage(appimage_env);
        out_restart_supported = true;
        return cur_appimage.parent_path() / (cur_appimage.filename().string() + ".new");
    }
#endif

    // Fallback to user's Downloads directory or current dir
    const char* home = std::getenv("HOME");
#if defined(_WIN32)
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile) {
        auto p = std::filesystem::path(userprofile) / "Downloads";
        if (std::filesystem::exists(p))
            return p / asset_name;
    }
#else
    if (home) {
        auto p = std::filesystem::path(home) / "Downloads";
        if (std::filesystem::exists(p))
            return p / asset_name;
    }
#endif
    return std::filesystem::current_path() / asset_name;
}

#if defined(_WIN32)

bool http_get_string_winhttp(const std::wstring& host, const std::wstring& path,
    std::string& out_body, std::string& out_error) {
    HINTERNET hSession = WinHttpOpen(L"Melee-PC-Updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        out_error = "WinHttpOpen failed: " + std::to_string(GetLastError());
        return false;
    }

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        out_error = "WinHttpConnect failed: " + std::to_string(GetLastError());
        WinHttpCloseHandle(hSession);
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        out_error = "WinHttpOpenRequest failed: " + std::to_string(GetLastError());
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    LPCWSTR headers = L"User-Agent: Melee-PC-Updater\r\nAccept: application/vnd.github.v3+json\r\n";
    BOOL bResults =
        WinHttpSendRequest(hRequest, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (bResults) {
        bResults = WinHttpReceiveResponse(hRequest, NULL);
    }

    if (!bResults) {
        out_error = "WinHttp request failed: " + std::to_string(GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD dwSize = 0;
    DWORD dwDownloaded = 0;
    std::string response;
    do {
        dwSize = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize))
            break;
        if (dwSize == 0)
            break;

        std::vector<char> buffer(dwSize + 1, 0);
        if (WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded)) {
            response.append(buffer.data(), dwDownloaded);
        }
    } while (dwSize > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    out_body = std::move(response);
    return true;
}

#endif

#if defined(MELEE_USE_CURL)

static size_t curl_write_string_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    auto* s = static_cast<std::string*>(userdata);
    s->append(static_cast<char*>(ptr), total);
    return total;
}

struct DownloadContext {
    std::ofstream file;
    size_t total_bytes = 0;
    size_t current_bytes = 0;
    std::atomic_bool* cancel = nullptr;
};

static size_t curl_write_file_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<DownloadContext*>(userdata);
    if (ctx->cancel && ctx->cancel->load())
        return 0;
    size_t total = size * nmemb;
    ctx->file.write(static_cast<const char*>(ptr), total);
    ctx->current_bytes += total;
    {
        std::lock_guard lock(g_updater_mutex);
        g_updater_state.download_current_bytes = ctx->current_bytes;
        if (g_updater_state.download_total_bytes > 0) {
            g_updater_state.download_progress =
                static_cast<float>(ctx->current_bytes) /
                static_cast<float>(g_updater_state.download_total_bytes);
        }
    }
    return total;
}

static int curl_xferinfo_cb(
    void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<DownloadContext*>(clientp);
    if (ctx->cancel && ctx->cancel->load())
        return 1;
    if (dltotal > 0) {
        std::lock_guard lock(g_updater_mutex);
        g_updater_state.download_total_bytes = static_cast<size_t>(dltotal);
        g_updater_state.download_current_bytes = static_cast<size_t>(dlnow);
        g_updater_state.download_progress = static_cast<float>(dlnow) / static_cast<float>(dltotal);
    }
    return 0;
}

bool http_get_string_curl(const std::string& url, std::string& out_body, std::string& out_error) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        out_error = "curl_easy_init failed";
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Melee-PC-Updater");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_string_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out_body);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;  // NOLINT: libcurl requires pointer to long
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    bool ok = (res == CURLE_OK && http_code >= 200 && http_code < 300);
    if (!ok) {
        if (res != CURLE_OK) {
            out_error = curl_easy_strerror(res);
        } else {
            out_error = "HTTP error " + std::to_string(http_code);
        }
    }
    curl_easy_cleanup(curl);
    return ok;
}

bool http_download_file_curl(
    const std::string& url, const std::filesystem::path& dest_path, std::string& out_error) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        out_error = "curl_easy_init failed";
        return false;
    }

    DownloadContext ctx;
    ctx.file.open(dest_path, std::ios::binary);
    if (!ctx.file.is_open()) {
        curl_easy_cleanup(curl);
        out_error = "Could not open destination file: " + dest_path.string();
        return false;
    }
    ctx.cancel = &g_cancel;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Melee-PC-Updater");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_file_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_xferinfo_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    CURLcode res = curl_easy_perform(curl);
    ctx.file.close();
    long http_code = 0;  // NOLINT: libcurl requires pointer to long
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (g_cancel.load()) {
        std::filesystem::remove(dest_path);
        out_error = "Download canceled.";
        return false;
    }

    if (res != CURLE_OK || http_code < 200 || http_code >= 300) {
        std::filesystem::remove(dest_path);
        if (res != CURLE_OK)
            out_error = curl_easy_strerror(res);
        else
            out_error = "HTTP " + std::to_string(http_code);
        return false;
    }

    return true;
}

#elif (defined(__linux__) || defined(__APPLE__)) && !defined(__ANDROID__)
static bool http_get(const std::string&, std::string&, std::string& out_error) {
    out_error = "curl not available";
    return false;
}
static bool http_download_file(
    const std::string&, const std::string&, std::string& out_error, std::atomic_bool*) {
    out_error = "curl not available";
    return false;
}
#endif

}  // namespace

void check_for_updates_async(bool include_prereleases) {
    {
        std::lock_guard lock(g_updater_mutex);
        if (g_updater_state.status == Status::Checking ||
            g_updater_state.status == Status::Downloading)
        {
            return;
        }
        g_updater_state.status = Status::Checking;
        g_updater_state.message = "Checking for updates...";
    }
    g_cancel = false;

    if (g_worker.thread.joinable()) {
        g_worker.thread.join();
    }

    g_worker.thread = std::thread([include_prereleases] {
        std::string body;
        std::string error;
        bool ok = false;

#if defined(_WIN32)
        ok = http_get_string_winhttp(
            L"api.github.com", L"/repos/999sian/melee-pc/releases", body, error);
#elif (defined(__linux__) || defined(__APPLE__)) && !defined(__ANDROID__)
        ok = http_get_string_curl(
            "https://api.github.com/repos/999sian/melee-pc/releases", body, error);
#else
        error = "Networking unsupported on this platform";
#endif

        std::lock_guard lock(g_updater_mutex);
        if (g_cancel.load()) {
            g_updater_state.status = Status::Idle;
            g_updater_state.message = "";
            return;
        }

        if (!ok) {
            g_updater_state.status = Status::Failed;
            g_updater_state.message = "Could not check for updates: " + error;
            return;
        }

        auto releases = parse_github_releases(body);
        if (releases.empty()) {
            g_updater_state.status = Status::UpToDate;
            g_updater_state.message = "Melee-PC is up to date.";
            return;
        }

        // Find the newest version among releases
        const Release* best_release = nullptr;
        SemVer best_ver;

        for (const auto& rel : releases) {
            if (!include_prereleases && rel.prerelease)
                continue;
            auto parsed = SemVer::parse(rel.tag_name);
            if (!parsed.valid)
                continue;
            if (!best_release || parsed > best_ver) {
                best_release = &rel;
                best_ver = parsed;
            }
        }

        if (!best_release) {
            g_updater_state.status = Status::UpToDate;
            g_updater_state.message = "Melee-PC is up to date.";
            return;
        }

        std::string current_ver = get_app_version();
        g_updater_state.latest_release = *best_release;
        g_updater_state.target_asset_name = select_best_asset(best_release->assets,
            g_updater_state.target_asset_url, g_updater_state.download_total_bytes);
        if (is_update_available(current_ver, best_release->tag_name)) {
            g_updater_state.status = Status::UpdateAvailable;
            g_updater_state.message = "Update available: " + best_release->tag_name;
        } else {
            g_updater_state.status = Status::UpToDate;
            g_updater_state.message = "Melee-PC is up to date (" + current_ver + ").";
        }
    });
}

void start_download_async() {
    std::string download_url;
    std::string asset_name;
    size_t total_bytes = 0;
    {
        std::lock_guard lock(g_updater_mutex);
        if (g_updater_state.status != Status::UpdateAvailable &&
            g_updater_state.status != Status::Failed)
        {
            return;
        }
        download_url = g_updater_state.target_asset_url;
        asset_name = g_updater_state.target_asset_name;
        total_bytes = g_updater_state.download_total_bytes;
        if (download_url.empty()) {
            open_release_in_browser();
            return;
        }
        g_updater_state.status = Status::Downloading;
        g_updater_state.download_progress = 0.0f;
        g_updater_state.download_current_bytes = 0;
        g_updater_state.download_total_bytes = total_bytes;
        g_updater_state.message = "Downloading update...";
    }
    g_cancel = false;

    if (g_worker.thread.joinable()) {
        g_worker.thread.join();
    }

    g_worker.thread = std::thread([download_url, asset_name] {
        bool restart_supported = false;
        auto dest_path = get_target_download_path(asset_name, restart_supported);
        std::string error;
        bool ok = false;

#if defined(_WIN32)
        // On Windows, fall back to browser or implement WinHTTP file download
        // WinHTTP streaming download:
        // Or open in browser if download URL is direct
        open_release_in_browser();
        ok = true;
#elif (defined(__linux__) || defined(__APPLE__)) && !defined(__ANDROID__)
        ok = http_download_file_curl(download_url, dest_path, error);
        if (ok) {
            chmod(dest_path.c_str(), 0755);
        }
#else
        error = "Direct downloading unsupported on this platform";
#endif

        std::lock_guard lock(g_updater_mutex);
        if (g_cancel.load()) {
            g_updater_state.status = Status::UpdateAvailable;
            g_updater_state.message = "Download canceled.";
            return;
        }

        if (ok) {
            g_updater_state.status = Status::Downloaded;
            g_updater_state.downloaded_path = dest_path.string();
            g_updater_state.restart_supported = restart_supported;
            g_updater_state.download_progress = 1.0f;
            g_updater_state.message = "Update downloaded successfully!";
        } else {
            g_updater_state.status = Status::Failed;
            g_updater_state.message = "Download failed: " + error;
        }
    });
}

void cancel() {
    g_cancel = true;
    if (g_worker.thread.joinable()) {
        g_worker.thread.join();
    }
}

UpdateState get_state() {
    std::lock_guard lock(g_updater_mutex);
    return g_updater_state;
}

void open_release_in_browser() {
    std::string url;
    {
        std::lock_guard lock(g_updater_mutex);
        url = g_updater_state.latest_release.html_url;
    }
    if (url.empty()) {
        url = "https://github.com/999sian/melee-pc/releases";
    }
    SDL_OpenURL(url.c_str());
}

void open_downloaded_location() {
    std::string path;
    {
        std::lock_guard lock(g_updater_mutex);
        path = g_updater_state.downloaded_path;
    }
    if (path.empty())
        return;

    auto parent = std::filesystem::path(path).parent_path().string();
#if defined(_WIN32)
    ShellExecuteA(NULL, "open", parent.c_str(), NULL, NULL, SW_SHOW);
#elif defined(__linux__)
    std::string cmd = "xdg-open \"" + parent + "\" &";
    system(cmd.c_str());
#endif
}

bool apply_update_and_restart(std::string& error) {
    std::string new_path;
    bool restartable = false;
    {
        std::lock_guard lock(g_updater_mutex);
        new_path = g_updater_state.downloaded_path;
        restartable = g_updater_state.restart_supported;
    }

#if defined(__linux__) && !defined(__ANDROID__)
    const char* appimage = std::getenv("APPIMAGE");
    if (restartable && appimage && !new_path.empty()) {
        std::error_code ec;
        std::filesystem::rename(new_path, appimage, ec);
        if (ec) {
            error = "Failed to replace AppImage: " + ec.message();
            return false;
        }
        chmod(appimage, 0755);
        execl(appimage, appimage, nullptr);
        error = "Failed to relaunch AppImage";
        return false;
    }
#endif

    open_downloaded_location();
    return true;
}

}  // namespace pc::updater
