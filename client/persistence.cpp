#include "persistence.h"

#include <shlobj.h>

#include <fstream>

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
