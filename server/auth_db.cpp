#include "auth_db.h"

#include <sqlite3.h>
#include <sodium.h>

#include <openssl/sha.h>

#include <iostream>
#include <iomanip>
#include <sstream>

namespace lilypad {

static constexpr int SESSION_EXPIRY_DAYS = 30;

AuthDB::AuthDB(const std::string& db_path) {
    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        throw std::runtime_error(std::string("Failed to open auth database: ") + sqlite3_errmsg(db_));
    }
    // Enable WAL mode for better concurrent performance
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
    init_schema();
}

AuthDB::~AuthDB() {
    if (db_) sqlite3_close(db_);
}

void AuthDB::init_schema() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS users (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            username      TEXT NOT NULL UNIQUE COLLATE NOCASE,
            password_hash TEXT NOT NULL,
            created_at    INTEGER NOT NULL DEFAULT (strftime('%s','now'))
        );
        CREATE TABLE IF NOT EXISTS sessions (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            token_hash TEXT NOT NULL UNIQUE,
            created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
            expires_at INTEGER NOT NULL
        );
    )";
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error("Failed to init auth schema: " + msg);
    }
}

std::string AuthDB::hash_token(const uint8_t* raw_token, size_t len) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(raw_token, len, hash);
    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
        oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(hash[i]);
    return oss.str();
}

std::string AuthDB::get_password_hash(int64_t user_id) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "SELECT password_hash FROM users WHERE id = ?", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, user_id);
    std::string hash;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return hash;
}

AuthResult AuthDB::register_user(const std::string& username, const std::string& password) {
    // Hash password with Argon2id
    char hash[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str(hash, password.c_str(), password.size(),
                          crypto_pwhash_OPSLIMIT_MODERATE,
                          crypto_pwhash_MEMLIMIT_MODERATE) != 0) {
        return {false, 0, "Server error: failed to hash password"};
    }

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "INSERT INTO users (username, password_hash) VALUES (?, ?)",
                        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, hash, -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_CONSTRAINT) {
        return {false, 0, "Username already taken"};
    }
    if (rc != SQLITE_DONE) {
        return {false, 0, "Server error: database write failed"};
    }

    int64_t user_id = sqlite3_last_insert_rowid(db_);
    std::cout << "[Auth] Registered user: " << username << " (id=" << user_id << ")\n";
    return {true, user_id, "Account created successfully"};
}

AuthResult AuthDB::verify_login(const std::string& username, const std::string& password) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "SELECT id, password_hash FROM users WHERE username = ?",
                        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);

    AuthResult result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t user_id = sqlite3_column_int64(stmt, 0);
        const char* stored_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));

        if (crypto_pwhash_str_verify(stored_hash, password.c_str(), password.size()) == 0) {
            result = {true, user_id, "Login successful"};
        } else {
            result = {false, 0, "Invalid username or password"};
        }
    } else {
        result = {false, 0, "Invalid username or password"};
    }

    sqlite3_finalize(stmt);
    return result;
}

std::vector<uint8_t> AuthDB::create_session(int64_t user_id) {
    std::vector<uint8_t> raw_token(32);
    randombytes_buf(raw_token.data(), raw_token.size());

    std::string token_hash = hash_token(raw_token.data(), raw_token.size());

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT INTO sessions (user_id, token_hash, expires_at) VALUES (?, ?, strftime('%s','now') + ?)",
        -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, user_id);
    sqlite3_bind_text(stmt, 2, token_hash.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, SESSION_EXPIRY_DAYS * 24 * 3600);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return raw_token;
}

TokenResult AuthDB::validate_token(const std::string& username, const uint8_t* raw_token) {
    std::string token_hash = hash_token(raw_token, 32);

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT s.id, s.user_id, u.username FROM sessions s "
        "JOIN users u ON u.id = s.user_id "
        "WHERE s.token_hash = ? AND u.username = ? AND s.expires_at > strftime('%s','now')",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, token_hash.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_STATIC);

    TokenResult result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t session_id = sqlite3_column_int64(stmt, 0);
        result.user_id = sqlite3_column_int64(stmt, 1);
        result.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        sqlite3_finalize(stmt);

        // Delete old token (rolling)
        sqlite3_stmt* del_stmt = nullptr;
        sqlite3_prepare_v2(db_, "DELETE FROM sessions WHERE id = ?", -1, &del_stmt, nullptr);
        sqlite3_bind_int64(del_stmt, 1, session_id);
        sqlite3_step(del_stmt);
        sqlite3_finalize(del_stmt);

        // Issue new token
        result.new_token = create_session(result.user_id);
        result.success = true;
        result.message = "Token login successful";
    } else {
        sqlite3_finalize(stmt);
        result.success = false;
        result.message = "Session expired or invalid";
    }

    return result;
}

void AuthDB::invalidate_all_sessions(int64_t user_id) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "DELETE FROM sessions WHERE user_id = ?", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, user_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

AuthResult AuthDB::change_password(int64_t user_id, const std::string& old_password,
                                    const std::string& new_password) {
    // Verify old password
    std::string stored_hash = get_password_hash(user_id);
    if (stored_hash.empty()) {
        return {false, 0, "User not found"};
    }
    if (crypto_pwhash_str_verify(stored_hash.c_str(), old_password.c_str(), old_password.size()) != 0) {
        return {false, 0, "Current password is incorrect"};
    }

    // Hash new password
    char new_hash[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str(new_hash, new_password.c_str(), new_password.size(),
                          crypto_pwhash_OPSLIMIT_MODERATE,
                          crypto_pwhash_MEMLIMIT_MODERATE) != 0) {
        return {false, 0, "Server error: failed to hash password"};
    }

    // Update
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "UPDATE users SET password_hash = ? WHERE id = ?", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, new_hash, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, user_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Invalidate all sessions
    invalidate_all_sessions(user_id);

    std::cout << "[Auth] Password changed for user_id=" << user_id << "\n";
    return {true, user_id, "Password changed successfully"};
}

AuthResult AuthDB::delete_account(int64_t user_id, const std::string& password) {
    // Verify password
    std::string stored_hash = get_password_hash(user_id);
    if (stored_hash.empty()) {
        return {false, 0, "User not found"};
    }
    if (crypto_pwhash_str_verify(stored_hash.c_str(), password.c_str(), password.size()) != 0) {
        return {false, 0, "Password is incorrect"};
    }

    // Delete user (cascades to sessions)
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "DELETE FROM users WHERE id = ?", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, user_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    std::cout << "[Auth] Deleted account user_id=" << user_id << "\n";
    return {true, user_id, "Account deleted"};
}

void AuthDB::cleanup_expired_sessions() {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "DELETE FROM sessions WHERE expires_at <= strftime('%s','now')",
                        -1, &stmt, nullptr);
    sqlite3_step(stmt);
    int deleted = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    if (deleted > 0) {
        std::cout << "[Auth] Cleaned up " << deleted << " expired sessions\n";
    }
}

} // namespace lilypad
