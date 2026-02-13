#include "persistence.h"

#include <shlobj.h>

#include <fstream>
#include <iomanip>
#include <sstream>

std::string get_lilypad_dir() {
    PWSTR documents = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents))) {
        int len = WideCharToMultiByte(CP_UTF8, 0, documents, -1, nullptr, 0, nullptr, nullptr);
        std::string path(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, documents, -1, path.data(), len, nullptr, nullptr);
        CoTaskMemFree(documents);
        path += "\\LilyPad";
        CreateDirectoryA(path.c_str(), nullptr);
        return path;
    }
    return "";
}

std::string get_favorites_path() {
    PWSTR documents = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents))) {
        int len = WideCharToMultiByte(CP_UTF8, 0, documents, -1, nullptr, 0, nullptr, nullptr);
        std::string path(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, documents, -1, path.data(), len, nullptr, nullptr);
        CoTaskMemFree(documents);

        path += "\\LilyPad";
        CreateDirectoryA(path.c_str(), nullptr);
        return path + "\\favorites.txt";
    }
    return "";
}

// File format: one server per line, tab-separated: name\tip\tusername
std::vector<ServerFavorite> load_favorites() {
    std::vector<ServerFavorite> favs;
    std::string path = get_favorites_path();
    if (path.empty()) return favs;

    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        auto tab1 = line.find('\t');
        if (tab1 != std::string::npos) {
            auto tab2 = line.find('\t', tab1 + 1);
            std::string name = line.substr(0, tab1);
            std::string ip, user;
            if (tab2 != std::string::npos) {
                ip   = line.substr(tab1 + 1, tab2 - tab1 - 1);
                user = line.substr(tab2 + 1);
            } else {
                ip = line.substr(tab1 + 1);
            }
            favs.push_back({name, ip, user});
        } else {
            favs.push_back({line, line, ""});
        }
    }
    return favs;
}

void save_favorites(const std::vector<ServerFavorite>& favs) {
    std::string path = get_favorites_path();
    if (path.empty()) return;

    std::ofstream file(path, std::ios::trunc);
    for (auto& f : favs) {
        file << f.name << '\t' << f.ip << '\t' << f.username << '\n';
    }
}

std::string get_settings_path() {
    PWSTR documents = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents))) {
        int len = WideCharToMultiByte(CP_UTF8, 0, documents, -1, nullptr, 0, nullptr, nullptr);
        std::string path(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, documents, -1, path.data(), len, nullptr, nullptr);
        CoTaskMemFree(documents);

        path += "\\LilyPad";
        CreateDirectoryA(path.c_str(), nullptr);
        return path + "\\settings.txt";
    }
    return "";
}

AppSettings load_settings() {
    AppSettings s;
    std::string path = get_settings_path();
    if (path.empty()) return s;

    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "auto_connect") s.auto_connect = (val == "1");
        else if (key == "last_server_ip") s.last_server_ip = val;
        else if (key == "last_username") s.last_username = val;
    }
    return s;
}

void save_settings(const AppSettings& s) {
    std::string path = get_settings_path();
    if (path.empty()) return;

    std::ofstream file(path, std::ios::trunc);
    file << "auto_connect=" << (s.auto_connect ? "1" : "0") << '\n';
    file << "last_server_ip=" << s.last_server_ip << '\n';
    file << "last_username=" << s.last_username << '\n';
}

std::string get_chat_cache_dir(const std::string& server_ip) {
    std::string base = get_lilypad_dir();
    if (base.empty()) return "";
    std::string cache_dir = base + "\\cache";
    CreateDirectoryA(cache_dir.c_str(), nullptr);
    // Sanitize IP for directory name (replace : with _)
    std::string safe_ip = server_ip;
    for (char& c : safe_ip) {
        if (c == ':' || c == '/' || c == '\\') c = '_';
    }
    std::string server_dir = cache_dir + "\\" + safe_ip;
    CreateDirectoryA(server_dir.c_str(), nullptr);
    return server_dir;
}

std::string get_chat_cache_path(const std::string& server_ip) {
    std::string dir = get_chat_cache_dir(server_ip);
    if (dir.empty()) return "";
    return dir + "\\chat.jsonl";
}

// ── Session token persistence ──

static std::string get_sessions_dir() {
    std::string base = get_lilypad_dir();
    if (base.empty()) return "";
    std::string dir = base + "\\sessions";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

static std::string sanitize_ip(const std::string& ip) {
    std::string safe = ip;
    for (char& c : safe) {
        if (c == ':' || c == '/' || c == '\\' || c == '.') c = '_';
    }
    return safe;
}

static std::string get_session_path(const std::string& server_ip) {
    std::string dir = get_sessions_dir();
    if (dir.empty()) return "";
    return dir + "\\" + sanitize_ip(server_ip) + ".token";
}

SavedSession load_session(const std::string& server_ip) {
    SavedSession session;
    std::string path = get_session_path(server_ip);
    if (path.empty()) return session;

    std::ifstream file(path);
    if (!file.is_open()) return session;

    // Format: line 1 = username, line 2 = hex-encoded 32-byte token
    std::string username, token_hex;
    std::getline(file, username);
    std::getline(file, token_hex);

    if (username.empty() || token_hex.size() != 64) return session;

    session.username = username;
    session.token.resize(32);
    for (int i = 0; i < 32; ++i) {
        unsigned int byte;
        std::istringstream iss(token_hex.substr(i * 2, 2));
        iss >> std::hex >> byte;
        session.token[i] = static_cast<uint8_t>(byte);
    }
    session.valid = true;
    return session;
}

void save_session(const std::string& server_ip, const std::string& username, const uint8_t* token) {
    std::string path = get_session_path(server_ip);
    if (path.empty()) return;

    std::ofstream file(path, std::ios::trunc);
    file << username << '\n';
    for (int i = 0; i < 32; ++i) {
        file << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(token[i]);
    }
    file << '\n';
}

void clear_session(const std::string& server_ip) {
    std::string path = get_session_path(server_ip);
    if (!path.empty()) {
        DeleteFileA(path.c_str());
    }
}
