#pragma once

// psx::policy::Policy — an optional command policy loaded from a file
// (`run --policy FILE`, PLAN.md §2.1, §3.8). It replaces the hardcoded
// allowlist for operator use: allowed argv[0] names, a max argument count, and
// a shell-metacharacter guard (on by default; a policy that runs shells opts
// out). An empty policy (the default) allows anything.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace psx::policy {

class Policy {
public:
    Policy() = default;

    // Parses a line-based policy; throws std::runtime_error naming the line on
    // a malformed directive.
    //   allow <name>                    add an allowed argv[0] (none = allow all)
    //   max-args <n>                    cap the argument count
    //   allow-shell-metacharacters      disable the ; | & $ ` < > guard
    static Policy parse(std::string_view text, const std::string& sourcePath = {});
    static Policy loadFromFile(const std::string& path);

    // Returns a rejection reason, or nullopt when the command is permitted.
    std::optional<std::string> validate(const std::vector<std::string>& argv) const;

    bool empty() const noexcept { return allowed_.empty() && !maxArgs_.has_value(); }

private:
    std::unordered_set<std::string> allowed_; // empty = any command
    std::optional<std::size_t> maxArgs_;
    bool allowShellMetacharacters_ = false;
};

} // namespace psx::policy
