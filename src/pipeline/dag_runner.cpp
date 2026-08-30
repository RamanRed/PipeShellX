#include "psx/pipeline/dag_runner.hpp"

#include "psx/os/io.hpp"
#include "psx/os/pipe.hpp"
#include "psx/os/process.hpp"
#include "psx/pipeline/planner.hpp"
#include "psx/stream/bounded_buffer.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <span>
#include <unordered_map>
#include <utility>

namespace psx::pipeline {

namespace {

bool isLocalPlacement(const std::string& placement) {
    return placement.empty() || placement == "local";
}

int toExitCode(const psx::os::ExitStatus& status) {
    return status.kind == psx::os::ExitStatus::Kind::Exited ? status.code : 128 + status.code;
}

psx::Error invalid(const char* operation) {
    return psx::Error{psx::ErrorClass::InvalidArgument, 0, operation};
}

} // namespace

struct DagRunner::Impl {
    struct Node {
        Stage stage;
        psx::os::Process process;
        psx::os::Handle stdoutReader;
        psx::os::Handle stdinWriter;
        psx::runtime::Token stdoutToken = 0;
        psx::runtime::Token stdinToken = 0;
        std::vector<std::size_t> incoming;
        std::vector<std::size_t> outgoing;
        std::size_t nextIncoming = 0;
        int exitCode = 0;
        bool stdoutClosed = false;
        bool stdoutReadable = true;
        bool stdinOpen = false;
        bool childWatched = false;
        bool exited = false;
    };

    struct EdgeState {
        EdgeState(std::size_t source, std::size_t destination, std::size_t capacity)
            : from(source), to(destination), buffer(capacity, psx::stream::OverflowPolicy::Block) {}

        std::size_t from;
        std::size_t to;
        psx::stream::BoundedBuffer buffer;
        bool sourceClosed = false;
        bool consumerClosed = false;
    };

    Impl(psx::runtime::Reactor& reactor, OnOutput output, std::size_t capacity)
        : reactor(reactor), onOutput(std::move(output)), edgeCapacity(capacity) {}

    ~Impl() { cancel(); }

    void abortRun() noexcept {
        for (Node& node : nodes) {
            if (node.stdoutToken != 0) {
                (void)reactor.unwatch(node.stdoutToken);
                node.stdoutToken = 0;
            }
            if (node.stdinToken != 0) {
                (void)reactor.unwatch(node.stdinToken);
                node.stdinToken = 0;
            }
            if (node.childWatched) {
                (void)reactor.unwatchChild(node.process.id());
                node.childWatched = false;
            }
            node.stdoutReader.close();
            node.stdinWriter.close();
            node.stdoutClosed = true;
            node.stdoutReadable = false;
            node.stdinOpen = false;
        }
        for (EdgeState& edge : edges) {
            edge.buffer.clear();
            edge.sourceClosed = true;
            edge.consumerClosed = true;
        }
        buffered = 0;

        // Moving an empty Process over an owned child kills its whole process
        // group and synchronously reaps the leader (Process' lifecycle rule).
        for (Node& node : nodes) {
            node.process = psx::os::Process{};
            node.exited = true;
        }
        exitedCount = nodes.size();
        onComplete = {};
        done = true;
    }

    psx::Result<void> failStart(psx::Error error) {
        abortRun();
        return error;
    }

    void cancel() noexcept {
        if (!started) {
            return;
        }
        abortRun();
    }

    psx::Result<void> run(const Pipeline& pipeline, std::function<void(Outcome)> complete) {
        if (started) {
            return invalid("dag runner already started");
        }
        if (edgeCapacity == 0) {
            return invalid("dag runner edge capacity is zero");
        }
        for (const Stage& stage : pipeline.stages) {
            if (!isLocalPlacement(stage.placement)) {
                return invalid("dag runner only supports local stages");
            }
        }

        auto planned = Planner::plan(pipeline);
        if (!planned.ok()) {
            return planned.error();
        }
        started = true;
        onComplete = std::move(complete);

        std::unordered_map<std::string, const Stage*> stagesById;
        stagesById.reserve(pipeline.stages.size());
        for (const Stage& stage : pipeline.stages) {
            stagesById.emplace(stage.id, &stage);
        }

        nodes.reserve(planned.value().order.size());
        std::unordered_map<std::string, std::size_t> nodeById;
        nodeById.reserve(planned.value().order.size());
        for (const std::string& id : planned.value().order) {
            nodeById.emplace(id, nodes.size());
            nodes.push_back(Node{.stage = *stagesById.at(id)});
        }

        std::set<std::pair<std::size_t, std::size_t>> uniqueEdges;
        for (const Edge& edge : pipeline.edges) {
            const std::size_t from = nodeById.at(edge.from);
            const std::size_t to = nodeById.at(edge.to);
            if (!uniqueEdges.emplace(from, to).second) {
                continue;
            }
            const std::size_t index = edges.size();
            edges.emplace_back(from, to, edgeCapacity);
            nodes[from].outgoing.push_back(index);
            nodes[to].incoming.push_back(index);
        }

        std::vector<psx::os::Pipe> inputs(nodes.size());
        std::vector<psx::os::Pipe> outputs;
        outputs.reserve(nodes.size());
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            auto output = psx::os::Pipe::create();
            if (!output.ok()) {
                return failStart(output.error());
            }
            outputs.push_back(std::move(output.value()));
            if (!nodes[i].incoming.empty()) {
                auto input = psx::os::Pipe::create();
                if (!input.ok()) {
                    return failStart(input.error());
                }
                inputs[i] = std::move(input.value());
            }
        }

        for (std::size_t i = 0; i < nodes.size(); ++i) {
            psx::os::SpawnSpec spec;
            spec.program = nodes[i].stage.argv.front();
            spec.argv = nodes[i].stage.argv;
            spec.in = nodes[i].incoming.empty() ? psx::os::SpawnSpec::Stdio::null()
                                                : psx::os::SpawnSpec::Stdio::from(inputs[i].reader);
            spec.out = psx::os::SpawnSpec::Stdio::from(outputs[i].writer);
            auto process = psx::os::Process::spawn(spec);
            if (!process.ok()) {
                return failStart(process.error());
            }
            nodes[i].process = std::move(process.value());
        }

        // The children own their duplicated stdio ends now. Keeping no parent
        // copies is essential for deterministic EOF propagation.
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            outputs[i].writer.close();
            nodes[i].stdoutReader = std::move(outputs[i].reader);
            if (auto nonBlocking = nodes[i].stdoutReader.setNonBlocking(true); !nonBlocking.ok()) {
                return failStart(nonBlocking.error());
            }
            if (!nodes[i].incoming.empty()) {
                inputs[i].reader.close();
                nodes[i].stdinWriter = std::move(inputs[i].writer);
                if (auto nonBlocking = nodes[i].stdinWriter.setNonBlocking(true); !nonBlocking.ok()) {
                    return failStart(nonBlocking.error());
                }
                nodes[i].stdinOpen = true;
            }
        }

        for (std::size_t i = 0; i < nodes.size(); ++i) {
            auto outputWatch = reactor.watch(nodes[i].stdoutReader, psx::os::Interest::Readable,
                                             [this, i](psx::os::Readiness) { onProducerReadable(i); });
            if (!outputWatch.ok()) {
                return failStart(outputWatch.error());
            }
            nodes[i].stdoutToken = outputWatch.value();

            if (nodes[i].stdinOpen) {
                auto inputWatch = reactor.watch(nodes[i].stdinWriter, psx::os::Interest::None,
                                                [this, i](psx::os::Readiness) { drainConsumer(i); });
                if (!inputWatch.ok()) {
                    return failStart(inputWatch.error());
                }
                nodes[i].stdinToken = inputWatch.value();
            }
        }

        for (std::size_t i = 0; i < nodes.size(); ++i) {
            auto childWatch =
                reactor.watchChild(nodes[i].process.id(), [this, i](psx::os::ProcessId) { onChildExit(i); });
            if (childWatch.ok()) {
                nodes[i].childWatched = true;
                continue;
            }
            if (childWatch.error().cls == psx::ErrorClass::NoSuchProcess) {
                onChildExit(i);
                continue;
            }
            return failStart(childWatch.error());
        }
        return {};
    }

    void onProducerReadable(std::size_t nodeIndex) {
        Node& node = nodes[nodeIndex];
        std::array<char, 16U * 1024U> storage{};
        while (!node.stdoutClosed) {
            std::size_t limit = storage.size();
            bool hasActiveEdge = false;
            for (const std::size_t edgeIndex : node.outgoing) {
                const EdgeState& edge = edges[edgeIndex];
                if (edge.consumerClosed) {
                    continue;
                }
                hasActiveEdge = true;
                limit = std::min(limit, edge.buffer.available());
            }
            if (hasActiveEdge && limit == 0) {
                setProducerReadable(nodeIndex, false);
                return;
            }

            auto got = psx::os::read(node.stdoutReader, std::span<char>(storage.data(), limit));
            if (got.ok()) {
                if (got.value() == 0) {
                    closeProducer(nodeIndex);
                    return;
                }
                if (node.outgoing.empty()) {
                    if (onOutput) {
                        onOutput(std::string_view(storage.data(), got.value()));
                    }
                    continue;
                }

                std::vector<std::size_t> consumers;
                consumers.reserve(node.outgoing.size());
                for (const std::size_t edgeIndex : node.outgoing) {
                    EdgeState& edge = edges[edgeIndex];
                    if (edge.consumerClosed) {
                        continue;
                    }
                    const std::size_t accepted = edge.buffer.append(std::span<const char>(storage.data(), got.value()));
                    buffered += accepted;
                    peakBuffered = std::max(peakBuffered, buffered);
                    consumers.push_back(edge.to);
                }
                std::sort(consumers.begin(), consumers.end());
                consumers.erase(std::unique(consumers.begin(), consumers.end()), consumers.end());
                for (const std::size_t consumer : consumers) {
                    drainConsumer(consumer);
                }
                continue;
            }
            if (got.error().cls == psx::ErrorClass::WouldBlock) {
                return;
            }
            closeProducer(nodeIndex);
            return;
        }
    }

    void closeProducer(std::size_t nodeIndex) {
        Node& node = nodes[nodeIndex];
        if (node.stdoutClosed) {
            return;
        }
        node.stdoutClosed = true;
        node.stdoutReadable = false;
        if (node.stdoutToken != 0) {
            (void)reactor.unwatch(node.stdoutToken);
            node.stdoutToken = 0;
        }
        node.stdoutReader.close();

        std::vector<std::size_t> consumers;
        consumers.reserve(node.outgoing.size());
        for (const std::size_t edgeIndex : node.outgoing) {
            edges[edgeIndex].sourceClosed = true;
            consumers.push_back(edges[edgeIndex].to);
        }
        std::sort(consumers.begin(), consumers.end());
        consumers.erase(std::unique(consumers.begin(), consumers.end()), consumers.end());
        for (const std::size_t consumer : consumers) {
            drainConsumer(consumer);
        }
        finishIfDone();
    }

    void drainConsumer(std::size_t nodeIndex) {
        Node& node = nodes[nodeIndex];
        if (!node.stdinOpen) {
            return;
        }

        while (true) {
            bool foundData = false;
            const std::size_t incomingCount = node.incoming.size();
            for (std::size_t offset = 0; offset < incomingCount; ++offset) {
                const std::size_t slot = (node.nextIncoming + offset) % incomingCount;
                EdgeState& edge = edges[node.incoming[slot]];
                if (edge.buffer.empty()) {
                    continue;
                }
                foundData = true;
                const std::span<const char> bytes = edge.buffer.peek();
                auto wrote = psx::os::write(node.stdinWriter, bytes);
                if (wrote.ok() && wrote.value() > 0) {
                    edge.buffer.drop(wrote.value());
                    buffered -= wrote.value();
                    node.nextIncoming = (slot + 1) % incomingCount;
                    setProducerReadable(edge.from, producerCanRead(edge.from));
                    break;
                }
                if (wrote.ok() || wrote.error().cls == psx::ErrorClass::WouldBlock) {
                    setConsumerWritable(nodeIndex, true);
                    return;
                }
                closeConsumer(nodeIndex);
                return;
            }
            if (!foundData) {
                break;
            }
        }

        const bool allClosed = std::all_of(node.incoming.begin(), node.incoming.end(), [this](std::size_t edgeIndex) {
            return edges[edgeIndex].sourceClosed || edges[edgeIndex].consumerClosed;
        });
        if (allClosed) {
            closeConsumer(nodeIndex);
        } else {
            setConsumerWritable(nodeIndex, false);
        }
    }

    void closeConsumer(std::size_t nodeIndex) {
        Node& node = nodes[nodeIndex];
        if (!node.stdinOpen) {
            return;
        }
        if (node.stdinToken != 0) {
            (void)reactor.unwatch(node.stdinToken);
            node.stdinToken = 0;
        }
        node.stdinWriter.close();
        node.stdinOpen = false;

        for (const std::size_t edgeIndex : node.incoming) {
            EdgeState& edge = edges[edgeIndex];
            if (!edge.buffer.empty()) {
                buffered -= edge.buffer.size();
                edge.buffer.clear();
            }
            edge.consumerClosed = true;
            setProducerReadable(edge.from, producerCanRead(edge.from));
        }
    }

    bool producerCanRead(std::size_t nodeIndex) const {
        for (const std::size_t edgeIndex : nodes[nodeIndex].outgoing) {
            const EdgeState& edge = edges[edgeIndex];
            if (edge.consumerClosed) {
                continue;
            }
            if (edge.buffer.full()) {
                return false;
            }
        }
        // A sink, or a producer whose consumers all closed, must still be
        // drained so that it can terminate instead of blocking on stdout.
        return true;
    }

    void setProducerReadable(std::size_t nodeIndex, bool readable) {
        Node& node = nodes[nodeIndex];
        if (node.stdoutClosed || node.stdoutToken == 0 || node.stdoutReadable == readable) {
            return;
        }
        node.stdoutReadable = readable;
        (void)reactor.modify(node.stdoutToken, readable ? psx::os::Interest::Readable : psx::os::Interest::None);
    }

    void setConsumerWritable(std::size_t nodeIndex, bool writable) {
        Node& node = nodes[nodeIndex];
        if (!node.stdinOpen || node.stdinToken == 0) {
            return;
        }
        (void)reactor.modify(node.stdinToken, writable ? psx::os::Interest::Writable : psx::os::Interest::None);
    }

    void onChildExit(std::size_t nodeIndex) {
        Node& node = nodes[nodeIndex];
        if (node.exited) {
            return;
        }
        node.childWatched = false;
        psx::os::ExitStatus status{psx::os::ExitStatus::Kind::Exited, 1};
        auto nonBlocking = node.process.tryWait();
        if (nonBlocking.ok() && nonBlocking.value().has_value()) {
            status = *nonBlocking.value();
        } else {
            auto blocking = node.process.wait();
            if (blocking.ok()) {
                status = blocking.value();
            }
        }
        node.exitCode = toExitCode(status);
        node.exited = true;
        ++exitedCount;
        closeConsumer(nodeIndex);
        finishIfDone();
    }

    void finishIfDone() {
        if (done || exitedCount != nodes.size() ||
            !std::all_of(nodes.begin(), nodes.end(), [](const Node& node) { return node.stdoutClosed; })) {
            return;
        }
        done = true;
        Outcome outcome;
        outcome.stageExitCodes.reserve(nodes.size());
        outcome.topologicalOrder.reserve(nodes.size());
        for (const Node& node : nodes) {
            outcome.topologicalOrder.push_back(node.stage.id);
            outcome.stageExitCodes.push_back(node.exitCode);
            if (node.exitCode != 0) {
                outcome.exitCode = node.exitCode;
            }
        }
        if (onComplete) {
            auto callback = std::move(onComplete);
            callback(std::move(outcome));
        }
    }

    std::size_t activeChildCount() const noexcept {
        return static_cast<std::size_t>(
            std::count_if(nodes.begin(), nodes.end(), [](const Node& node) { return node.process.running(); }));
    }

    psx::runtime::Reactor& reactor;
    OnOutput onOutput;
    std::function<void(Outcome)> onComplete;
    std::size_t edgeCapacity;
    std::vector<Node> nodes;
    std::vector<EdgeState> edges;
    std::size_t buffered = 0;
    std::size_t peakBuffered = 0;
    std::size_t exitedCount = 0;
    bool started = false;
    bool done = false;
};

DagRunner::DagRunner(psx::runtime::Reactor& reactor, OnOutput onOutput, std::size_t edgeCapacity)
    : impl_(std::make_unique<Impl>(reactor, std::move(onOutput), edgeCapacity)) {}

DagRunner::~DagRunner() = default;

psx::Result<void> DagRunner::run(const Pipeline& pipeline, std::function<void(Outcome)> onComplete) {
    return impl_->run(pipeline, std::move(onComplete));
}

void DagRunner::cancel() noexcept {
    impl_->cancel();
}

std::size_t DagRunner::activeChildCount() const noexcept {
    return impl_->activeChildCount();
}

std::size_t DagRunner::bufferedBytes() const noexcept {
    return impl_->buffered;
}

std::size_t DagRunner::peakBufferedBytes() const noexcept {
    return impl_->peakBuffered;
}

} // namespace psx::pipeline
