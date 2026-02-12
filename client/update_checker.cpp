#include "update_checker.h"
#include <wininet.h>
#include <string>

void check_for_update_thread(AppState& app) {
    HINTERNET inet = InternetOpenA("LilyPad", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!inet) return;

    HINTERNET url = InternetOpenUrlA(inet, UPDATE_CHECK_URL, nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!url) {
        InternetCloseHandle(inet);
        return;
    }

    // Read response (version.txt is tiny -- a few hundred bytes at most)
    std::string body;
    char buf[1024];
    DWORD bytes_read = 0;
    while (InternetReadFile(url, buf, sizeof(buf), &bytes_read) && bytes_read > 0) {
        body.append(buf, bytes_read);
        if (body.size() > 4096) break; // safety limit
    }

    InternetCloseHandle(url);
    InternetCloseHandle(inet);

    if (body.empty()) return;

    // Parse: line 1 = version, line 2 = download URL
    auto nl = body.find('\n');
    if (nl == std::string::npos) return;

    std::string version = body.substr(0, nl);
    // Trim carriage return if present
    if (!version.empty() && version.back() == '\r') version.pop_back();

    std::string dl_url = body.substr(nl + 1);
    // Trim trailing whitespace/newlines
    while (!dl_url.empty() && (dl_url.back() == '\r' || dl_url.back() == '\n' || dl_url.back() == ' '))
        dl_url.pop_back();

    if (version.empty() || dl_url.empty()) return;

    if (is_newer_version(APP_VERSION, version)) {
        std::lock_guard<std::mutex> lk(app.update_mutex);
        app.update_version = version;
        app.update_url     = dl_url;
        app.update_available = true;
    }
}
