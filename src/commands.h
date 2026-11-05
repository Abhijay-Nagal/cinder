// Command dispatch: turns a parsed command into a RESP reply.
#pragma once

#include <string>
#include <vector>

#include "store.h"

namespace cinder {

// Executes `args` (args[0] is the command name, case-insensitive) against
// `store` and appends the RESP reply to `out`.
void execute(Store& store, const std::vector<std::string>& args, std::string& out);

}  // namespace cinder
