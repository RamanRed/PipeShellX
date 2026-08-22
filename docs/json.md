# `--json` Output Schema

`pipeshellx run --json` emits **JSON Lines** (one JSON object per line, RFC
7464-style) so the output can be consumed incrementally with `jq`, a log
pipeline, or any streaming JSON reader. There are two object shapes.

## Stage object

One per host, emitted when that stage finishes:

```json
{"stage":"deploy@web001","exit":0,"timed_out":false,"error":"","dropped":0,"stdout":"…","stderr":"…"}
```

| Field | Type | Meaning |
|---|---|---|
| `stage` | string | the host/client id (`user@host`) |
| `exit` | number | process exit code; `-1` if the command was terminated by a signal; `127` if it could not be started |
| `timed_out` | bool | the stage exceeded `--timeout` and was killed |
| `error` | string | the normalized failure class when there is one (`ERROR: connection failed`, `ERROR: authentication failed`, `ERROR: host key verification failed`, `ERROR: command timed out`, …), else `""` |
| `dropped` | number | bytes discarded by a `drop-*` buffer policy (0 under the default lossless policy) |
| `stdout` | string | the stage's full standard output |
| `stderr` | string | the stage's full standard error |

Strings are escaped per RFC 8259: `"` `\` and the C0 control characters
become `\"` `\\` `\n` `\r` `\t` `\b` `\f` or `\u00XX`.

## Summary object

One per run, emitted last:

```json
{"summary":true,"stages":500,"succeeded":497,"failed":3,"dropped":0,"cancelled":false}
```

| Field | Type | Meaning |
|---|---|---|
| `summary` | bool | always `true` — distinguishes the summary from a stage object |
| `stages` | number | total stages run |
| `succeeded` | number | stages that exited 0 with no error and no timeout |
| `failed` | number | `stages − succeeded` |
| `dropped` | number | total bytes dropped across all stages |
| `cancelled` | bool | the run was interrupted (Ctrl-C) before completing |

## Consuming

```bash
# every failing host
pipeshellx run -g web --json -- systemctl is-active nginx \
  | jq -r 'select(.summary|not) | select(.exit != 0) | .stage'

# the run's success count
pipeshellx run -g web --json -- uptime | jq 'select(.summary) | .succeeded'
```

A stage object is complete on its own line, so a reader can act on each host
as it finishes rather than waiting for the whole run.
