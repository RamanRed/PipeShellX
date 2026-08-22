#include "psx/transport/node_stage_runner.hpp"

#include "psx/os/io.hpp"
#include "psx/os/pipe.hpp"

#include <array>
#include <utility>

namespace psx::transport {

using psx::os::ExitStatus;
using psx::os::Interest;
using psx::os::Pipe;
using psx::os::Readiness;
using psx::os::SpawnSpec;

NodeStageRunner::NodeStageRunner(psx::runtime::Reactor& reactor) : reactor_(reactor) {}

NodeStageRunner::~NodeStageRunner() {
    for (auto& [id, stage] : stages_) {
        if (stage.stdoutToken != 0) {
            (void)reactor_.unwatch(stage.stdoutToken);
        }
        if (stage.stderrToken != 0) {
            (void)reactor_.unwatch(stage.stderrToken);
        }
        if (!stage.exited) {
            (void)reactor_.unwatchChild(stage.process.id());
        }
    }
}

void NodeStageRunner::onOpen(StreamId id, const OpenRequest& request) {
    if (request.argv.empty()) {
        session_->sendExit(id, {ExitStatus::Kind::Exited, 127});
        return;
    }

    auto stdoutPipe = Pipe::create();
    auto stderrPipe = Pipe::create();
    if (!stdoutPipe.ok() || !stderrPipe.ok()) {
        session_->sendExit(id, {ExitStatus::Kind::Exited, 127});
        return;
    }

    SpawnSpec spec;
    spec.program = request.argv.front(); // PATH lookup when it has no '/'
    spec.argv = request.argv;
    spec.cwd = request.cwd;
    spec.in = SpawnSpec::Stdio::null();
    spec.out = SpawnSpec::Stdio::from(stdoutPipe.value().writer);
    spec.err = SpawnSpec::Stdio::from(stderrPipe.value().writer);

    auto process = psx::os::Process::spawn(spec);
    if (!process.ok()) {
        session_->sendExit(id, {ExitStatus::Kind::Exited, 127}); // could not start
        return;
    }

    Stage stage;
    stage.process = std::move(process.value());
    stage.stdoutReader = std::move(stdoutPipe.value().reader);
    stage.stderrReader = std::move(stderrPipe.value().reader);
    auto [it, inserted] = stages_.emplace(id, std::move(stage));
    // The pipe write ends close here (only the child holds them now).
    arm(id, it->second);
}

void NodeStageRunner::arm(StreamId id, Stage& stage) {
    auto watchOut = reactor_.watch(stage.stdoutReader, Interest::Readable,
                                   [this, id](Readiness) { onReadable(id, /*isStdout=*/true); });
    if (watchOut.ok()) {
        stage.stdoutToken = watchOut.value();
    }
    auto watchErr = reactor_.watch(stage.stderrReader, Interest::Readable,
                                   [this, id](Readiness) { onReadable(id, /*isStdout=*/false); });
    if (watchErr.ok()) {
        stage.stderrToken = watchErr.value();
    }
    if (auto watched = reactor_.watchChild(stage.process.id(), [this, id](psx::os::ProcessId) { onStageExit(id); });
        !watched.ok()) {
        // The child may have exited before we watched it (fast commands); the exit
        // source then reports NoSuchProcess. Treat it as already-exited.
        if (watched.error().cls == psx::ErrorClass::NoSuchProcess) {
            onStageExit(id);
        }
    }
}

void NodeStageRunner::onReadable(StreamId id, bool isStdout) {
    auto it = stages_.find(id);
    if (it == stages_.end()) {
        return;
    }
    Stage& stage = it->second;
    psx::os::Handle& reader = isStdout ? stage.stdoutReader : stage.stderrReader;

    std::array<char, 16 * 1024> buffer{};
    while (true) { // edge-triggered: drain to WouldBlock
        auto got = psx::os::read(reader, std::span<char>(buffer.data(), buffer.size()));
        if (got.ok()) {
            if (got.value() == 0) {
                closeReader(isStdout, stage);
                break;
            }
            session_->sendData(id, std::string_view(buffer.data(), got.value()), /*endStream=*/false);
            continue;
        }
        if (got.error().cls == psx::ErrorClass::WouldBlock) {
            break;
        }
        closeReader(isStdout, stage); // EBADF/EIO: nothing more will arrive
        break;
    }
    finishIfDone(id);
}

void NodeStageRunner::closeReader(bool isStdout, Stage& stage) {
    psx::runtime::Token& token = isStdout ? stage.stdoutToken : stage.stderrToken;
    bool& open = isStdout ? stage.stdoutOpen : stage.stderrOpen;
    if (!open) {
        return;
    }
    if (token != 0) {
        (void)reactor_.unwatch(token);
        token = 0;
    }
    (isStdout ? stage.stdoutReader : stage.stderrReader).close();
    open = false;
}

void NodeStageRunner::onStageExit(StreamId id) {
    auto it = stages_.find(id);
    if (it == stages_.end()) {
        return;
    }
    Stage& stage = it->second;
    if (auto status = stage.process.tryWait(); status.ok() && status.value().has_value()) {
        stage.status = *status.value();
    } else if (auto blocking = stage.process.wait(); blocking.ok()) {
        stage.status = blocking.value();
    }
    stage.exited = true;
    finishIfDone(id);
}

void NodeStageRunner::finishIfDone(StreamId id) {
    auto it = stages_.find(id);
    if (it == stages_.end()) {
        return;
    }
    const Stage& stage = it->second;
    if (stage.exited && !stage.stdoutOpen && !stage.stderrOpen) {
        session_->sendExit(id, stage.status);
        stages_.erase(it);
    }
}

} // namespace psx::transport
