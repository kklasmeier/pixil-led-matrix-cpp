#pragma once

#include <string>

#include "consumer/api.h"

namespace pixil::consumer {

struct ConsumerOptions {
    bool headless = false;
    std::string record_path;
};

// Runs until shutdown / data EOF. Returns process exit code.
int run_consumer(MatrixApi& api, const ConsumerOptions& opts);

}  // namespace pixil::consumer
