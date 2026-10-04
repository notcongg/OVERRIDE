#include "override/scenario.hpp"

namespace override {

std::vector<Scenario> builtinScenarios() {
    std::vector<Scenario> out;

    Scenario cascading;
    cascading.name = "cascading_failure";
    cascading.description = "Break the server and watch the client degrade, then repair.";
    cascading.steps = {
        {"status"},
        {"break server"},
        {"status"},
        {"trace client"},
        {"why client"},
        {"repair server"},
        {"status"},
    };
    out.push_back(cascading);

    Scenario storm;
    storm.name = "latency_storm";
    storm.description = "Inject latency into the server and observe propagation + prediction.";
    storm.steps = {
        {"inject server latency 800"},
        {"tick 5"},
        {"status"},
        {"inspect client"},
        {"predict server"},
        {"predict client"},
    };
    out.push_back(storm);

    Scenario cacheOut;
    cacheOut.name = "cache_outage";
    cacheOut.description = "Kill the cache and observe server degradation.";
    cacheOut.steps = {
        {"break cache"},
        {"status"},
        {"trace server"},
        {"repair cache"},
        {"status"},
    };
    out.push_back(cacheOut);

    Scenario dbCorrupt;
    dbCorrupt.name = "db_corruption";
    dbCorrupt.description = "Corrupt the database and trace the impact.";
    dbCorrupt.steps = {
        {"inject database corruption"},
        {"status"},
        {"trace server"},
        {"why server"},
        {"repair database"},
        {"status"},
    };
    out.push_back(dbCorrupt);

    Scenario deceive;
    deceive.name = "deception_demo";
    deceive.description = "Plant a false latency belief in the client.";
    deceive.steps = {
        {"override server latency 800"},
        {"deceive client server.latency 20"},
        {"inspect server"},
        {"ping server"},
    };
    out.push_back(deceive);

    return out;
}

const Scenario* findScenario(const std::vector<Scenario>& all, const std::string& name) {
    for (const auto& s : all)
        if (s.name == name) return &s;
    return nullptr;
}

} // namespace override
