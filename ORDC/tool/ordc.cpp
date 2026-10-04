// ordc: deterministic .ord inspection tool (parse/validate/show).
// Reads files, never writes them. Exit 0 = valid, 1 = invalid, 2 = misuse.
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "ordc/ord.hpp"

namespace {
void usage(const char* prog) {
    std::cout << "usage: " << prog << " check <file.ord> | show <file.ord>\n"
                 "  check: parse + validate schema/types/ranges (diagnostics on failure)\n"
                 "  show:  print the canonical deterministic serialization\n";
}

std::string readAll(const std::string& path, bool& ok) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        ok = false;
        return "";
    }
    std::ostringstream o;
    o << f.rdbuf();
    ok = true;
    return o.str();
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        usage(argv[0]);
        return 2;
    }
    std::string cmd = argv[1], path = argv[2];
    if (cmd != "check" && cmd != "show") {
        usage(argv[0]);
        return 2;
    }
    bool ok = false;
    std::string text = readAll(path, ok);
    if (!ok) {
        std::cout << path << ": cannot open file\n";
        return 1;
    }
    ordc::OrdDoc doc;
    ordc::OrdError err;
    if (!ordc::parseOrd(text, path, doc, err)) {
        std::cout << err.str() << "\n";
        return 1;
    }
    if (cmd == "check") {
        std::cout << "OK: " << path << " (ORD " << doc.version << ", type=" << doc.type
                  << ", id=" << doc.id << ", " << doc.sections.size() << " sections)\n";
        return 0;
    }
    std::cout << ordc::serializeOrd(doc);
    return 0;
}
