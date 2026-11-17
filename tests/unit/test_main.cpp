#include <cstring>

#include "check.h"

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0;
    for (const auto& c : pixil_test::registry()) {
        if (filter && std::strstr(c.name, filter) == nullptr) continue;
        try {
            c.fn();
        } catch (const std::exception& e) {
            pixil_test::report(c.name, 0, std::string("uncaught exception: ") + e.what());
        }
        ++ran;
    }
    std::cout << ran << " tests, " << pixil_test::failures() << " failures\n";
    return pixil_test::failures() == 0 ? 0 : 1;
}
