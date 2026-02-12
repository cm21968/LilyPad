#pragma once

#include "app_state.h"
#include <string>

void do_connect(AppState& app, const std::string& server_ip, const std::string& username);
void do_join_voice(AppState& app, int input_device, int output_device);
void do_leave_voice(AppState& app);
void do_disconnect(AppState& app);
