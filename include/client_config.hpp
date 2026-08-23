#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ClientEntry {
    std::string raw;
    std::string user;
    std::string host;
    std::uint16_t port{22};
    std::string identityFile;
    std::string password;
    // Absolute path of the per-inventory known_hosts file (derived from the
    // inventory path by ClientConfig::knownHostsPathFor; never serialized).
    std::string knownHostsFile;
    // Native backplane (psx/1) per-host options; ignored by the SSH transport.
    std::string expectedSan;     // pin the node's SAN-URI identity (empty = trust the CA only)
    std::uint16_t nativePort{0}; // node port (0 = use the run's --native-port)

    std::string clientId() const;
    std::string sshTarget() const;
    std::string serialize() const;
};

class ClientConfig {
public:
    // `inventoryPath` names the file whose `<path>.known_hosts` trust store is
    // attached to every entry produced by makeEntry() or loadFromFile().
    explicit ClientConfig(std::string inventoryPath = {});

    void loadFromFile(const std::string& path);
    void saveToFile(const std::string& path) const;
    void setClients(std::vector<ClientEntry> clients);

    const std::vector<ClientEntry>& clients() const noexcept;
    bool empty() const noexcept;

    // Pure parse: no inventory-derived state is attached.
    static ClientEntry parseEntry(const std::string& line);
    static bool isValidEntry(const std::string& line) noexcept;

    // parseEntry() plus this inventory's known_hosts path (when it has one).
    ClientEntry makeEntry(const std::string& specification) const;
    const std::string& inventoryPath() const noexcept;

    // `<absolute inventory path>.known_hosts` — one trust store per fleet file.
    static std::string knownHostsPathFor(const std::string& inventoryPath);

private:
    std::string inventoryPath_;
    std::vector<ClientEntry> clients_;
};
