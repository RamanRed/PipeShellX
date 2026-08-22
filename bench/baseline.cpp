// Baseline measurement harness for PLAN.md §7 (recorded in docs/benchmarks.md).
//
// Measures what the *current* implementation costs so that later phases have
// numbers to beat:
//   --spawn N        N round-trips of ProcessManager::execute({"true"})
//                    (fork + exec + 3 pipes + poll + waitpid), p50/p90/p99
//   --fanout N[,N…]  executeRemote() against N copies of `ssh localhost uptime`
//                    (skipped with a reason when localhost is not reachable)
// Every section also reports the open-descriptor count and peak RSS.
//
// Usage: pipeshellx_bench_baseline [--spawn N] [--fanout 50,100] [--user NAME]

#include "client_config.hpp"
#include "logger.hpp"
#include "process_manager.hpp"

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Percentiles {
    double p50 = 0;
    double p90 = 0;
    double p99 = 0;
    double max = 0;
    double mean = 0;
};

Percentiles summarise(std::vector<double> samples) {
    Percentiles result;
    if (samples.empty()) {
        return result;
    }
    std::sort(samples.begin(), samples.end());
    auto at = [&](double fraction) {
        const auto index = static_cast<std::size_t>(fraction * static_cast<double>(samples.size() - 1));
        return samples[index];
    };
    result.p50 = at(0.50);
    result.p90 = at(0.90);
    result.p99 = at(0.99);
    result.max = samples.back();
    double total = 0;
    for (double sample : samples) {
        total += sample;
    }
    result.mean = total / static_cast<double>(samples.size());
    return result;
}

std::string mib(long kib) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << static_cast<double>(kib) / 1024.0 << " MiB";
    return out.str();
}

long peakRssKiB() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
#if defined(__APPLE__)
    return usage.ru_maxrss / 1024; // bytes on Darwin
#else
    return usage.ru_maxrss; // KiB on Linux
#endif
}

int openDescriptorCount() {
    const long limit = std::min<long>(sysconf(_SC_OPEN_MAX), 65536);
    int count = 0;
    for (int fd = 0; fd < limit; ++fd) {
        if (fcntl(fd, F_GETFD) != -1) {
            ++count;
        }
    }
    return count;
}

std::string platformString() {
    utsname info{};
    if (uname(&info) != 0) {
        return "unknown";
    }
    return std::string(info.sysname) + " " + info.release + " " + info.machine;
}

int parsePositive(const std::string& text, const std::string& what) {
    std::size_t consumed = 0;
    int value = 0;
    try {
        value = std::stoi(text, &consumed);
    } catch (const std::exception&) {
        consumed = 0;
    }
    if (consumed != text.size() || value <= 0) {
        throw std::invalid_argument(what + " expects a positive integer, got '" + text + "'");
    }
    return value;
}

std::vector<int> parseList(const std::string& value, const std::string& what) {
    std::vector<int> values;
    std::stringstream stream(value);
    std::string item;
    while (std::getline(stream, item, ',')) {
        if (!item.empty()) {
            values.push_back(parsePositive(item, what));
        }
    }
    return values;
}

std::string currentUser() {
    if (const char* user = std::getenv("USER"); user != nullptr && *user != '\0') {
        return user;
    }
    if (const char* login = getlogin(); login != nullptr) {
        return login;
    }
    return "root";
}

void printRow(const std::string& metric, const std::string& value, const std::string& note = "") {
    std::cout << "| " << std::left << std::setw(44) << metric << " | " << std::setw(20) << value << " | " << note
              << " |\n";
}

std::string ms(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << value << " ms";
    return out.str();
}

void runSpawnBenchmark(int iterations) {
    ProcessManager manager;
    const LogContext context{getpid(), "bench", "-", "true"};
    const std::vector<std::string> args{"true"};

    for (int i = 0; i < 20; ++i) { // warm-up
        static_cast<void>(manager.execute(args, context));
    }

    const int fdsBefore = openDescriptorCount();
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iterations));
    int failures = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto result = manager.execute(args, context);
        const auto t1 = std::chrono::steady_clock::now();
        if (result.exitCode != 0) {
            ++failures;
        }
        samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    const auto wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const int fdsAfter = openDescriptorCount();
    const auto stats = summarise(samples);

    std::cout << "\n### Local spawn round-trip (`execute({\"true\"})`, N=" << iterations << ")\n\n";
    std::cout << "| Metric | Value | Note |\n|---|---|---|\n";
    printRow("p50 / p90 / p99", ms(stats.p50) + " / " + ms(stats.p90) + " / " + ms(stats.p99));
    printRow("mean / max", ms(stats.mean) + " / " + ms(stats.max));
    printRow("throughput", std::to_string(static_cast<int>(static_cast<double>(iterations) / wall)) + " spawns/s");
    printRow("failures", std::to_string(failures));
    printRow("open descriptors before / after", std::to_string(fdsBefore) + " / " + std::to_string(fdsAfter),
             fdsBefore == fdsAfter ? "no leak" : "LEAK");
    printRow("peak RSS", mib(peakRssKiB()));
}

void runFanoutBenchmark(const std::vector<int>& sizes, const std::string& user) {
    ProcessManager manager;
    // Use the application's real argv shape, including a pinned trust store,
    // rather than the operator's ~/.ssh/known_hosts.
    ClientEntry probe = ClientConfig::parseEntry(user + "@localhost");
    probe.knownHostsFile = (std::filesystem::temp_directory_path() / "pipeshellx-bench.known_hosts").string();
    const LogContext context{getpid(), "bench", "localhost", "ssh localhost"};

    std::cout << "\n### SSH fan-out (`ssh localhost uptime`, agentless)\n\n";
    const auto probeResult = manager.executeRemote({probe}, "true", context, {.timeoutSec = 10});
    if (probeResult.exitCode != 0 || probeResult.timedOut) {
        const auto& clientResult = probeResult.clientResults.front();
        std::cout << "SKIPPED: `ssh " << user << "@localhost` not usable ("
                  << (clientResult.errorMessage.empty() ? clientResult.stderrData : clientResult.errorMessage) << ")\n";
        return;
    }

    std::cout << "| Metric | Value | Note |\n|---|---|---|\n";
    for (int size : sizes) {
        std::vector<ClientEntry> clients(static_cast<std::size_t>(size), probe);
        const int fdsBefore = openDescriptorCount();
        const auto t0 = std::chrono::steady_clock::now();
        const auto result = manager.executeRemote(clients, "uptime", context, {.timeoutSec = 120});
        const auto wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const int fdsAfter = openDescriptorCount();

        int ok = 0;
        for (const auto& clientResult : result.clientResults) {
            if (clientResult.exitCode == 0 && !clientResult.timedOut) {
                ++ok;
            }
        }
        const std::string label = "N=" + std::to_string(size) + " ";
        printRow(label + "wall time", ms(wall), std::to_string(ok) + "/" + std::to_string(size) + " ok");
        printRow(label + "per-host", ms(wall / size));
        printRow(label + "open descriptors before / after",
                 std::to_string(fdsBefore) + " / " + std::to_string(fdsAfter),
                 fdsBefore == fdsAfter ? "no leak" : "LEAK");
        printRow(label + "peak RSS", mib(peakRssKiB()));
    }
}

} // namespace

int main(int argc, char** argv) try {
    int spawnIterations = 1000;
    std::vector<int> fanoutSizes{50, 100};
    std::string user = currentUser();

    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        auto value = [&]() -> std::string {
            if (index + 1 >= argc) {
                std::cerr << arg << " requires a value\n";
                std::exit(2);
            }
            return argv[++index];
        };
        if (arg == "--spawn") {
            spawnIterations = parsePositive(value(), "--spawn");
        } else if (arg == "--fanout") {
            fanoutSizes = parseList(value(), "--fanout");
        } else if (arg == "--user") {
            user = value();
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return 2;
        }
    }

    auto& logger = Logger::getInstance();
    logger.setLevel(LogLevel::ERROR);
    logger.setLogFile("");

    std::cout << "## Baseline — " << platformString() << " · " << __VERSION__ << " · "
#if defined(NDEBUG)
              << "Release"
#else
              << "Debug"
#endif
              << "\n";

    runSpawnBenchmark(spawnIterations);
    if (!fanoutSizes.empty()) {
        runFanoutBenchmark(fanoutSizes, user);
    }
    return 0;
} catch (const std::exception& ex) {
    std::cerr << "pipeshellx_bench_baseline: " << ex.what() << "\n";
    return 2;
}
