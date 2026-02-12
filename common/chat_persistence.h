#pragma once

#include <cstdint>
#include <string>
#include <sstream>

namespace lilypad {

struct ChatLine {
    uint64_t    seq       = 0;
    std::string sender;
    int64_t     timestamp = 0;
    std::string text;
    bool        valid     = false;
};

// Escape special characters for JSON string values
inline std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out += c;      break;
        }
    }
    return out;
}

// Unescape JSON string values
inline std::string json_unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
            case '"':  out += '"';  ++i; break;
            case '\\': out += '\\'; ++i; break;
            case 'n':  out += '\n'; ++i; break;
            case 'r':  out += '\r'; ++i; break;
            case 't':  out += '\t'; ++i; break;
            default:   out += s[i]; break;
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

// Serialize a chat entry to a JSON Lines string (no trailing newline)
inline std::string serialize_chat_line(uint64_t seq, const std::string& sender,
                                        int64_t timestamp, const std::string& text) {
    std::ostringstream oss;
    oss << "{\"seq\":" << seq
        << ",\"sender\":\"" << json_escape(sender) << "\""
        << ",\"ts\":" << timestamp
        << ",\"text\":\"" << json_escape(text) << "\"}";
    return oss.str();
}

// Extract a JSON string value after the given key (e.g. "sender")
// Expects: "key":"value" format
inline std::string extract_json_string(const std::string& line, const std::string& key) {
    std::string needle = "\"" + key + "\":\"";
    auto pos = line.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    std::string result;
    for (size_t i = pos; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            result += line[i];
            result += line[i + 1];
            ++i;
        } else if (line[i] == '"') {
            break;
        } else {
            result += line[i];
        }
    }
    return json_unescape(result);
}

// Extract a JSON integer value after the given key
inline int64_t extract_json_int(const std::string& line, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    auto pos = line.find(needle);
    if (pos == std::string::npos) return 0;
    pos += needle.size();
    // Skip whitespace
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    // Handle negative
    bool neg = false;
    if (pos < line.size() && line[pos] == '-') { neg = true; ++pos; }
    int64_t val = 0;
    while (pos < line.size() && line[pos] >= '0' && line[pos] <= '9') {
        val = val * 10 + (line[pos] - '0');
        ++pos;
    }
    return neg ? -val : val;
}

// Parse a JSON Lines chat entry
inline ChatLine parse_chat_line(const std::string& line) {
    ChatLine entry;
    if (line.empty() || line[0] != '{') return entry;
    entry.seq       = static_cast<uint64_t>(extract_json_int(line, "seq"));
    entry.sender    = extract_json_string(line, "sender");
    entry.timestamp = extract_json_int(line, "ts");
    entry.text      = extract_json_string(line, "text");
    entry.valid     = (entry.seq > 0);
    return entry;
}

} // namespace lilypad
