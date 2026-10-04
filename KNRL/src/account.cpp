// Authentication layer: SHA-256 + PBKDF2-HMAC-SHA256 (self-contained,
// stdlib only) and the oup.ord account document. Never touches host auth.
#include "override/account.hpp"

#include <random>
#include <sstream>

namespace override {

namespace {
// Minimal SHA-256 (FIPS 180-4). Standard constants; big-endian everywhere.
uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

std::string sha256Raw(const std::string& data) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string msg = data;
    uint64_t bitlen = (uint64_t)data.size() * 8;
    msg += (char)0x80;
    while (msg.size() % 64 != 56) msg += (char)0x00;
    for (int i = 7; i >= 0; --i) msg += (char)((bitlen >> (i * 8)) & 0xff);
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = ((uint32_t)(unsigned char)msg[off + i * 4] << 24) |
                   ((uint32_t)(unsigned char)msg[off + i * 4 + 1] << 16) |
                   ((uint32_t)(unsigned char)msg[off + i * 4 + 2] << 8) |
                   ((uint32_t)(unsigned char)msg[off + i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6],
                 hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    std::string out;
    for (int i = 0; i < 8; ++i)
        for (int j = 3; j >= 0; --j) out += (char)((h[i] >> (j * 8)) & 0xff);
    return out;
}

std::string hexOf(const std::string& raw) {
    static const char* H = "0123456789abcdef";
    std::string o;
    for (unsigned char c : raw) {
        o += H[c >> 4];
        o += H[c & 0xf];
    }
    return o;
}

std::string unhex(const std::string& hex, bool& ok) {
    std::string o;
    ok = (hex.size() % 2 == 0) && !hex.empty();
    if (!ok) return "";
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = val(hex[i]), lo = val(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            ok = false;
            return "";
        }
        o += (char)(hi * 16 + lo);
    }
    return o;
}

std::string hmacSha256(const std::string& key, const std::string& msg) {
    std::string k = key;
    if (k.size() > 64) k = sha256Raw(k);
    k.append(64 - k.size(), '\0');
    std::string okey, ikey;
    for (size_t i = 0; i < 64; ++i) {
        okey += (char)(k[i] ^ 0x5c);
        ikey += (char)(k[i] ^ 0x36);
    }
    return sha256Raw(okey + sha256Raw(ikey + msg));
}
} // namespace

std::string sha256Hex(const std::string& data) { return hexOf(sha256Raw(data)); }

std::string pbkdf2Sha256Hex(const std::string& password, const std::string& salt,
                            int iterations) {
    if (iterations < 1) iterations = 1;
    std::string block = salt + std::string("\x00\x00\x00\x01", 4);
    std::string u = hmacSha256(password, block), acc = u;
    for (int i = 1; i < iterations; ++i) {
        u = hmacSha256(password, u);
        for (size_t j = 0; j < acc.size(); ++j) acc[j] ^= u[j];
    }
    return hexOf(acc);
}

std::string randomSaltHex(size_t bytes) {
    std::random_device rd;
    std::string raw;
    for (size_t i = 0; i < bytes; ++i) raw += (char)(rd() & 0xff);
    return hexOf(raw);
}

bool validAccountName(const std::string& u) {
    if (u.empty() || u.size() > 32) return false;
    if (u[0] < 'a' || u[0] > 'z') return false;
    for (char c : u) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

AccountStore AccountStore::create(const std::string& username, const std::string& password,
                                 std::string& err, int iterations) {
    if (!validAccountName(username)) {
        err = "invalid username '" + username + "' (use [a-z][a-z0-9_-]*, max 32)";
        return AccountStore{};
    }
    if (password.empty()) {
        err = "password must not be empty";
        return AccountStore{};
    }
    if (iterations < kMinIterations) {
        err = "iteration count too low (min 1000)";
        return AccountStore{};
    }
    AccountStore a;
    a.username = username;
    a.kdf = "pbkdf2-sha256";
    a.iterations = iterations;
    a.saltHex = randomSaltHex(kSaltBytes);
    bool ok = false;
    std::string salt = unhex(a.saltHex, ok);
    if (!ok) {
        err = "salt generation failed";
        return AccountStore{};
    }
    a.hashHex = pbkdf2Sha256Hex(password, salt, a.iterations);
    return a;
}

bool AccountStore::verifyPassword(const std::string& password) const {
    if (!configured() || kdf != "pbkdf2-sha256" || iterations < 1) return false;
    bool ok = false;
    std::string salt = unhex(saltHex, ok);
    if (!ok) return false;
    std::string trial = pbkdf2Sha256Hex(password, salt, iterations);
    // Constant-time comparison (no early exit on content).
    if (trial.size() != hashHex.size()) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < trial.size(); ++i) diff |= (unsigned)(trial[i] ^ hashHex[i]);
    return diff == 0;
}

void AccountStore::serializeDoc(ordc::OrdDoc& doc) const {
    doc = ordc::OrdDoc{};
    doc.type = "user";
    doc.id = "account";
    doc.set("account", "usr", username);
    std::string ps = kdf + "$" + std::to_string(iterations) + "$" + saltHex + "$" + hashHex;
    doc.set("account", "ps", ps);
    doc.set("sudo", "installed", sudoInstalled ? "true" : "false");
}

bool AccountStore::deserializeDoc(const ordc::OrdDoc& doc, std::string& err) {
    if (doc.type != "user") {
        err = "not a user document (type = " + doc.type + ")";
        return false;
    }
    ordc::OrdError oerr;
    oerr.file = "oup.ord";
    std::string usr, ps, inst;
    if (!ordc::getString(doc, "account", "usr", usr, oerr)) {
        err = oerr.str();
        return false;
    }
    if (!ordc::getString(doc, "account", "ps", ps, oerr)) {
        err = oerr.str();
        return false;
    }
    if (!validAccountName(usr)) {
        err = "oup.ord: [account] usr: invalid stored username";
        return false;
    }
    // ps = kdf$iterations$salt$hash (hash never plaintext, never reversible).
    auto p1 = ps.find('$'), p2 = ps.find('$', p1 == std::string::npos ? 0 : p1 + 1),
         p3 = ps.find('$', p2 == std::string::npos ? 0 : p2 + 1);
    if (p1 == std::string::npos || p2 == std::string::npos || p3 == std::string::npos) {
        err = "oup.ord: [account] ps: malformed credential record";
        return false;
    }
    std::string kdf = ps.substr(0, p1), iterS = ps.substr(p1 + 1, p2 - p1 - 1),
                salt = ps.substr(p2 + 1, p3 - p2 - 1), hash = ps.substr(p3 + 1);
    if (kdf != "pbkdf2-sha256" || iterS.empty() || salt.empty() || hash.empty()) {
        err = "oup.ord: [account] ps: unsupported credential record";
        return false;
    }
    for (char c : iterS)
        if (c < '0' || c > '9') {
            err = "oup.ord: [account] ps: bad iteration count";
            return false;
        }
    int iters = 0;
    try {
        iters = std::stoi(iterS);
    } catch (...) {
        err = "oup.ord: [account] ps: bad iteration count";
        return false;
    }
    if (iters < AccountStore::kMinIterations) {
        err = "oup.ord: [account] ps: iteration count too low";
        return false;
    }
    bool ok = false;
    unhex(salt, ok);
    if (!ok || salt.size() != 32) { // 16 bytes hex
        err = "oup.ord: [account] ps: bad salt";
        return false;
    }
    unhex(hash, ok);
    if (!ok || hash.size() != 64) { // 32 bytes hex
        err = "oup.ord: [account] ps: bad hash";
        return false;
    }
    AccountStore loaded;
    loaded.username = usr;
    loaded.kdf = kdf;
    loaded.iterations = iters;
    loaded.saltHex = salt;
    loaded.hashHex = hash;
    if (ordc::getString(doc, "sudo", "installed", inst, oerr)) {
        if (inst != "true" && inst != "false") {
            err = "oup.ord: [sudo] installed: invalid boolean";
            return false;
        }
        loaded.sudoInstalled = (inst == "true");
    }
    *this = loaded;
    return true;
}

} // namespace override
