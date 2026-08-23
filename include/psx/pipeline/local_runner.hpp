#pragma once

#include "psx/os/handle.hpp"
#include "psx/os/process.hpp"
#include "psx/pipeline/pipeline.hpp"
#include "psx/result.hpp"
#include "psx/runtime/reactor.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace psx::pipeline {

// Runs a linear chain of local stages as a Unix pipeline on the reactor: each
// stage's stdout feeds the next stage's stdin, the first stage's stdin is empty,
// the last stage's stdout is delivered to onOutput, and stderr is inherited.
// onComplete fires once every stage has exited and the final stdout has drained.
// Non-blocking (no pipe deadlock); one run per instance; not thread-safe.
class LocalRunner {
public:
    struct Outcome {
        // pipefail: the rightmost non-zero stage code, or 0 if all succeeded.
        int exitCode = 0;
        std::vector<int> stageExitCodes; // per stage, in order (128+signal if killed)
    };
    using OnOutput = std::function<void(std::string_view)>;

    explicit LocalRunner(psx::runtime::Reactor& reactor, OnOutput onOutput = {});
    ~LocalRunner();
    LocalRunner(const LocalRunner&) = delete;
    LocalRunner& operator=(const LocalRunner&) = delete;

    // Spawn the chain (`stages` non-empty, in execution order). Returns an error
    // only for a pipe/spawn failure; a non-zero stage exit is a normal Outcome.
    // onComplete runs on the reactor thread when the pipeline finishes. With
    // externalStdin, the first stage's stdin is fed by writeStdin()/closeStdin()
    // instead of being empty -- so a local segment can be spliced after an
    // upstream (remote) segment.
    psx::Result<void>
    run(const std::vector<Stage>& stages, std::function<void(Outcome)> onComplete, bool externalStdin = false);

    // Feed the first stage's stdin (only when run(..., externalStdin=true)).
    // Non-blocking and buffered; closeStdin() sends EOF.
    void writeStdin(std::string_view bytes);
    void closeStdin();

private:
    struct Child {
        psx::os::Process process;
        bool exited = false;
        int exitCode = 0;
    };
    void onFinalReadable();
    void onChildExit(std::size_t index);
    void drainStdin();
    void onStdinWritable();
    void finishIfDone();

    psx::runtime::Reactor& reactor_;
    OnOutput onOutput_;
    std::function<void(Outcome)> onComplete_;
    std::vector<Child> children_;
    psx::os::Handle finalReader_;
    psx::runtime::Token finalToken_ = 0;
    psx::os::Handle stdinWriter_; // first stage stdin, when externalStdin
    std::string stdinBuffer_;
    psx::runtime::Token stdinToken_ = 0;
    bool stdinOpen_ = false;
    bool stdinEndPending_ = false;
    bool finalClosed_ = false;
    std::size_t exitedCount_ = 0;
    bool done_ = false;
};

} // namespace psx::pipeline
