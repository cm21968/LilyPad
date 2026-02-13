#pragma once

#include <openssl/ssl.h>
#include <string>

namespace lilypad {

// Load cert/key from paths. If they don't exist, generates a self-signed cert and saves it.
// Returns true on success.
bool load_or_generate_cert(const std::string& cert_path, const std::string& key_path);

// Create server SSL_CTX using the specified cert/key files.
// Returns nullptr on failure.
SSL_CTX* create_server_ssl_ctx(const std::string& cert_path, const std::string& key_path);

} // namespace lilypad
