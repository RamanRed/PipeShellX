#pragma once

#include "psx/os/socket.hpp"
#include "psx/os/tls.hpp"
#include "psx/result.hpp"
#include "psx/runtime/reactor.hpp"
#include "psx/transport/native_transport.hpp"
#include "psx/transport/node_stage_runner.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace psx::transport {

// The psx/1 node daemon: accepts mTLS connections on a listening socket and, per
// connection, runs a NativeTransport(Role::Node) + NodeStageRunner so a
// controller can execute stages. All on one reactor; not thread-safe.
class NodeServer {
public:
    // serverConfig supplies the node's cert/key and the CA it trusts; isServer is
    // forced on. authorize gates accepted peers by SAN URI (empty = authN only).
    NodeServer(psx::runtime::Reactor& reactor,
               psx::os::Socket listener,
               psx::os::TlsConfig serverConfig,
               std::function<bool(std::string_view)> authorize = {});
    ~NodeServer();
    NodeServer(const NodeServer&) = delete;
    NodeServer& operator=(const NodeServer&) = delete;

    // Begins accepting connections.
    psx::Result<void> start();

    std::size_t connectionCount() const noexcept { return connections_.size(); }

private:
    struct Connection {
        std::unique_ptr<NodeStageRunner> runner;
        std::unique_ptr<NativeTransport> transport; // references runner; destroyed first
    };

    void onAccept();
    void acceptOne(psx::os::Socket socket);
    void dropConnection(std::uint64_t id);
    void reap();

    psx::runtime::Reactor& reactor_;
    psx::os::Socket listener_;
    psx::os::TlsConfig serverConfig_;
    std::function<bool(std::string_view)> authorize_;
    psx::runtime::Token listenerToken_ = 0;
    std::unordered_map<std::uint64_t, Connection> connections_;
    std::vector<std::uint64_t> pendingRemoval_;
    std::uint64_t nextConnId_ = 1;
    bool reapScheduled_ = false;
};

} // namespace psx::transport
