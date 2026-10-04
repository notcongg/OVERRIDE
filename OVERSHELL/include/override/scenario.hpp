#pragma once

#include <string>
#include <vector>

#include "override/system.hpp"

namespace override {

struct ScenarioStep {
    std::string line; // an OVERSHELL command line
};

struct Scenario {
    std::string name;
    std::string description;
    std::vector<ScenarioStep> steps;
};

std::vector<Scenario> builtinScenarios();
const Scenario* findScenario(const std::vector<Scenario>& all, const std::string& name);

} // namespace override
