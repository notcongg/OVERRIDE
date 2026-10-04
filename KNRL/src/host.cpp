#include "override/host.hpp"

#include <cstdint>

namespace override {

std::string localHostname() { return "override-local"; }
int localCpuCores() { return 8; }
int localRamMb() { return 16384; }

std::vector<LocalIface> localInterfaces() {
    return {{"lo", "127.0.0.1", true}, {"sim0", "10.7.0.1", true}, {"sim1", "10.8.0.1", true}};
}

std::vector<LocalPort> localPorts() {
    return {{22, "ssh-sim", "OPEN"},     {80, "http-sim", "OPEN"},   {443, "tls-sim", "OPEN"},
            {3000, "overshell", "OPEN"}, {5432, "db-sim", "OPEN"},   {6379, "cache-sim", "OPEN"},
            {8080, "alt-http", "FILTERED"}, {9090, "metrics", "OPEN"}};
}

namespace {
uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
} // namespace

int localLoad(uint64_t seed, uint64_t tick) {
    return 5 + (int)(mix64(seed ^ (tick * 0x9E3779B97F4A7C15ull)) % 91);
}

int localRamUsed(uint64_t seed, uint64_t tick, int procCount) {
    uint64_t h = mix64(seed ^ 0x1234 ^ (tick * 31));
    return 2048 + (int)(h % 2048) + procCount * 64;
}

} // namespace override
