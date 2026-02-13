#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace lilypad {

// ── TCP signaling message types ──
enum class MsgType : uint8_t {
    JOIN         = 0x01,  // Client→Server: username
    WELCOME      = 0x02,  // Server→Client: client_id + udp_port
    USER_JOINED  = 0x03,  // Server→All:    client_id + username
    USER_LEFT    = 0x04,  // Server→All:    client_id
    LEAVE        = 0x05,  // Client→Server: (empty)
    TEXT_CHAT    = 0x06,  // Client→Server: text / Server→All: client_id + text

    // Screen sharing
    SCREEN_START       = 0x07,  // Client→Server: empty; Server→All: sharer_id(4)
    SCREEN_STOP        = 0x08,  // Client→Server: empty; Server→All: sharer_id(4)
    SCREEN_SUBSCRIBE   = 0x09,  // Client→Server: target_id(4)
    SCREEN_UNSUBSCRIBE = 0x0A,  // Client→Server: target_id(4)
    SCREEN_FRAME       = 0x0B,  // Client→Server: width(2)+height(2)+flags(1)+h264
                                // Server→Subscribers: sharer_id(4)+width(2)+height(2)+flags(1)+h264
    SCREEN_AUDIO       = 0x0C,  // Client→Server: opus_data
                                // Server→Subscribers: sharer_id(4)+opus_data

    UPDATE_AVAILABLE   = 0x0D,  // Server→Client: version\0url\0

    // Voice channel (separate from text chat)
    VOICE_JOIN     = 0x0E,  // Client→Server: empty payload
    VOICE_LEAVE    = 0x0F,  // Client→Server: empty payload
    VOICE_JOINED   = 0x10,  // Server→All: client_id(4)
    VOICE_LEFT     = 0x11,  // Server→All: client_id(4)

    // Chat sync (persistent chat)
    CHAT_SYNC      = 0x12,  // Client→Server: last_known_seq(8)

    SCREEN_REQUEST_KEYFRAME = 0x13,  // Server→Client: empty payload (request IDR)

    // Authentication
    AUTH_REGISTER_REQ     = 0x20,  // C->S: username\0 + password\0
    AUTH_REGISTER_RESP    = 0x21,  // S->C: status(1) + message\0
    AUTH_LOGIN_REQ        = 0x22,  // C->S: username\0 + password\0
    AUTH_LOGIN_RESP       = 0x23,  // S->C: status(1) + client_id(4) + udp_port(2) + token(32) + message\0
    AUTH_TOKEN_LOGIN_REQ  = 0x24,  // C->S: username\0 + token(32)
    AUTH_TOKEN_LOGIN_RESP = 0x25,  // S->C: same as AUTH_LOGIN_RESP
    AUTH_CHANGE_PASS_REQ  = 0x26,  // C->S: old_password\0 + new_password\0
    AUTH_CHANGE_PASS_RESP = 0x27,  // S->C: status(1) + message\0
    AUTH_DELETE_ACCT_REQ  = 0x28,  // C->S: password\0
    AUTH_DELETE_ACCT_RESP = 0x29,  // S->C: status(1) + message\0
    AUTH_LOGOUT           = 0x2A,  // C->S: empty
};

enum class AuthStatus : uint8_t {
    OK                 = 0x00,
    ERR_USERNAME_TAKEN = 0x01,
    ERR_INVALID_CREDS  = 0x02,
    ERR_TOKEN_EXPIRED  = 0x03,
    ERR_RATE_LIMITED   = 0x04,
    ERR_INVALID_INPUT  = 0x05,
    ERR_INTERNAL       = 0x06,
};

// ── TCP signal header: [msg_type:1][payload_len:4] = 5 bytes ──
constexpr size_t SIGNAL_HEADER_SIZE = 5;
constexpr size_t MAX_USERNAME_LEN   = 32;

struct SignalHeader {
    MsgType  type;
    uint32_t payload_len;
};

inline std::vector<uint8_t> serialize_header(const SignalHeader& h) {
    std::vector<uint8_t> buf(SIGNAL_HEADER_SIZE);
    buf[0] = static_cast<uint8_t>(h.type);
    buf[1] = static_cast<uint8_t>(h.payload_len & 0xFF);
    buf[2] = static_cast<uint8_t>((h.payload_len >> 8) & 0xFF);
    buf[3] = static_cast<uint8_t>((h.payload_len >> 16) & 0xFF);
    buf[4] = static_cast<uint8_t>((h.payload_len >> 24) & 0xFF);
    return buf;
}

inline SignalHeader deserialize_header(const uint8_t* data) {
    SignalHeader h;
    h.type        = static_cast<MsgType>(data[0]);
    h.payload_len = static_cast<uint32_t>(data[1])       |
                    (static_cast<uint32_t>(data[2]) << 8)  |
                    (static_cast<uint32_t>(data[3]) << 16) |
                    (static_cast<uint32_t>(data[4]) << 24);
    return h;
}

// ── Helpers to build complete TCP messages ──

inline std::vector<uint8_t> make_join_msg(const std::string& username) {
    std::string name = username.substr(0, MAX_USERNAME_LEN);
    uint32_t len = static_cast<uint32_t>(name.size() + 1); // include null terminator
    SignalHeader h{MsgType::JOIN, len};
    auto buf = serialize_header(h);
    buf.insert(buf.end(), name.begin(), name.end());
    buf.push_back('\0');
    return buf;
}

inline std::vector<uint8_t> make_welcome_msg(uint32_t client_id, uint16_t udp_port) {
    SignalHeader h{MsgType::WELCOME, 6}; // 4 + 2
    auto buf = serialize_header(h);
    buf.push_back(static_cast<uint8_t>(client_id & 0xFF));
    buf.push_back(static_cast<uint8_t>((client_id >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((client_id >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((client_id >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>(udp_port & 0xFF));
    buf.push_back(static_cast<uint8_t>((udp_port >> 8) & 0xFF));
    return buf;
}

inline std::vector<uint8_t> make_user_joined_msg(uint32_t client_id, const std::string& username) {
    std::string name = username.substr(0, MAX_USERNAME_LEN);
    uint32_t len = static_cast<uint32_t>(4 + name.size() + 1);
    SignalHeader h{MsgType::USER_JOINED, len};
    auto buf = serialize_header(h);
    buf.push_back(static_cast<uint8_t>(client_id & 0xFF));
    buf.push_back(static_cast<uint8_t>((client_id >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((client_id >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((client_id >> 24) & 0xFF));
    buf.insert(buf.end(), name.begin(), name.end());
    buf.push_back('\0');
    return buf;
}

inline std::vector<uint8_t> make_user_left_msg(uint32_t client_id) {
    SignalHeader h{MsgType::USER_LEFT, 4};
    auto buf = serialize_header(h);
    buf.push_back(static_cast<uint8_t>(client_id & 0xFF));
    buf.push_back(static_cast<uint8_t>((client_id >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((client_id >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((client_id >> 24) & 0xFF));
    return buf;
}

inline std::vector<uint8_t> make_leave_msg() {
    SignalHeader h{MsgType::LEAVE, 0};
    return serialize_header(h);
}

// Client→Server: just the text (null-terminated)
constexpr size_t MAX_CHAT_LEN = 512;

inline std::vector<uint8_t> make_text_chat_msg(const std::string& text) {
    std::string t = text.substr(0, MAX_CHAT_LEN);
    uint32_t len = static_cast<uint32_t>(t.size() + 1);
    SignalHeader h{MsgType::TEXT_CHAT, len};
    auto buf = serialize_header(h);
    buf.insert(buf.end(), t.begin(), t.end());
    buf.push_back('\0');
    return buf;
}

// Server→All: client_id (4) + text (null-terminated)
inline std::vector<uint8_t> make_text_chat_broadcast_msg(uint32_t client_id, const std::string& text) {
    std::string t = text.substr(0, MAX_CHAT_LEN);
    uint32_t len = static_cast<uint32_t>(4 + t.size() + 1);
    SignalHeader h{MsgType::TEXT_CHAT, len};
    auto buf = serialize_header(h);
    buf.push_back(static_cast<uint8_t>(client_id & 0xFF));
    buf.push_back(static_cast<uint8_t>((client_id >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((client_id >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((client_id >> 24) & 0xFF));
    buf.insert(buf.end(), t.begin(), t.end());
    buf.push_back('\0');
    return buf;
}

// ── Parse helpers for payloads ──

inline uint32_t read_u32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0])       |
           (static_cast<uint32_t>(data[1]) << 8)  |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

inline uint16_t read_u16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8);
}

inline uint64_t read_u64(const uint8_t* data) {
    return static_cast<uint64_t>(data[0])        |
           (static_cast<uint64_t>(data[1]) << 8)  |
           (static_cast<uint64_t>(data[2]) << 16) |
           (static_cast<uint64_t>(data[3]) << 24) |
           (static_cast<uint64_t>(data[4]) << 32) |
           (static_cast<uint64_t>(data[5]) << 40) |
           (static_cast<uint64_t>(data[6]) << 48) |
           (static_cast<uint64_t>(data[7]) << 56);
}

inline void write_u64(std::vector<uint8_t>& buf, uint64_t val) {
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 32) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 40) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 48) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 56) & 0xFF));
}

inline void write_u32(std::vector<uint8_t>& buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
}

// ── UDP voice packet: [client_id:4][sequence:4][opus_data:variable] ──
constexpr size_t VOICE_HEADER_SIZE = 8;
constexpr size_t MAX_VOICE_PACKET  = 1400; // safe for MTU

struct VoicePacket {
    uint32_t             client_id = 0;
    uint32_t             sequence  = 0;
    std::vector<uint8_t> opus_data;

    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> buf(VOICE_HEADER_SIZE + opus_data.size());
        buf[0] = static_cast<uint8_t>(client_id & 0xFF);
        buf[1] = static_cast<uint8_t>((client_id >> 8) & 0xFF);
        buf[2] = static_cast<uint8_t>((client_id >> 16) & 0xFF);
        buf[3] = static_cast<uint8_t>((client_id >> 24) & 0xFF);
        buf[4] = static_cast<uint8_t>(sequence & 0xFF);
        buf[5] = static_cast<uint8_t>((sequence >> 8) & 0xFF);
        buf[6] = static_cast<uint8_t>((sequence >> 16) & 0xFF);
        buf[7] = static_cast<uint8_t>((sequence >> 24) & 0xFF);
        std::memcpy(buf.data() + VOICE_HEADER_SIZE, opus_data.data(), opus_data.size());
        return buf;
    }

    static VoicePacket from_bytes(const uint8_t* data, size_t len) {
        VoicePacket pkt;
        pkt.client_id = read_u32(data);
        pkt.sequence  = read_u32(data + 4);
        if (len > VOICE_HEADER_SIZE) {
            pkt.opus_data.assign(data + VOICE_HEADER_SIZE, data + len);
        }
        return pkt;
    }
};

// ── Screen sharing message helpers ──

// Client→Server: empty payload (server knows sender from connection)
inline std::vector<uint8_t> make_screen_start_msg() {
    SignalHeader h{MsgType::SCREEN_START, 0};
    return serialize_header(h);
}

// Server→All: sharer_id(4)
inline std::vector<uint8_t> make_screen_start_broadcast(uint32_t sharer_id) {
    SignalHeader h{MsgType::SCREEN_START, 4};
    auto buf = serialize_header(h);
    buf.push_back(static_cast<uint8_t>(sharer_id & 0xFF));
    buf.push_back(static_cast<uint8_t>((sharer_id >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((sharer_id >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((sharer_id >> 24) & 0xFF));
    return buf;
}

// Client→Server: empty payload
inline std::vector<uint8_t> make_screen_stop_msg() {
    SignalHeader h{MsgType::SCREEN_STOP, 0};
    return serialize_header(h);
}

// Server→All: sharer_id(4)
inline std::vector<uint8_t> make_screen_stop_broadcast(uint32_t sharer_id) {
    SignalHeader h{MsgType::SCREEN_STOP, 4};
    auto buf = serialize_header(h);
    buf.push_back(static_cast<uint8_t>(sharer_id & 0xFF));
    buf.push_back(static_cast<uint8_t>((sharer_id >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((sharer_id >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((sharer_id >> 24) & 0xFF));
    return buf;
}

// Client→Server: target_id(4)
inline std::vector<uint8_t> make_screen_subscribe_msg(uint32_t target_id) {
    SignalHeader h{MsgType::SCREEN_SUBSCRIBE, 4};
    auto buf = serialize_header(h);
    buf.push_back(static_cast<uint8_t>(target_id & 0xFF));
    buf.push_back(static_cast<uint8_t>((target_id >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((target_id >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((target_id >> 24) & 0xFF));
    return buf;
}

// Client→Server: target_id(4)
inline std::vector<uint8_t> make_screen_unsubscribe_msg(uint32_t target_id) {
    SignalHeader h{MsgType::SCREEN_UNSUBSCRIBE, 4};
    auto buf = serialize_header(h);
    buf.push_back(static_cast<uint8_t>(target_id & 0xFF));
    buf.push_back(static_cast<uint8_t>((target_id >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((target_id >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((target_id >> 24) & 0xFF));
    return buf;
}

// Screen frame flags bitmask
constexpr uint8_t SCREEN_FLAG_KEYFRAME = 0x01;  // Bit 0: IDR keyframe

// Client→Server: width(2) + height(2) + flags(1) + h264_data
inline std::vector<uint8_t> make_screen_frame_msg(uint16_t width, uint16_t height,
                                                   uint8_t flags,
                                                   const uint8_t* data, size_t data_len) {
    uint32_t payload_len = static_cast<uint32_t>(5 + data_len); // 2+2+1+data
    SignalHeader h{MsgType::SCREEN_FRAME, payload_len};
    auto buf = serialize_header(h);
    buf.push_back(static_cast<uint8_t>(width & 0xFF));
    buf.push_back(static_cast<uint8_t>((width >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(height & 0xFF));
    buf.push_back(static_cast<uint8_t>((height >> 8) & 0xFF));
    buf.push_back(flags);
    buf.insert(buf.end(), data, data + data_len);
    return buf;
}

// Server→Subscribers: sharer_id(4) + width(2) + height(2) + flags(1) + h264_data
inline std::vector<uint8_t> make_screen_frame_relay(uint32_t sharer_id, uint16_t width,
                                                     uint16_t height, uint8_t flags,
                                                     const uint8_t* data,
                                                     size_t data_len) {
    uint32_t payload_len = static_cast<uint32_t>(9 + data_len); // 4+2+2+1+data
    SignalHeader h{MsgType::SCREEN_FRAME, payload_len};
    auto buf = serialize_header(h);
    buf.push_back(static_cast<uint8_t>(sharer_id & 0xFF));
    buf.push_back(static_cast<uint8_t>((sharer_id >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((sharer_id >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((sharer_id >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>(width & 0xFF));
    buf.push_back(static_cast<uint8_t>((width >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(height & 0xFF));
    buf.push_back(static_cast<uint8_t>((height >> 8) & 0xFF));
    buf.push_back(flags);
    buf.insert(buf.end(), data, data + data_len);
    return buf;
}

// Server→Client: request keyframe from sharer (empty payload)
inline std::vector<uint8_t> make_screen_request_keyframe_msg() {
    SignalHeader h{MsgType::SCREEN_REQUEST_KEYFRAME, 0};
    return serialize_header(h);
}

// ── System audio (screen sharing audio) message helpers ──

// Client→Server: opus_data
inline std::vector<uint8_t> make_screen_audio_msg(const uint8_t* opus_data, size_t opus_len) {
    uint32_t payload_len = static_cast<uint32_t>(opus_len);
    SignalHeader h{MsgType::SCREEN_AUDIO, payload_len};
    auto buf = serialize_header(h);
    buf.insert(buf.end(), opus_data, opus_data + opus_len);
    return buf;
}

// Server→Subscribers: sharer_id(4) + opus_data
inline std::vector<uint8_t> make_screen_audio_relay(uint32_t sharer_id,
                                                     const uint8_t* opus_data, size_t opus_len) {
    uint32_t payload_len = static_cast<uint32_t>(4 + opus_len);
    SignalHeader h{MsgType::SCREEN_AUDIO, payload_len};
    auto buf = serialize_header(h);
    buf.push_back(static_cast<uint8_t>(sharer_id & 0xFF));
    buf.push_back(static_cast<uint8_t>((sharer_id >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((sharer_id >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((sharer_id >> 24) & 0xFF));
    buf.insert(buf.end(), opus_data, opus_data + opus_len);
    return buf;
}

// ── Voice channel message helpers ──

// Client→Server: empty payload
inline std::vector<uint8_t> make_voice_join_msg() {
    SignalHeader h{MsgType::VOICE_JOIN, 0};
    return serialize_header(h);
}

// Client→Server: empty payload
inline std::vector<uint8_t> make_voice_leave_msg() {
    SignalHeader h{MsgType::VOICE_LEAVE, 0};
    return serialize_header(h);
}

// Server→All: client_id(4)
inline std::vector<uint8_t> make_voice_joined_broadcast(uint32_t client_id) {
    SignalHeader h{MsgType::VOICE_JOINED, 4};
    auto buf = serialize_header(h);
    write_u32(buf, client_id);
    return buf;
}

// Server→All: client_id(4)
inline std::vector<uint8_t> make_voice_left_broadcast(uint32_t client_id) {
    SignalHeader h{MsgType::VOICE_LEFT, 4};
    auto buf = serialize_header(h);
    write_u32(buf, client_id);
    return buf;
}

// Client→Server: last_known_seq(8)
inline std::vector<uint8_t> make_chat_sync_msg(uint64_t last_seq) {
    SignalHeader h{MsgType::CHAT_SYNC, 8};
    auto buf = serialize_header(h);
    write_u64(buf, last_seq);
    return buf;
}

// Server→All (v2): seq(8) + client_id(4) + timestamp(8) + sender_name\0 + text\0
inline std::vector<uint8_t> make_text_chat_broadcast_v2(uint64_t seq, uint32_t client_id,
                                                         int64_t timestamp,
                                                         const std::string& sender_name,
                                                         const std::string& text) {
    std::string t = text.substr(0, MAX_CHAT_LEN);
    std::string name = sender_name.substr(0, MAX_USERNAME_LEN);
    uint32_t len = static_cast<uint32_t>(8 + 4 + 8 + name.size() + 1 + t.size() + 1);
    SignalHeader h{MsgType::TEXT_CHAT, len};
    auto buf = serialize_header(h);
    write_u64(buf, seq);
    write_u32(buf, client_id);
    write_u64(buf, static_cast<uint64_t>(timestamp));
    buf.insert(buf.end(), name.begin(), name.end());
    buf.push_back('\0');
    buf.insert(buf.end(), t.begin(), t.end());
    buf.push_back('\0');
    return buf;
}

// ── Update notification ──

// Server→Client: version (null-terminated) + url (null-terminated)
inline std::vector<uint8_t> make_update_available_msg(const std::string& version,
                                                       const std::string& url) {
    uint32_t payload_len = static_cast<uint32_t>(version.size() + 1 + url.size() + 1);
    SignalHeader h{MsgType::UPDATE_AVAILABLE, payload_len};
    auto buf = serialize_header(h);
    buf.insert(buf.end(), version.begin(), version.end());
    buf.push_back('\0');
    buf.insert(buf.end(), url.begin(), url.end());
    buf.push_back('\0');
    return buf;
}

// ── Authentication message helpers ──

constexpr size_t SESSION_TOKEN_SIZE = 32;
constexpr size_t MAX_PASSWORD_LEN   = 128;
constexpr size_t MIN_PASSWORD_LEN   = 8;

// C->S: username\0 + password\0
inline std::vector<uint8_t> make_auth_register_req(const std::string& username, const std::string& password) {
    std::string name = username.substr(0, MAX_USERNAME_LEN);
    std::string pass = password.substr(0, MAX_PASSWORD_LEN);
    uint32_t len = static_cast<uint32_t>(name.size() + 1 + pass.size() + 1);
    SignalHeader h{MsgType::AUTH_REGISTER_REQ, len};
    auto buf = serialize_header(h);
    buf.insert(buf.end(), name.begin(), name.end());
    buf.push_back('\0');
    buf.insert(buf.end(), pass.begin(), pass.end());
    buf.push_back('\0');
    return buf;
}

// S->C: status(1) + message\0
inline std::vector<uint8_t> make_auth_register_resp(AuthStatus status, const std::string& message) {
    uint32_t len = static_cast<uint32_t>(1 + message.size() + 1);
    SignalHeader h{MsgType::AUTH_REGISTER_RESP, len};
    auto buf = serialize_header(h);
    buf.push_back(static_cast<uint8_t>(status));
    buf.insert(buf.end(), message.begin(), message.end());
    buf.push_back('\0');
    return buf;
}

// C->S: username\0 + password\0
inline std::vector<uint8_t> make_auth_login_req(const std::string& username, const std::string& password) {
    std::string name = username.substr(0, MAX_USERNAME_LEN);
    std::string pass = password.substr(0, MAX_PASSWORD_LEN);
    uint32_t len = static_cast<uint32_t>(name.size() + 1 + pass.size() + 1);
    SignalHeader h{MsgType::AUTH_LOGIN_REQ, len};
    auto buf = serialize_header(h);
    buf.insert(buf.end(), name.begin(), name.end());
    buf.push_back('\0');
    buf.insert(buf.end(), pass.begin(), pass.end());
    buf.push_back('\0');
    return buf;
}

// S->C: status(1) + client_id(4) + udp_port(2) + token(32) + message\0
inline std::vector<uint8_t> make_auth_login_resp(AuthStatus status, uint32_t client_id,
                                                  uint16_t udp_port, const uint8_t* token,
                                                  const std::string& message) {
    uint32_t len = static_cast<uint32_t>(1 + 4 + 2 + SESSION_TOKEN_SIZE + message.size() + 1);
    SignalHeader h{MsgType::AUTH_LOGIN_RESP, len};
    auto buf = serialize_header(h);
    buf.push_back(static_cast<uint8_t>(status));
    write_u32(buf, client_id);
    buf.push_back(static_cast<uint8_t>(udp_port & 0xFF));
    buf.push_back(static_cast<uint8_t>((udp_port >> 8) & 0xFF));
    buf.insert(buf.end(), token, token + SESSION_TOKEN_SIZE);
    buf.insert(buf.end(), message.begin(), message.end());
    buf.push_back('\0');
    return buf;
}

// C->S: username\0 + token(32)
inline std::vector<uint8_t> make_auth_token_login_req(const std::string& username, const uint8_t* token) {
    std::string name = username.substr(0, MAX_USERNAME_LEN);
    uint32_t len = static_cast<uint32_t>(name.size() + 1 + SESSION_TOKEN_SIZE);
    SignalHeader h{MsgType::AUTH_TOKEN_LOGIN_REQ, len};
    auto buf = serialize_header(h);
    buf.insert(buf.end(), name.begin(), name.end());
    buf.push_back('\0');
    buf.insert(buf.end(), token, token + SESSION_TOKEN_SIZE);
    return buf;
}

// S->C: same format as AUTH_LOGIN_RESP
inline std::vector<uint8_t> make_auth_token_login_resp(AuthStatus status, uint32_t client_id,
                                                        uint16_t udp_port, const uint8_t* token,
                                                        const std::string& message) {
    uint32_t len = static_cast<uint32_t>(1 + 4 + 2 + SESSION_TOKEN_SIZE + message.size() + 1);
    SignalHeader h{MsgType::AUTH_TOKEN_LOGIN_RESP, len};
    auto buf = serialize_header(h);
    buf.push_back(static_cast<uint8_t>(status));
    write_u32(buf, client_id);
    buf.push_back(static_cast<uint8_t>(udp_port & 0xFF));
    buf.push_back(static_cast<uint8_t>((udp_port >> 8) & 0xFF));
    buf.insert(buf.end(), token, token + SESSION_TOKEN_SIZE);
    buf.insert(buf.end(), message.begin(), message.end());
    buf.push_back('\0');
    return buf;
}

// C->S: old_password\0 + new_password\0
inline std::vector<uint8_t> make_auth_change_pass_req(const std::string& old_pass, const std::string& new_pass) {
    uint32_t len = static_cast<uint32_t>(old_pass.size() + 1 + new_pass.size() + 1);
    SignalHeader h{MsgType::AUTH_CHANGE_PASS_REQ, len};
    auto buf = serialize_header(h);
    buf.insert(buf.end(), old_pass.begin(), old_pass.end());
    buf.push_back('\0');
    buf.insert(buf.end(), new_pass.begin(), new_pass.end());
    buf.push_back('\0');
    return buf;
}

// S->C: status(1) + message\0
inline std::vector<uint8_t> make_auth_change_pass_resp(AuthStatus status, const std::string& message) {
    uint32_t len = static_cast<uint32_t>(1 + message.size() + 1);
    SignalHeader h{MsgType::AUTH_CHANGE_PASS_RESP, len};
    auto buf = serialize_header(h);
    buf.push_back(static_cast<uint8_t>(status));
    buf.insert(buf.end(), message.begin(), message.end());
    buf.push_back('\0');
    return buf;
}

// C->S: password\0
inline std::vector<uint8_t> make_auth_delete_acct_req(const std::string& password) {
    uint32_t len = static_cast<uint32_t>(password.size() + 1);
    SignalHeader h{MsgType::AUTH_DELETE_ACCT_REQ, len};
    auto buf = serialize_header(h);
    buf.insert(buf.end(), password.begin(), password.end());
    buf.push_back('\0');
    return buf;
}

// S->C: status(1) + message\0
inline std::vector<uint8_t> make_auth_delete_acct_resp(AuthStatus status, const std::string& message) {
    uint32_t len = static_cast<uint32_t>(1 + message.size() + 1);
    SignalHeader h{MsgType::AUTH_DELETE_ACCT_RESP, len};
    auto buf = serialize_header(h);
    buf.push_back(static_cast<uint8_t>(status));
    buf.insert(buf.end(), message.begin(), message.end());
    buf.push_back('\0');
    return buf;
}

// C->S: empty
inline std::vector<uint8_t> make_auth_logout_msg() {
    SignalHeader h{MsgType::AUTH_LOGOUT, 0};
    return serialize_header(h);
}

// Input validation helpers
inline bool is_valid_username(const std::string& username) {
    if (username.empty() || username.size() > MAX_USERNAME_LEN) return false;
    for (char c : username) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    }
    return true;
}

inline bool is_valid_password(const std::string& password) {
    return password.size() >= MIN_PASSWORD_LEN && password.size() <= MAX_PASSWORD_LEN;
}

} // namespace lilypad
