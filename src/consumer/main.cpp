// pixil-consumer — Phase 2 native LED consumer (ADR-001 / ADR-002).
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <iostream>
#include <memory>
#include <string>

#include "consumer/api.h"
#include "consumer/loop.h"
#include "consumer/matrix.h"

using namespace pixil::consumer;

namespace {

void usage(std::ostream& out) {
    out << "pixil-consumer — native LED consumer (ADR-001)\n"
           "  Spawned by pixil with fds 3/4/5 inherited. Do not run by hand unless debugging.\n"
           "  --headless     in-memory canvas; no GPIO\n"
           "  --record FILE  JSONL of executed commands / markers\n"
           "  --help\n";
}

}  // namespace

int main(int argc, char** argv) {
    ConsumerOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            usage(std::cout);
            return 0;
        } else if (a == "--headless") {
            opts.headless = true;
        } else if (a == "--record") {
            if (i + 1 >= argc) {
                std::cerr << "pixil-consumer: --record needs a path\n";
                return 2;
            }
            opts.record_path = argv[++i];
        } else {
            std::cerr << "pixil-consumer: unknown option " << a << "\n";
            usage(std::cerr);
            return 2;
        }
    }

    for (int fd : {3, 4, 5}) {
        if (::fcntl(fd, F_GETFD) < 0) {
            std::cerr << "[pixil-consumer] fd " << fd
                      << " not open; must be spawned by the native producer\n";
            return 2;
        }
    }

    signal(SIGINT, SIG_IGN);
    signal(SIGPIPE, SIG_DFL);

    try {
        std::unique_ptr<MatrixSurface> surface =
            opts.headless ? make_headless_matrix() : make_hardware_matrix();
        MatrixApi api(std::move(surface));
        int rc = run_consumer(api, opts);
        api.cleanup();
        return rc;
    } catch (const std::exception& e) {
        std::cerr << "[pixil-consumer] fatal: " << e.what() << "\n";
        return 1;
    }
}
