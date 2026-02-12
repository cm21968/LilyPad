#pragma once

#include "app_state.h"

void tcp_receive_thread(AppState& app);
void voice_send_thread(AppState& app);
void udp_receive_thread_func(AppState& app);
void audio_playback_thread_func(AppState& app);
