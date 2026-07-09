# ChangeLog

## 2026-07-08 — write-path back-pressure: buffer ceiling, stall detection, split keepalive

Reworks how a slow or wedged TCP consumer is bounded on the write side, and
splits TCP keepalive out from the write timeout so the two can be tuned
independently. Adds two new tunables (`max_write_buffer`, `blocked_stall_window`),
promotes `tcp_keepalive` to a first-class parameter, and makes the existing
`blocked_read_rate` / write-timeout knobs behave more predictably.

Touches `raims` (config plumbing in `src/session.cpp`, constants in
`include/raims/config_const.h`) and `raikv` (enforcement in `ev_net`, socket
options in `ev_tcp`).

Commits: `raims` e7239bf (`add max_buffer, stall_window, keepalive vars`),
`raikv` febf6fa (`add max_buffer, stall_window, remove linger`),
building on `raims` 84d3d4d / `raikv` 6746845 (original `blocked_read_rate`)
and `raikv` 9e04ecd (`fix blocked_read_rate`).

### Background

A single slow or frozen consumer must not be allowed to consume unbounded
memory or to hold-of-line (HOL) block every other leg it is fanned out to.
Three independent limits now govern a backed-up leg, each with a distinct job:

1. **`blocked_read_rate`** — *rate*: how fast a back-pressured leg is allowed to
   keep releasing parked readers while its own write buffer is draining.
2. **`max_write_buffer`** — *memory ceiling*: an absolute per-leg cap on
   send-buffer growth, independent of fanout or of whether anyone is parked
   behind this leg.
3. **`blocked_stall_window`** — *liveness*: how long a leg may make zero send
   progress (while backed up past `tcp_write_highwater`) before it is declared
   wedged and dropped — but only when it is actually HOL-blocking parked
   readers.

Kernel-side, `tcp_write_timeout` now also arms `TCP_USER_TIMEOUT`
(Linux) / `TCP_MAXRT` (Windows) so the OS reaps an *active* wedged-write peer
that `SO_KEEPALIVE` (idle-only) can never see.

---

### New parameters

#### `max_write_buffer` (bytes, default 256 MB)

Hard per-leg ceiling on outstanding write-buffer bytes (`wr_pending`). When a
leg's buffered send data exceeds this cap, the leg is closed with
`EV_ERR_MAX_BUFFER` ("exceeded write buffer space"). Enforced in two places:

- from `write()` on `EAGAIN`, and
- from the `check_write_poll_timeout` sweep.

This is a fanout-independent memory guard: it fires regardless of whether any
readers are parked in `bp_wait` behind the socket. Replaces the old, disabled
(`#if 0`) budget clamp inside `bp_rate_ready()`. Reloadable.

Internally the `EvConnection` field was renamed `max_buffer` →
`max_write_buffer` to match the parameter name, and it now initializes from
`poll.max_write_buffer` rather than the compile-time default.

#### `blocked_stall_window` (time, default 3 secs)

Wedged-leg detector. If a leg is backed up past `tcp_write_highwater` and makes
**zero** successful send progress within this window, it is closed with
`EV_ERR_STALL` ("exceeded write stall time"). Progress is tracked via
`active_ns`, which is bumped only by real sends — reads cannot mask a write
stall. This is a liveness trigger and only applies when the leg is HOL-blocking
parked readers, so `check_write_poll_timeout` gates it on a non-empty `bp_wait`;
a healthy consumer draining one large message keeps bumping `active_ns` and
never trips. Reloadable.

#### `tcp_keepalive` (time, default 30 secs)

Now a first-class, independently configurable parameter (`so_keepalive_ns`).
**Previously keepalive was silently aliased to `tcp_write_timeout`** — setting
the write timeout also moved the keepalive interval. That coupling is removed:
the two are now set and reloaded separately. Default raised 10s → 30s.

`SO_KEEPALIVE` only probes *idle* connections; it does nothing for a connection
with data queued unacked in the send buffer. See `tcp_write_timeout` below for
how active wedged writes are now reaped.

---

### Changed behavior

#### `tcp_write_timeout` (time, default raised 15s → 20s)

- Now also arms the kernel's unacked-data timeout so an *active* wedged-write
  peer is dropped by the OS, not left to the ~15-minute `tcp_retries2`
  retransmit horizon:
  - **Linux:** `TCP_USER_TIMEOUT` = `tcp_write_timeout` (ms).
  - **Windows:** `TCP_MAXRT` = `tcp_write_timeout` (secs, rounded up).
- The app-level write-poll timeout is now `tcp_write_timeout * 5/4` (a small
  margin over the kernel timeout so attribution stays clean).
- On an `EPOLLERR` close, `SO_ERROR` is now captured so the disconnect is
  attributable: an `ETIMEDOUT` from a `TCP_USER_TIMEOUT` abort is recorded as
  `EV_ERR_WRITE_TIMEOUT`, a peer `RST`/`HUP` as `EV_ERR_WRITE_RESET`.
- No longer aliases keepalive (see `tcp_keepalive`).

#### `blocked_read_rate` (bytes, default 25 MB)

- Now **reloadable** at runtime — moved into the `reload_parameters()` set
  alongside the other write-path knobs. Previously it could only be set at
  startup.
- `bp_rate_ready()` is now strictly rate-only: the absolute memory clamp was
  removed from the budget calculation (that job belongs to `max_write_buffer`),
  and `bp_block_ns` anchors the budget from the start of a block episode so
  partial sends don't reset it. (raikv 9e04ecd groundwork.)

---

### Other

- **`idle_busy` default lowered 16 → 8** (`raims` `SessionMgr` ctor).
- **`OPT_LINGER` removed from the default TCP listen/connect/accept option
  sets** (`raikv`). `SO_LINGER` is no longer applied by default; wedged/aborted
  connections are governed by the timeouts above instead.
- New socket error codes: `EV_ERR_MAX_BUFFER` (19), `EV_ERR_STALL` (20),
  `EV_ERR_LAST` bumped to 21. Both have `err_string()` entries.
- New virtual hooks `EvSocket::bp_check_write_max_buffer()` /
  `bp_check_write_stall()` (base returns false; `EvConnection` implements the
  real checks).
- All five parameters are printed at startup and log a line on live reload when
  their value changes.

### Config summary

| parameter              | type  | default  | reloadable | on breach            |
|------------------------|-------|----------|-----------|----------------------|
| `tcp_keepalive`        | time  | 30 secs  | yes       | (idle probe only)    |
| `tcp_write_timeout`    | time  | 20 secs  | yes       | close, WRITE_TIMEOUT |
| `blocked_read_rate`    | bytes | 25 MB    | yes       | (throttles releases) |
| `max_write_buffer`     | bytes | 256 MB   | yes       | close, MAX_BUFFER    |
| `blocked_stall_window` | time  | 3 secs   | yes       | close, STALL         |
