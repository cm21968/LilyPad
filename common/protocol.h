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
    SCREEN_FRAME       = 0x0B,  // Client→Server: width(2)+height(2)+jpeg
                                // Server→Subscribers: sharer_id(4)+width(2)+height(2)+jpeg
    SCREEN_AUDIO       = 0x0C,  // Client→Server: opus_data
                                // Server→Subscribers: sharer_id(4)+opus_data

    UPDATE_AVAILABLE   = 0x0D,  // Server→Client: version\0url\0
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

// ── UDP voice packet: [client_id:4][sequence:4][opus_data:variable] ──
constexpr size_t VOICE_HEADER_SIZE = 8;
constexpr size_t MAX_VOICE_PACKET  = 1400; // safe for MTU

struct VoicePacket {
    uint32_t             client_id;
    uint32_t             sequence;
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

// Client→Server: width(2) + height(2) + jpeg_data
inline std::vector<uint8_t> make_screen_frame_msg(uint16_t width, uint16_t height,
                                                   const uint8_t* jpeg, size_t jpeg_len) {
    uint32_t payload_len = static_cast<uint32_t>(4 + jpeg_len); // 2+2+jpeg
    SignalHeader h{MsgType::SCREEN_FRAME, payload_len};
    auto buf = serialize_header(h);
    buf.push_back(static_cast<uint8_t>(width & 0xFF));
    buf.push_back(static_cast<uint8_t>((width >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(height & 0xFF));
    buf.push_back(static_cast<uint8_t>((height >> 8) & 0xFF));
    buf.insert(buf.end(), jpeg, jpeg + jpeg_len);
    return buf;
}

// Server→Subscribers: sharer_id(4) + width(2) + height(2) + jpeg_data
inline std::vector<uint8_t> make_screen_frame_relay(uint32_t sharer_id, uint16_t width,
                                                     uint16_t height, const uint8_t* jpeg,
                                                     size_t jpeg_len) {
    uint32_t payload_len = static_cast<uint32_t>(8 + jpeg_len); // 4+2+2+jpeg
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
    buf.insert(buf.end(), jpeg, jpeg + jpeg_len);
    return buf;
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

} // namespace lilypad
