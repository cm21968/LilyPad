#pragma once

#include "app_state.h"
#include <string>

// TLS connect only (no auth) — sets auth_state to CONNECTED_UNAUTH on success
void do_tls_connect(AppState& app, const std::string& server_ip);

// Login with username/password — sends AUTH_LOGIN_REQ, processes response
// On success: sets auth_state to AUTHENTICATED, starts threads
void do_login(AppState& app, const std::string& username, const std::string& password, bool remember_me);

// Register a new account — sends AUTH_REGISTER_REQ, processes response
void do_register(AppState& app, const std::string& username, const std::string& password);

// Login with saved session token — sends AUTH_TOKEN_LOGIN_REQ
void do_token_login(AppState& app, const std::string& username, const uint8_t* token);

// Change password (while authenticated)
void do_change_password(AppState& app, const std::string& old_pass, const std::string& new_pass);

// Delete account (while authenticated)
void do_delete_account(AppState& app, const std::string& password);

// Logout (invalidates session, disconnects)
void do_logout(AppState& app);

// Voice channel
void do_join_voice(AppState& app, int input_device, int output_device);
void do_leave_voice(AppState& app);

// Full disconnect (cleanup)
void do_disconnect(AppState& app);
