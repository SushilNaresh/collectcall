# Collect Call Service — Operator B2BUA (PJSUA C API)

## Why PJSUA C (not PJSUA2 C++)?

| Aspect | PJSUA2 C++ | **PJSUA C** |
|---|---|---|
| Call handle | `pj::Call` subclass | `pjsua_call_id` integer |
| Callbacks | Virtual methods | Single global `pjsua_callback` struct |
| SIP header access | Limited (`CallInfo`) | Direct `pjsip_msg_find_hdr()` |
| SDP manipulation | `SdpSession` object | `pjmedia_sdp_session *` direct |
| Memory | C++ RAII | `pj_pool_t *` explicit pools |
| UPDATE SDP | Auto-generated | Fully hand-constructed |
| Overhead | Higher | Lower — better for operator scale |

## Call Flow

```
UE-A                  B2BUA (PJSUA C)               UE-B
 │                        │                            │
 │──INVITE (prefix+B)────►│                            │
 │◄─183 + SDP─────────────│  ← early media, WAV loops  │
 │◄═══"please wait"═══════│                            │
 │                        │──INVITE (B number)────────►│
 │                        │◄─180 Ringing───────────────│
 │                        │◄─200 OK + SDP──────────────│
 │                        │──ACK──────────────────────►│
 │                        │◄══"collect call — 1 or 2"══│
 │                        │◄─DTMF=1────────────────────│
 │◄─200 OK────────────────│  ← CDR billing starts here │
 │──ACK───────────────────►│                            │
 │        [B2BUA bridges A↔B in conf bridge]           │
 │◄──SIP UPDATE (new SDP)──│──SIP UPDATE (new SDP)────►│
 │──200 OK────────────────►│◄─200 OK────────────────────│
 │◄═══════════RTP direct A↔B════════════════════════════►│
 │                B2BUA exits RTP path                  │
```

## Project Structure

```
collect_call_pjsua_c/
├── CMakeLists.txt
├── Makefile
├── include/
│   ├── config.h      — all operator parameters
│   ├── session.h     — cc_session_t shared state
│   ├── handlers.h    — leg-A and leg-B function declarations
│   ├── b2bua.h       — global PJSUA callbacks + origination
│   └── utils.h       — SDP, SIP header, URI, media helpers
├── src/
│   ├── main.c        — PJSUA init, transports, account, event loop
│   ├── b2bua.c       — cc_on_incoming_call, cc_on_call_state, etc.
│   ├── leg_a.c       — inbound leg: 183 hold, WAV, answer200, UPDATE
│   ├── leg_b.c       — outbound leg: prompt, DTMF FSM, timers, UPDATE
│   ├── session.c     — session create/destroy (pj_pool based)
│   └── utils.c       — SDP extract/rewrite, header capture, media bridge
├── systemd/
│   └── collect_call.service
└── wav/
    └── README.txt
```

## Configuration

Edit `include/config.h`:

| Macro | Default | Description |
|---|---|---|
| `CC_COLLECT_PREFIX` | `"1800"` | Compile-time fallback prefix stripped to get B's number |
| `CC_SIP_DOMAIN` | `"ims.operator.net"` | SIP domain for B's URI |
| `CC_LOCAL_HOST` | `"10.0.0.1"` | B2BUA's routable IP |
| `CC_LOCAL_SIP_PORT` | `5060` | SIP listen port (UDP + TCP) |
| `CC_B_RING_TIMEOUT_SEC` | `60` | Max ring time before unavailable |
| `CC_B_DTMF_TIMEOUT_SEC` | `30` | Max wait for B's key press |
| `CC_MGW_SUBNETS` | `10.200.*, 10.201.*` | MGW RTP pool subnets |
| `CC_MAX_CALLS` | `200` | Max concurrent calls |
| `CC_LOG_LEVEL` | `4` | PJSUA log verbosity (0–6) |

### Production Runtime Environment

`include/config.h` still provides safe fallback defaults, but production
deployment values can now be supplied through environment variables without
rebuilding the app.

Example production configuration:

```bash
export CC_LOCAL_HOST=10.185.49.39
export CC_LOCAL_SIP_PORT=5060
export CC_SBC_HOST=<MTN_SBC_IP>
export CC_SBC_PORT=5060
export CC_COLLECT_PREFIXES=49013,49014,612,613
export CC_DEFAULT_COUNTRY_CODE=234
export CC_PREFIX_MODE=strip_required
export CC_SERVICE_KEY_MODE=request_uri_and_from
export CC_SERVICE_KEY_PLACEHOLDER=8024
export CC_VALIDATION_HOST=127.0.0.1
export CC_VALIDATION_PORT=9090
export CC_ENDCALL_HOST=127.0.0.1
export CC_ENDCALL_PORT=9092
export CC_PANI_VALUE='GSTN;gstn-location="03930803406806";network-provided'
export CC_MEDIA_MODE=local_bridge
```

Supported modes:

| Variable | Values |
|---|---|
| `CC_PREFIX_MODE` | `strip_required`, `allow_already_stripped` |
| `CC_SERVICE_KEY_MODE` | `disabled`, `from_only`, `request_uri`, `request_uri_and_from` |
| `CC_SERVICE_KEY_PLACEHOLDER` | Temporary fallback serviceKey for ELIGIBLE responses that omit `serviceKey`; default `8024` |
| `CC_MEDIA_MODE` | `local_bridge`, `update`, `reinvite` |

Production routing notes:

- Local SIPp/PJSUA values are lab simulation only.
- Production caller and receiver are real MSISDNs.
- The app receives A-leg INVITEs from the SBC and sends the B-leg INVITE back
  to the configured `CC_SBC_HOST:CC_SBC_PORT`.
- Incoming dialed-number extraction uses raw Request-URI first, then To header,
  then PJSUA local info as a fallback.
- `CC_COLLECT_PREFIXES` is comma-separated and matched by longest prefix first.
  If `CC_PREFIX_MODE=allow_already_stripped`, an already-stripped dialed number
  is accepted as the sponsor MSISDN and logged.

## Build

### Prerequisites

```bash
# Debian/Ubuntu
sudo apt-get install build-essential libasound2-dev libssl-dev cmake

# Build pjproject (PJSUA C API — no SWIG/Python needed)
git clone https://github.com/pjsip/pjproject.git
cd pjproject
./configure --enable-shared
make dep && make -j$(nproc)
sudo make install
sudo ldconfig
```

### GNU Make

```bash
make
```

### CMake

```bash
mkdir build && cd build
cmake .. -DPJSIP_PREFIX=/usr/local
make -j$(nproc)
```

### Install

```bash
sudo make install
# or: cmake --install build
```

## Running

```bash
# Direct
/opt/collect_call/collect_call

# systemd
sudo cp systemd/collect_call.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now collect_call
sudo journalctl -u collect_call -f
```

## Key Implementation Details

### Session / Call user_data

Each `pjsua_call_id` has a `user_data` void pointer set via
`pjsua_call_set_user_data()`.  Both legs point to the same
`cc_session_t *`.  The global callbacks use this to dispatch to
the correct leg handler:

```c
cc_session_t *s = pjsua_call_get_user_data(call_id);
if (call_id == s->call_a) leg_a_on_call_state(...);
else                       leg_b_on_call_state(...);
```

### SDP Direct Rewrite (BYPASS_DIRECT)

`cc_sdp_rewrite_rtp()` clones the active local SDP using
`pjmedia_sdp_session_clone()` then rewrites the `c=` address
and `m=audio` port to the peer's RTP endpoint before passing
it to `pjsua_call_update()`.

### MGW Bypass (BYPASS_MGW)

For CS/MGCF paths, the UPDATE carries `X-MGW-Directive: release-anchor`
appended via `pjsip_generic_string_hdr_create()` + `pj_list_push_back()`.
No SDP rewrite is performed; the IMS-MGW interprets the header.

### Operator Header Capture

`cc_capture_fwd_headers()` uses `pjsip_msg_find_hdr_by_name()` on the
raw `pjsip_rx_data->msg_info.msg` to read P-Asserted-Identity,
P-Charging-Vector, etc. from A's INVITE and stores them in the session.
They are re-injected into B's INVITE via `pjsip_generic_string_hdr_create()`.

### DTMF

Both `on_dtmf_digit` and `on_dtmf_digit2` are registered. The extended
`on_dtmf_digit2` callback is the primary PJSUA callback and receives both
RFC2833/RFC4733 RTP telephone-event and SIP INFO DTMF with method metadata.
The legacy callback remains wired for older compatibility. Only B-leg digits
enter the sponsor decision state machine; A-leg DTMF is logged and ignored.

Method values are:

```text
0 = RFC2833
1 = SIP INFO
```

Digit `1` accepts, digit `2` rejects, and other digits are ignored. The
decision is claimed once under the session mutex, so duplicate telephone-event
indications or retransmitted INFO requests cannot bridge or reject twice.

The compile-time prefix remains in `include/config.h`. Set an environment
override before starting the app for customer short-code testing:

```bash
export CC_COLLECT_PREFIX=49013
./collect_call
```

`49013` and `49014` are the customer short codes. With prefix `49013`, a
called user of `490132349162059818` is stripped to sponsor MSISDN
`2349162059818`.

### Memory

All session state is allocated from a `pj_pool_t` created per session.
`cc_session_destroy()` calls `pj_pool_release()` which frees the pool
and everything allocated from it, including the session struct itself.
