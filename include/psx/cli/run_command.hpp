#pragma once

// psx::cli — parsing and execution of the `pipeshellx run` subcommand
// (PLAN.md Appendix A). Kept separate from execution so the argument grammar
// is unit-tested on its own.

#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace psx::cli {

class CliError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class SinkMode { Group, Stream, Json };
enum class SelectorKind { All, Group, Tag, Hosts };

struct Selector {
    SelectorKind kind = SelectorKind::All;
    std::string value;              // group or tag name
    std::vector<std::string> hosts; // for SelectorKind::Hosts
};

struct RunInvocation {
    std::string inventoryPath; // -i / --inventory; empty = default lookup
    Selector selector;
    SinkMode sink = SinkMode::Group;
    int timeoutSec = 0;
    bool colour = true;               // --no-color turns it off
    std::vector<std::string> command; // the argv after `--`
};

// Parses the args following `run`. Throws CliError on any grammar violation.
RunInvocation parseRun(const std::vector<std::string>& args);

// Loads the inventory, selects hosts, runs `command` on each over SSH, renders
// through the chosen sink, and returns the process exit code (0 all succeeded,
// 1 some stage failed, 2 usage/config, 3 no hosts selected). `colourTty` is
// whether stdout is a terminal (the effective colour is colour && colourTty).
int runSubcommand(const RunInvocation& invocation, std::ostream& out, std::ostream& err, bool colourTty);

} // namespace psx::cli
