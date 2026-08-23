#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

namespace psx::sink {

// Groups hosts by identical output and ranks the groups largest-first, so the
// majority (the consensus) is buckets.front() and any smaller buckets are the
// outliers (configuration drift). Deterministic: buckets ordered by size
// descending then output ascending; hosts within a bucket sorted ascending.
struct ConsensusReport {
    struct Bucket {
        std::string output;
        std::vector<std::string> hosts;
    };
    std::vector<Bucket> buckets;

    bool unanimous() const { return buckets.size() <= 1; }
    std::size_t hostCount() const;
};

// Reduce a set of (host, output) pairs to a consensus report.
ConsensusReport consensus(const std::vector<std::pair<std::string, std::string>>& hostOutputs);

// Render a human-readable report: the majority verdict and each outlier's
// output. Writes nothing surprising for the empty/unanimous cases.
void renderConsensus(const ConsensusReport& report, std::ostream& out);

// Render the report as one JSON object: {unanimous, hosts, buckets:[{hosts,output}]}.
// The largest bucket is first (the consensus); the rest are drift.
void renderConsensusJson(const ConsensusReport& report, std::ostream& out);

} // namespace psx::sink
