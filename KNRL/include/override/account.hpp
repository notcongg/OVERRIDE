#pragma once

// Explicit authentication layer (no host auth APIs, no host users).
//
// The OVERRIDE account lives in oup.ord (type = user) under the platform
// application data directory. Passwords are stored as PBKDF2-HMAC-SHA256
// with a unique random salt (self-contained: the project takes no crypto
// dependencies, so Argon2 is unavailable; parameters below are documented).
// Format of `ps` is deterministic, hashes depend on salt by design:
//   pbkdf2-sha256$<iterations>$<salt-hex>$<hash-hex>
#include <cstdint>
#include <string>

#include "ordc/ord.hpp"

namespace override {

// SHA-256 (single shot) and PBKDF2-HMAC-SHA256, hex-encoded. Deterministic,
// no host interaction except randomSaltHex (std::random_device: host-side
// account metadata only, never simulation state).
std::string sha256Hex(const std::string& data);
std::string pbkdf2Sha256Hex(const std::string& password, const std::string& salt,
                            int iterations);
std::string randomSaltHex(size_t bytes);

// Username rule: [a-z][a-z0-9_-]*, max 32 (mirrors node naming, separate).
bool validAccountName(const std::string& u);

struct AccountStore {
    static const int kDefaultIterations = 100000;
    static const size_t kSaltBytes = 16;

    std::string username; // "" = no account configured
    std::string kdf = "pbkdf2-sha256";
    int iterations = kDefaultIterations;
    std::string saltHex;
    std::string hashHex;
    bool sudoInstalled = false;

    bool configured() const { return !username.empty() && !hashHex.empty(); }
    // Create (validates name + non-empty password, mints a fresh salt).
    // `iterations` is exposed so tests can use a cheap cost model; production
    // callers keep the default. Values below 1000 are rejected everywhere.
    static const int kMinIterations = 1000;
    static AccountStore create(const std::string& username, const std::string& password,
                               std::string& err,
                               int iterations = kDefaultIterations);
    bool verifyPassword(const std::string& password) const;
    void serializeDoc(ordc::OrdDoc& doc) const; // type user, id account
    bool deserializeDoc(const ordc::OrdDoc& doc, std::string& err);
};

} // namespace override
