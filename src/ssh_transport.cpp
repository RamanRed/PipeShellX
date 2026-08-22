#include "ssh_transport.hpp"

#include "psx/os/io.hpp"
#include "ssh_auth.hpp"

#include <span>

psx::Result<void>
SshTransport::prepare(const ClientEntry& client, const std::string& remoteCommand, Prepared& out) const {
    out = Prepared{};

    // Passwords never touch argv: sshpass reads the secret from an inherited
    // pipe created here (non-inheritable everywhere else).
    if (!client.password.empty()) {
        if (client.password.size() >= kMaxPasswordBytes) {
            out.failure = "password too long for secure hand-off\n";
            return {};
        }
        auto created = psx::os::Pipe::create();
        if (!created.ok()) {
            return created.error();
        }
        out.passwordPipe = std::move(created.value());
        const std::string secret = client.password + "\n";
        auto written = psx::os::write(out.passwordPipe.writer, std::span<const char>(secret.data(), secret.size()));
        if (!written.ok() || written.value() != secret.size()) {
            return written.ok() ? psx::Error{psx::ErrorClass::Other, 0, "password pipe write"} : written.error();
        }
        out.passwordPipe.writer.close();
        out.spec.extraHandles = {{&out.passwordPipe.reader, kPasswordFd}};
    }

    SshOptions sshOptions;
    sshOptions.controlPath = options_.controlPath;
    sshOptions.controlPersistSeconds = options_.controlPersistSeconds;
    out.spec.argv =
        buildSshCommandArguments(client, remoteCommand, client.password.empty() ? -1 : kPasswordFd, sshOptions);
    out.spec.program = out.spec.argv.front();
    return {};
}
