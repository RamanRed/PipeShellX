# ADR-006: TLV frame envelope for the native backplane (over CBOR)

- **Status:** Accepted
- **Date:** 2026-08-23
- **Deciders:** project maintainers
- **Related:** `PLAN.md` §3.4 (readiness vs completion), §4.4 (I/O model), Phase 4 roadmap; `docs/wire_protocol.md`; ADR-001 (completion-style runtime)

## Context

The Phase 4 native backplane (`psx/1`) multiplexes many logical streams — one
per remote stage — over a single mTLS connection, with HTTP/2-style credit-window
flow control. Every unit on the wire needs a framing envelope carrying a type, a
stream id, per-type flags, and a length-delimited payload, decodable from an
arbitrarily chunked TLS byte stream on the single-threaded reactor.

The spike had to choose the envelope encoding. The two candidates:

- **CBOR** (RFC 8949): self-describing, schema-flexible, good tooling. But it
  pulls in a codec dependency, its variable-length integers make the hot decode
  path branchy, a self-describing format is a larger fuzzing surface, and framing
  (finding message boundaries in a stream) still needs a length prefix on top.
- **TLV** (fixed binary header + length-prefixed value): trivial to frame, O(1)
  branch-free header decode, a tiny and easily-bounded fuzzing surface, no
  dependency. Less flexible for evolving *payload* schemas.

## Decision

Use a **fixed 10-byte big-endian TLV header** for the frame envelope:

```
  type      : u8      (FrameType; unknown values decode, semantics one layer up)
  flags     : u8      (per-type; kEndStream = 0x01)
  streamId  : u32 be
  length    : u32 be  (payload byte count; > maxPayload is a protocol violation)
```

followed by `length` payload bytes. The envelope is the framing layer only; the
*payload* of a structured frame (e.g. `OPEN`'s job spec) is encoded separately and
MAY use a compact structured encoding later without changing the envelope. The
decoder bounds its buffer with a configurable `maxPayload` (default 16 MiB) and
poisons itself on violation so a bad peer cannot desync the stream.

`os::Tls` uses **OpenSSL** on every platform (see ADR-007), so there is no
platform-specific serialisation concern that would favour a self-describing wire
format.

## Consequences

- Framing and header decode are branch-free and allocation-light; the fuzz target
  (`FrameDecoder::push`) has a minimal surface — essentially one bounds check.
- Payload schema evolution is deferred to per-type payload encodings, keeping the
  envelope stable.
- The envelope is implemented in `psx::transport` (`frame.hpp` / `frame_codec.hpp`),
  a pure L3 component depending only on the standard library and `psx::Result`.
