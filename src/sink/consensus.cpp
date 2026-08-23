#include "psx/sink/consensus.hpp"

#include <algorithm>
#include <map>
#include <ostream>

namespace psx::sink {

std::size_t ConsensusReport::hostCount() const {
    std::size_t total = 0;
    for (const Bucket& bucket : buckets) {
        total += bucket.hosts.size();
    }
    return total;
}

ConsensusReport consensus(const std::vector<std::pair<std::string, std::string>>& hostOutputs) {
    // std::map keeps outputs sorted (a deterministic tie-break) and groups hosts.
    std::map<std::string, std::vector<std::string>> byOutput;
    for (const auto& [host, output] : hostOutputs) {
        byOutput[output].push_back(host);
    }

    ConsensusReport report;
    report.buckets.reserve(byOutput.size());
    for (auto& [output, hosts] : byOutput) {
        std::sort(hosts.begin(), hosts.end());
        report.buckets.push_back({.output = output, .hosts = std::move(hosts)});
    }
    // Largest bucket first (the consensus); ties keep the output-ascending order
    // from the map, so stable_sort by size descending.
    std::stable_sort(report.buckets.begin(), report.buckets.end(),
                     [](const ConsensusReport::Bucket& a, const ConsensusReport::Bucket& b) {
                         return a.hosts.size() > b.hosts.size();
                     });
    return report;
}

void renderConsensus(const ConsensusReport& report, std::ostream& out) {
    if (report.buckets.empty()) {
        out << "no hosts\n";
        return;
    }
    const std::size_t total = report.hostCount();
    const ConsensusReport::Bucket& majority = report.buckets.front();

    if (report.unanimous()) {
        out << "consensus: all " << total << (total == 1 ? " host agrees" : " hosts agree") << "\n";
        return;
    }

    out << "consensus: " << majority.hosts.size() << "/" << total << " hosts agree; " << (report.buckets.size() - 1)
        << (report.buckets.size() - 1 == 1 ? " outlier" : " outliers") << "\n";
    for (std::size_t i = 1; i < report.buckets.size(); ++i) {
        const ConsensusReport::Bucket& outlier = report.buckets[i];
        out << "--- outlier";
        for (const std::string& host : outlier.hosts) {
            out << " " << host;
        }
        out << " ---\n" << outlier.output;
        if (!outlier.output.empty() && outlier.output.back() != '\n') {
            out << "\n";
        }
    }
}

} // namespace psx::sink
