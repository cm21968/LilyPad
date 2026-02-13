#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

namespace lilypad {

struct AuthResult {
    bool        success = false;
    int64_t     user_id = 0;
    std::string message;
};

struct TokenResult {
    bool                 success = false;
    int64_t              user_id = 0;
    std::string          username;
    std::vector<uint8_t> new_token; // 32 bytes, raw (for client)
    std::string          message;
};

class AuthDB {
public:
    explicit AuthDB(const std::string& db_path);
    ~AuthDB();
    AuthDB(const AuthDB&) = delete;
    AuthDB& operator=(const AuthDB&) = delete;

    // Register a new user. Returns success/failure with message.
    AuthResult register_user(const std::string& username, const std::string& password);

    // Verify login credentials. Returns user_id on success.
    AuthResult verify_login(const std::string& username, const std::string& password);

    // Create a session token for a user. Returns raw 32-byte token (for sending to client).
    std::vector<uint8_t> create_session(int64_t user_id);

    // Validate a session token. Invalidates old token and issues a new one (rolling).
    // Returns user_id and username on success.
    TokenResult validate_token(const std::string& username, const uint8_t* raw_token);

    // Invalidate all sessions for a user (e.g. on password change)
    void invalidate_all_sessions(int64_t user_id);

    // Change password. Requires old password verification. Invalidates all sessions.
    AuthResult change_password(int64_t user_id, const std::string& old_password,
                               const std::string& new_password);

    // Delete account. Requires password verification.
    AuthResult delete_account(int64_t user_id, const std::string& password);

    // Clean up expired sessions
    void cleanup_expired_sessions();

private:
    sqlite3* db_ = nullptr;
    void init_schema();

    // Hash raw token with SHA-256 for storage
    std::string hash_token(const uint8_t* raw_token, size_t len);

    // Get stored password hash for a user
    std::string get_password_hash(int64_t user_id);
};

} // namespace lilypad
