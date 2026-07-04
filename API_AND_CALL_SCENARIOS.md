# Collect Call — API Responses & Call Scenario Summary

---

## 1. Initiate API (Validation)

### Transport

| Property | Value |
|---|---|
| Protocol | UDP (request/response) |
| Target | `CC_VALIDATION_HOST:CC_VALIDATION_PORT` (default `127.0.0.1:9090`) |
| Timeout | `CC_VALIDATION_TIMEOUT_MS` (default `1000 ms`) |
| Trigger | After A-leg CONFIRMED (ACK received), before B-leg INVITE is sent |

### Request Payload

```json
{
  "callerMsisdn": "2349162059820",
  "sponsorMsisdn": "2349162059876",
  "callId": "isbc'RdY9301408020XZbmGhEfCjBiA0d@LGMBC5.MSS.MTNNIGERIA.NG",
  "source": "PREFIX_INITIATED",
  "timestamp": "2025-01-15T10:30:00+01:00"
}
```

| Field | Source |
|---|---|
| `callerMsisdn` | Normalized from `P-Asserted-Identity` → `From` header of A-leg INVITE |
| `sponsorMsisdn` | Normalized B-number after prefix strip + country code prepend |
| `callId` | SIP `Call-ID` of A-leg INVITE |
| `source` | Compile-time constant `CC_INITIATE_SOURCE` (`PREFIX_INITIATED`) |
| `timestamp` | Nigeria time (UTC+01:00) at moment of validation call |

---

### Response Handling

#### ELIGIBLE — Allow Call

```json
{
  "status": "ELIGIBLE",
  "serviceKey": "8024",
  "details": "CALLER_IS_WHITELISTED"
}
```

| Field | Behaviour |
|---|---|
| `serviceKey` | Prepended to B-number in Request-URI / From per `CC_SERVICE_KEY_MODE` |
| `serviceKey` absent | Placeholder `CC_SERVICE_KEY_PLACEHOLDER` (`8024`) used instead |
| `details` | Logged only, not used in routing |

→ B-leg INVITE is sent.

---

#### INELIGIBLE — Reject Call

```json
{
  "status": "INELIGIBLE",
  "statusCode": "SPONSOR_BALANCE_FAIL",
  "reasonDescription": "The person you are trying to call does not have sufficient balance."
}
```

| `statusCode` | Internal code | `final_status` | `final_reason` | SIP action on A-leg |
|---|---|---|---|---|
| `CALLER_BLACKLISTED` | `1` | `CANCELLED` | `CALLER_BLACKLISTED` | BYE (403 Forbidden) |
| `SPONSOR_BALANCE_FAIL` | `2` | `CANCELLED` | `SPONSOR_BALANCE_FAIL` | BYE (403 Forbidden) |
| `SPONSOR_DND_ACTIVE` | `4` | `CANCELLED` | `SPONSOR_DND_ACTIVE` | BYE (403 Forbidden) |
| `SPONSOR_ROAMING` | `5` | `CANCELLED` | `SPONSOR_ROAMING` | BYE (403 Forbidden) |
| anything else | `3` | `FAILED` | `API_FAILURE` | BYE (403 Forbidden) |

---

#### Timeout / Transport Error

| Condition | `errno` | `result.reason` | `final_status` | `final_reason` |
|---|---|---|---|---|
| `recvfrom` timeout | `EAGAIN` / `EWOULDBLOCK` | `ELIGIBILITY_TIMEOUT` | `FAILED` | `ELIGIBILITY_TIMEOUT` |
| socket / send error | other | `SYSTEM_ERROR` | `FAILED` | `SYSTEM_ERROR` |
| JSON encode overflow | — | `SYSTEM_ERROR` | `FAILED` | `SYSTEM_ERROR` |

---

#### Legacy Numeric Response (stub compatibility)

```json
{ "status": 0, "reason": "ALLOWED", "serviceKey": "1234" }
```

| `status` value | Behaviour |
|---|---|
| `0` | Allow — treated same as ELIGIBLE |
| non-zero | Reject — treated same as API_FAILURE |

---

## 2. End Call API

### Transport

| Property | Value |
|---|---|
| Protocol | UDP (fire-and-forget, no response) |
| Target | `CC_ENDCALL_HOST:CC_ENDCALL_PORT` (default `127.0.0.1:9092`) |
| Trigger | Once per session at teardown via `cc_send_end_call_udp()` |

### Payload

```json
{
  "callId": "isbc'RdY9301408020XZbmGhEfCjBiA0d@LGMBC5.MSS.MTNNIGERIA.NG",
  "callDuration": 42,
  "status": "COMPLETED",
  "reason": "NORMAL_CLEARING",
  "startDate": "2025-01-15T10:30:00+01:00",
  "endDate": "2025-01-15T10:30:42+01:00",
  "ICID": "4C01CF4632-0702-08143009"
}
```

| Field | Source |
|---|---|
| `callId` | SIP `Call-ID` of A-leg INVITE |
| `callDuration` | Seconds from `call_connected_ts` to `call_end_ts`; `0` if never connected |
| `status` | Mapped from internal `final_status` (see table below) |
| `reason` | Mapped from internal `final_reason` (see table below) |
| `startDate` | Nigeria time (UTC+01:00) of call arrival |
| `endDate` | Nigeria time (UTC+01:00) of call teardown |
| `ICID` | `icid-value` extracted from `P-Charging-Vector` of A-leg INVITE |

### Internal → API Status / Reason Mapping

| `final_reason` (internal) | `status` (API) | `reason` (API) |
|---|---|---|
| `NORMAL_CLEARING` | `COMPLETED` | `NORMAL_CLEARING` |
| `USER_ABANDONED` | `CANCELLED` | `USER_ABANDONED` |
| `REJECTED_BY_SPONSOR` | `CANCELLED` | `REJECTED_BY_SPONSOR` |
| `SPONSOR_UNREACHABLE_NoMCA` | `FAILED` | `SPONSOR_UNREACHABLE_NoMCA` |
| `SPONSOR_UNREACHABLE_MCA` | `FAILED` | `SPONSOR_UNREACHABLE_MCA` |
| `NO_ANSWER` | `FAILED` | `NO_ANSWER` |
| `ELIGIBILITY_TIMEOUT` | `FAILED` | `ELIGIBILITY_TIMEOUT` |
| `SYSTEM_ERROR` | `FAILED` | `SYSTEM_ERROR` |
| anything else | `FAILED` | `SYSTEM_ERROR` |

---

## 3. Complete Call Scenario Matrix

| Scenario | Validation | B-leg result | DTMF | `status` | `reason` | A-leg treatment |
|---|---|---|---|---|---|---|
| Normal accepted call | ELIGIBLE | 200 OK | `1` | `COMPLETED` | `NORMAL_CLEARING` | none (bridged) |
| B rejects with DTMF 2 | ELIGIBLE | 200 OK | `2` | `CANCELLED` | `REJECTED_BY_SPONSOR` | `rejected.wav` + BYE |
| B DTMF timeout (30 s) | ELIGIBLE | 200 OK | none | `FAILED` | `ELIGIBILITY_TIMEOUT` | `rejected.wav` + BYE |
| B ring timeout (60 s) | ELIGIBLE | no answer | — | `FAILED` | `NO_ANSWER` | `unavailable.wav` + BYE |
| B disconnects before DTMF | ELIGIBLE | 200 OK → BYE | — | `CANCELLED` | `REJECTED_BY_SPONSOR` | `rejected.wav` + BYE |
| B unreachable (3xx/4xx/5xx) | ELIGIBLE | 3xx–5xx | — | `CANCELLED` | `SPONSOR_UNREACHABLE_NoMCA` | `rejected.wav` + BYE |
| B busy (486 / 603) | ELIGIBLE | 486 / 603 | — | `CANCELLED` | `REJECTED_BY_SPONSOR` | `rejected.wav` + BYE |
| A abandons before B answers | ELIGIBLE | pending | — | `CANCELLED` | `USER_ABANDONED` | — |
| A abandons after B accepts | ELIGIBLE | 200 OK | `1` | `COMPLETED` | `NORMAL_CLEARING` | — |
| Caller blacklisted | INELIGIBLE / `CALLER_BLACKLISTED` | not sent | — | `CANCELLED` | `CALLER_BLACKLISTED` | BYE (403) |
| Sponsor balance fail | INELIGIBLE / `SPONSOR_BALANCE_FAIL` | not sent | — | `CANCELLED` | `SPONSOR_BALANCE_FAIL` | BYE (403) |
| Sponsor DND active | INELIGIBLE / `SPONSOR_DND_ACTIVE` | not sent | — | `CANCELLED` | `SPONSOR_DND_ACTIVE` | BYE (403) |
| Sponsor roaming | INELIGIBLE / `SPONSOR_ROAMING` | not sent | — | `CANCELLED` | `SPONSOR_ROAMING` | BYE (403) |
| Validation API failure | `API_FAILURE` | not sent | — | `FAILED` | `API_FAILURE` | BYE (403) |
| Validation timeout | transport error | not sent | — | `FAILED` | `ELIGIBILITY_TIMEOUT` | BYE (403) |
| B-leg origination failure | ELIGIBLE | make_call fails | — | `FAILED` | `SYSTEM_ERROR` | `unavailable.wav` + BYE |
| Prefix not matched | — | not sent | — | — | — | 404 Not Found |
| No dialed number extractable | — | not sent | — | — | — | 404 Not Found |

---

## 4. Call Flows

### 4.1 Happy Path — B Accepts (DTMF `1`)

```
UE-A                      B2BUA                         UE-B
 │                          │                             │
 │──INVITE (prefix+B)──────►│                             │
 │◄─200 OK─────────────────│                             │
 │──ACK────────────────────►│                             │
 │◄══ waiting.wav (loop) ══│                             │
 │                          │  [UDP validation → ELIGIBLE]│
 │                          │──INVITE (serviceKey+B)─────►│
 │                          │  [+P-Charging-Vector,        │
 │                          │   P-Asserted-Identity,       │
 │                          │   P-Access-Network-Info]     │
 │                          │◄─200 OK + SDP──────────────│
 │                          │──ACK──────────────────────►│
 │                          │◄══ collect_prompt.wav ═════│
 │                          │◄─DTMF 1───────────────────│
 │  [Stop waiting.wav]      │  [Stop collect_prompt.wav] │
 │  [Bridge A↔B conf]       │                             │
 │◄──UPDATE / re-INVITE────│──UPDATE / re-INVITE────────►│
 │──200 OK────────────────►│◄─200 OK────────────────────│
 │◄══════════ RTP (direct or bridged) ═════════════════►│
 │                          │                             │
 │──BYE────────────────────►│──BYE──────────────────────►│
 │                          │  [END-API: COMPLETED / NORMAL_CLEARING]
```

---

### 4.2 B Rejects (DTMF `2`)

```
UE-A                      B2BUA                         UE-B
 │──INVITE────────────────►│──INVITE────────────────────►│
 │◄─200 OK────────────────│◄─200 OK────────────────────│
 │──ACK───────────────────►│──ACK──────────────────────►│
 │◄══ waiting.wav ═════════│◄══ collect_prompt.wav ═════│
 │                          │◄─DTMF 2───────────────────│
 │                          │──BYE──────────────────────►│
 │◄══ rejected.wav (4 s) ══│                             │
 │◄─BYE (486 Decline)─────│                             │
 │                          │  [END-API: CANCELLED / REJECTED_BY_SPONSOR]
```

---

### 4.3 B Disconnects Before Accepting

```
UE-A                      B2BUA                         UE-B
 │──INVITE────────────────►│──INVITE────────────────────►│
 │◄─200 OK────────────────│◄─200 OK────────────────────│
 │──ACK───────────────────►│──ACK──────────────────────►│
 │◄══ waiting.wav ═════════│◄══ collect_prompt.wav ═════│
 │                          │◄─BYE──────────────────────│
 │◄══ rejected.wav ════════│                             │
 │◄─BYE───────────────────│                             │
 │                          │  [END-API: CANCELLED / REJECTED_BY_SPONSOR]
 │                          │  (3xx–5xx response → SPONSOR_UNREACHABLE_NoMCA)
```

---

### 4.4 B Ring Timeout (60 s)

```
UE-A                      B2BUA                         UE-B
 │──INVITE────────────────►│──INVITE────────────────────►│
 │◄─200 OK────────────────│                             │
 │──ACK───────────────────►│  [ring timer: 60 s expires] │
 │◄══ waiting.wav ═════════│──CANCEL / BYE─────────────►│
 │◄══ unavailable.wav ═════│                             │
 │◄─BYE───────────────────│                             │
 │                          │  [END-API: FAILED / NO_ANSWER]
```

---

### 4.5 B DTMF Timeout (30 s — No Key Press After Answer)

```
UE-A                      B2BUA                         UE-B
 │──INVITE────────────────►│──INVITE────────────────────►│
 │◄─200 OK────────────────│◄─200 OK────────────────────│
 │──ACK───────────────────►│──ACK──────────────────────►│
 │◄══ waiting.wav ═════════│◄══ collect_prompt.wav ═════│
 │                          │  [DTMF timer: 30 s expires]│
 │                          │──BYE──────────────────────►│
 │◄══ rejected.wav ════════│                             │
 │◄─BYE───────────────────│                             │
 │                          │  [END-API: FAILED / ELIGIBILITY_TIMEOUT]
```

---

### 4.6 Validation Rejects the Call

```
UE-A                      B2BUA
 │──INVITE────────────────►│
 │◄─200 OK────────────────│
 │──ACK───────────────────►│
 │                          │  [UDP validation → INELIGIBLE / timeout]
 │◄─BYE (403 Forbidden)───│
 │                          │  [END-API: CANCELLED or FAILED /
 │                          │   CALLER_BLACKLISTED | SPONSOR_BALANCE_FAIL |
 │                          │   SPONSOR_DND_ACTIVE | SPONSOR_ROAMING |
 │                          │   API_FAILURE | ELIGIBILITY_TIMEOUT]
```

---

### 4.7 A Abandons Before B Answers

```
UE-A                      B2BUA                         UE-B
 │──INVITE────────────────►│──INVITE────────────────────►│
 │◄─200 OK────────────────│                             │
 │──ACK───────────────────►│                             │
 │──BYE───────────────────►│──BYE / CANCEL─────────────►│
 │                          │  [END-API: CANCELLED / USER_ABANDONED]
```

---

### 4.8 A Hangs Up After B Accepts

```
UE-A                      B2BUA                         UE-B
 │  [calls bridged, RTP flowing]                         │
 │──BYE───────────────────►│──BYE──────────────────────►│
 │                          │  [END-API: COMPLETED / NORMAL_CLEARING]
```

---

### 4.9 Prefix / Number Extraction Failure

```
UE-A                      B2BUA
 │──INVITE────────────────►│
 │◄─404 Not Found─────────│  [no valid dialed number or prefix mismatch]
```

---

### 4.10 B-Leg Origination Failure

```
UE-A                      B2BUA
 │──INVITE────────────────►│
 │◄─200 OK────────────────│
 │──ACK───────────────────►│
 │                          │  [pjsua_call_make_call() fails]
 │◄══ unavailable.wav ═════│
 │◄─BYE (480)─────────────│
 │                          │  [END-API: FAILED / SYSTEM_ERROR]
```

---

## 5. ServiceKey Modes

| `CC_SERVICE_KEY_MODE` | Request-URI user | From user |
|---|---|---|
| `disabled` | `2349162059876` | `2349162059876` |
| `from_only` | `2349162059876` | `80242349162059876` |
| `request_uri` | `80242349162059876` | `2349162059876` |
| `request_uri_and_from` | `80242349162059876` | `80242349162059876` |

ServiceKey source priority:
1. `serviceKey` field in ELIGIBLE API response
2. `CC_SERVICE_KEY_PLACEHOLDER` fallback (default `8024`)
3. No prefix (if mode is `disabled`)

---

## 6. Media Modes (Post-Accept)

| `CC_MEDIA_MODE` | Behaviour after DTMF `1` |
|---|---|
| `local_bridge` | PJSUA conf bridge stays active; B2BUA remains in RTP path |
| `update` | SIP UPDATE sent to both legs; SDP rewritten with peer RTP endpoint |
| `reinvite` | re-INVITE sent to both legs; SDP rewritten with peer RTP endpoint |

---

## 7. Number Normalization

Numbers extracted from Request-URI, To, or P-Asserted-Identity are normalized via `cc_normalize_msisdn()`:

| Input | Country code (`CC_DEFAULT_COUNTRY_CODE`) | Output |
|---|---|---|
| `08031234567` | `234` | `2348031234567` |
| `2349162059876` | `234` | `2349162059876` (already has CC) |
| `9162059876` | `234` | `2349162059876` |

Rules:
- Extract digits only
- If already starts with country code → use as-is
- Otherwise strip leading `0`(s) and prepend country code

---

## 8. Operator Headers Forwarded A → B

Captured from A-leg INVITE and re-injected into B-leg INVITE:

| Header | Notes |
|---|---|
| `P-Asserted-Identity` | Caller identity; also used to extract `callerMsisdn` |
| `P-Preferred-Identity` | Forwarded as-is |
| `Privacy` | Forwarded as-is |
| `P-Access-Network-Info` | Replaced by static `CC_PANI_VALUE` when `CC_BLEG_STATIC_PANI_ENABLE=1` |
| `P-Charging-Vector` | Forwarded as-is; `icid-value` extracted for End Call API |
| `P-Charging-Function-Addresses` | Forwarded as-is |

---

## 9. Configuration Reference

| Environment Variable | `config.h` fallback | Description |
|---|---|---|
| `CC_LOCAL_HOST` | `10.20.10.119` | B2BUA listen IP |
| `CC_LOCAL_SIP_PORT` | `6060` | B2BUA SIP port |
| `CC_SBC_HOST` | parsed from `CC_SIP_DOMAIN` | SBC / next-hop IP |
| `CC_SBC_PORT` | parsed from `CC_SIP_DOMAIN` | SBC / next-hop port |
| `CC_COLLECT_PREFIXES` | `1800` | Comma-separated collect-call prefixes |
| `CC_DEFAULT_COUNTRY_CODE` | `234` | Country code prepended to local-format numbers |
| `CC_PREFIX_MODE` | `strip_required` | `strip_required` or `allow_already_stripped` |
| `CC_SERVICE_KEY_MODE` | `disabled` | `disabled`, `from_only`, `request_uri`, `request_uri_and_from` |
| `CC_SERVICE_KEY_PLACEHOLDER` | `8024` | Fallback serviceKey when API omits it |
| `CC_VALIDATION_HOST` | `127.0.0.1` | Initiate API UDP host |
| `CC_VALIDATION_PORT` | `9090` | Initiate API UDP port |
| `CC_ENDCALL_HOST` | `127.0.0.1` | End Call API UDP host |
| `CC_ENDCALL_PORT` | `9092` | End Call API UDP port |
| `CC_PANI_VALUE` | `GSTN;gstn-location="03930803406806";network-provided` | Static PANI for B-leg |
| `CC_MEDIA_MODE` | `reinvite` | `local_bridge`, `update`, `reinvite` |
| `CC_ENV_FILE` | `/etc/collect_call.env` | Path to `.env` config file loaded at startup |
