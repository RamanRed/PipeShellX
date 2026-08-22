#pragma once

#include "psx/os/handle.hpp"
#include "psx/os/process.hpp"
#include "psx/runtime/reactor.hpp"
#include "psx/transport/session.hpp"

#include <unordered_map>

namespace psx::transport {

// The node side of the backplane: a SessionHandler that runs each opened stream
// as a local process, streaming its stdout/stderr back as DATA frames and an
// EXIT frame when it finishes. Stage stdout/stderr are read on the reactor.
// Not thread-safe (single reactor thread).
//
// Backpressure note: this slice does not yet gate reads on the send window — if
// the controller withholds credit, the Session buffers the stage's output in
// memory. A follow-up will deregister a stage's read interest when its stream's
// send window is exhausted and resume on WINDOW_UPDATE.
class NodeStageRunner : public SessionHandler {
public:
    // The Session this runner sends stage output/exit on must be bound() before
    // any stream is opened (it is the Session that dispatches onOpen to us).
    explicit NodeStageRunner(psx::runtime::Reactor& reactor);
    void bind(Session& session) noexcept { session_ = &session; }
    ~NodeStageRunner() override;
    NodeStageRunner(const NodeStageRunner&) = delete;
    NodeStageRunner& operator=(const NodeStageRunner&) = delete;

    void onOpen(StreamId id, const OpenRequest& request) override;

    std::size_t runningStages() const noexcept { return stages_.size(); }

private:
    struct Stage {
        psx::os::Process process;
        psx::os::Handle stdoutReader;
        psx::os::Handle stderrReader;
        psx::runtime::Token stdoutToken = 0;
        psx::runtime::Token stderrToken = 0;
        bool stdoutOpen = true;
        bool stderrOpen = true;
        bool exited = false;
        psx::os::ExitStatus status{};
    };

    void arm(StreamId id, Stage& stage);
    void onReadable(StreamId id, bool isStdout);
    void onStageExit(StreamId id);
    void closeReader(bool isStdout, Stage& stage);
    void finishIfDone(StreamId id);

    psx::runtime::Reactor& reactor_;
    Session* session_ = nullptr;
    std::unordered_map<StreamId, Stage> stages_;
};

} // namespace psx::transport
