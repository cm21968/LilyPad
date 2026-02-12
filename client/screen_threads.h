#pragma once

#include "app_state.h"

void screen_decode_thread_func(AppState& app);
void screen_send_thread_func(AppState& app);
void screen_capture_thread_func(AppState& app);
void sys_audio_capture_thread_func(AppState& app);
