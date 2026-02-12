#pragma once

#include "app_state.h"
#include <string>
#include <vector>

// ── File paths ──
std::string get_lilypad_dir();
std::string get_favorites_path();
std::string get_settings_path();
std::string get_chat_cache_dir(const std::string& server_ip);
std::string get_chat_cache_path(const std::string& server_ip);

// ── Favorites ──
std::vector<ServerFavorite> load_favorites();
void save_favorites(const std::vector<ServerFavorite>& favs);

// ── Settings ──
AppSettings load_settings();
void save_settings(const AppSettings& s);
