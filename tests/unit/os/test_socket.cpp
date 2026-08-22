#include "psx/os/socket.hpp"

#include "psx/os/io.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <span>
#include <string>
#include <thread>

using psx::os::Socket;

namespace {

// Polls a non-blocking accept() for up to ~2 s (loopback completes fast).
Socket acceptWithin(const Socket& listener) {
    for (int i = 0; i < 2000; ++i) {
        auto accepted = listener.accept();
        if (accepted.ok()) {
            return std::move(accepted.value());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return Socket{};
}

// Reads exactly n bytes, polling past WouldBlock.
std::string readN(const Socket& sock, std::size_t n) {
    std::string out;
    char buffer[256];
    for (int i = 0; i < 2000 && out.size() < n; ++i) {
        auto got = psx::os::read(sock.handle(), std::span<char>(buffer, sizeof(buffer)));
        if (got.ok() && got.value() > 0) {
            out.append(buffer, got.value());
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return out;
}

} // namespace

TEST(SocketTest, LoopbackConnectAcceptAndBidirectionalTransfer) {
    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok()) << (listener.ok() ? "" : listener.error().message());
    const auto port = listener.value().localPort();
    ASSERT_TRUE(port.ok());
    EXPECT_NE(port.value(), 0);

    auto client = Socket::connect("127.0.0.1", port.value());
    ASSERT_TRUE(client.ok()) << (client.ok() ? "" : client.error().message());

    Socket server = acceptWithin(listener.value());
    ASSERT_TRUE(server.valid()) << "accept did not complete";

    // The non-blocking connect has completed by now (loopback).
    for (int i = 0; i < 2000 && !client.value().connectResult().ok(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(client.value().connectResult().ok());

    // client -> server
    const std::string ping = "ping";
    auto wrote = psx::os::write(client.value().handle(), std::span<const char>(ping.data(), ping.size()));
    ASSERT_TRUE(wrote.ok());
    EXPECT_EQ(readN(server, ping.size()), ping);

    // server -> client
    const std::string pong = "pong!";
    ASSERT_TRUE(psx::os::write(server.handle(), std::span<const char>(pong.data(), pong.size())).ok());
    EXPECT_EQ(readN(client.value(), pong.size()), pong);
}

TEST(SocketTest, ConnectToAClosedPortFailsPromptly) {
    // Bind+listen then close to obtain a port that is very likely free.
    std::uint16_t deadPort = 0;
    {
        auto probe = Socket::listen("127.0.0.1", 0);
        ASSERT_TRUE(probe.ok());
        deadPort = probe.value().localPort().value();
    } // listener closed here

    auto client = Socket::connect("127.0.0.1", deadPort);
    // connect() may return in-progress; connectResult then reports the refusal.
    if (client.ok()) {
        psx::Result<void> result{};
        for (int i = 0; i < 2000; ++i) {
            result = client.value().connectResult();
            if (!result.ok()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_FALSE(result.ok()) << "connect to a closed port should fail";
    }
}

TEST(SocketTest, AcceptWithNoPendingConnectionWouldBlock) {
    auto listener = Socket::listen("127.0.0.1", 0);
    ASSERT_TRUE(listener.ok());
    auto accepted = listener.value().accept();
    EXPECT_FALSE(accepted.ok());
    EXPECT_EQ(accepted.error().cls, psx::ErrorClass::WouldBlock);
}
