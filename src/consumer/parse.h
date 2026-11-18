#pragma once

#include <string>

#include "consumer/api.h"

namespace pixil::consumer {

void parse_and_execute(MatrixApi& api, const std::string& command);

}  // namespace pixil::consumer
