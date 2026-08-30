# Authentication and Inventory

PipeShellX has two remote transports:

- SSH delegates identity, host trust, and target-shell behavior to the system
  OpenSSH client.
- The optional psx/1 native backplane uses mutual TLS 1.3 with a fleet CA.

Transport authentication proves who may connect. It does not make commands
safe or sandbox them; see [Security](security.md).

## Inventory

The current INI format groups hosts and carries transport-specific metadata:

```ini
[defaults]
user = deploy
port = 22
identity = /home/operator/.ssh/fleet_ed25519

[web]
web-01 tag=prod,blue transport=ssh
web-02 user=release port=2222 transport=ssh

[native]
node-01 transport=native san=spiffe://psx/node/node-01 native_port=7433
```

Supported per-host keys are `user`, `port`,
`identity`, `tag`, `transport`,
`san`, and `native_port`. Transport is strictly
`ssh` or `native`; another value is a configuration error.
`port` and `native_port` must be in `1..65535`.

### Resolution precedence

Commands that use normal inventory resolution choose the first candidate in
this order:

1. explicit `-i FILE` / `--inventory FILE`;
2. non-empty `PIPESHELLX_INVENTORY`;
3. `./inventory.ini`;
4. legacy `./clients.txt`;
5. `$XDG_CONFIG_HOME/pipeshellx/inventory.ini`, or
   `$HOME/.config/pipeshellx/inventory.ini` when XDG is unset.

This precedence is used by `run`, `ping`, and
`hosts list`. Native `diff` accepts the same resolver even
though its usage normally supplies `-i`. A remote
`pipe` requires its explicit `-i FILE`.

Selectors `-g GROUP`, `-t TAG`, and
`-H h1,h2` are mutually exclusive. With no selector,
`run` and `ping` select every inventory host.

### Per-host transport routing

`run` honors the inventory's per-host `transport`:

- without `--transport`, all selected hosts must have the same
  transport;
- a mixed SSH/native selection exits `2` and asks for an explicit
  override;
- `--transport ssh` or `--transport native` overrides every
  selected host for that invocation.

Transport-specific options are rejected with the other transport. For example,
`--reuse`, `--retries`, and `--shell` are SSH
options; certificates, CRL, native port, and canary are native options.

`ping` currently probes SSH only and rejects any selected native host.
`diff` and remote `pipe` use the native transport.

### Safe host administration

Listing may use normal precedence:

```bash
pipeshellx hosts list -i fleet.ini
```

The output columns are `HOST`, `GROUPS`, `TAGS`,
and `TRANSPORT`.

Mutations require an explicit INI target:

```bash
pipeshellx hosts add web-03 -i fleet.ini --group web \
  --user deploy --transport ssh --identity /home/operator/.ssh/fleet_ed25519

pipeshellx hosts add node-02 -i fleet.ini --group native \
  --transport native --san spiffe://psx/node/node-02 --native-port 7433

pipeshellx hosts remove web-03 -i fleet.ini
pipeshellx hosts import clients.txt -i fleet.ini
```

Add/import rejects duplicate hosts. Add rejects embedded URLs, query strings,
`user:password@host`, and secret option names. Import preserves
supported legacy connection metadata such as
`ssh://deploy@host:2222?identity=/path/to/key` while discarding
recognized legacy secret query values. No secret is serialized.

Mutations refuse an INI target whose basename is `clients.txt`,
because that name always invokes legacy parsing. Rewrites use a same-directory
temporary file plus atomic rename; existing permissions are preserved and a
new POSIX target is created private.

## SSH authentication

PipeShellX does not embed an SSH stack
([ADR-002](adr/ADR-002-system-openssh-as-agentless-transport.md)). Each worker
starts `ssh` resolved from `PATH` with the selected user,
port, identity file, and these defaults:

```text
-o StrictHostKeyChecking=accept-new
-o UserKnownHostsFile=<inventory>.known_hosts
-o BatchMode=yes
-o ConnectTimeout=5
-o ServerAliveInterval=15
```

This supports keys, certificates, `ssh-agent`,
`~/.ssh/config`, host aliases, and jump/proxy rules implemented by
OpenSSH. `run` has no password option; unattended use should use
keys, agents, or SSH certificates.

`accept-new` is trust on first use: an unknown host is recorded, but a
changed key is rejected. For stronger first-contact trust, pre-seed
`<inventory>.known_hosts` with fingerprints verified out of band.
When rotation is legitimate, inspect it before removing the stale entry, for
example:

```bash
ssh-keygen -R web-01 -f fleet.ini.known_hosts
```

The legacy interactive shell can keep a password in memory for its session and
passes it to `sshpass -d <fd>` over a pipe, never on argv. That
compatibility path does not persist the password and does not change the
recommended noninteractive model. Its ordinary string storage is not locked or
guaranteed to be zeroed.

SSH argv is serialized for the target remote shell. Choose
`--shell posix|cmd|powershell` to match the target. OpenSSH then
invokes that shell; PipeShellX does not claim shell-free remote execution.

## Native mTLS identity

Native mode requires a controller certificate, private key, and CA:

```bash
pipeshellx run -i fleet.ini -g native --transport native \
  --cert controller.crt --key controller.key --ca ca/ca.crt -- uname -a
```

Both peers require and verify a CA-signed certificate. Identity is the
certificate's SAN URI:

- inventory `san=<uri>` pins the expected node identity exactly;
- node `--allow SAN[,SAN...]` restricts controller identities;
- omitting the inventory SAN trusts any CA-signed node;
- omitting node `--allow` admits any CA-signed controller and logs a
  warning.

Start a node with an explicit controller allowlist:

```bash
pipeshellx node --listen 0.0.0.0:7433 \
  --cert node.crt --key node.key --ca ca/ca.crt \
  --allow spiffe://psx/controller/ops --policy /etc/pipeshellx/node.policy
```

`--policy` is optional node-side defense in depth. It rejects
disallowed argv before process creation with stage exit `126`.
Without it, an identity admitted by the CA/allowlist may request arbitrary
argv.

### Revocation

Revoke a certificate or serial and distribute the regenerated CRL:

```bash
pipeshellx ca revoke --ca ca --cert node-07.crt

pipeshellx node --listen 0.0.0.0:7433 \
  --cert node.crt --key node.key --ca ca/ca.crt --crl ca/crl.pem

pipeshellx run -i fleet.ini -g native --transport native \
  --cert controller.crt --key controller.key --ca ca/ca.crt \
  --crl ca/crl.pem -- uptime
```

CRL checking is opt-in on each endpoint. Operational distribution and refresh
are the administrator's responsibility.

### Node-generated keys and CSR enrollment

Keep the private key on the node:

```bash
# Node: writes PREFIX.key and PREFIX.csr
pipeshellx node keygen --san spiffe://psx/node/node-01 \
  --out /etc/pipeshellx/node

# CA host: verifies the CSR signature and applies the operator-approved SAN
pipeshellx ca sign --ca ca --csr node.csr \
  --san spiffe://psx/node/node-01 --out node.crt
```

Only the CSR and issued certificate need to move. The repository does not yet
automate that transfer over SSH.

## Native build and protocol limits

Native support requires OpenSSL 3. Configure an SSH-only build with
`-DPIPESHELLX_NATIVE_TRANSPORT=OFF`; that build reports a clear
configuration error for `node`, `ca`, `diff`,
native `run`, and remote `pipe` features.

psx/1 has credit flow control, distinct stdout/stderr channels, leases, drain,
and connection-loss fencing. It has no reconnect/resume, and native Windows
controller/node support is not implemented. See
[the wire protocol](wire_protocol.md) and [Windows](windows.md).
