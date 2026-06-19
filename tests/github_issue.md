# Multi-admin commissioning fails: ArmFailSafe(0s) sent by HA after AttestationRequest instead of CSRRequest

## Environment

- **Home Assistant**: latest (just updated before this test)
- **Installation**: VM (bridged network)
- **Device**: ESP32-S3, ESP-IDF v5.5.3, esp-matter SDK
- **Device type**: On/Off Plug-In Unit (2 endpoints) + Aggregator/Bridge
- **VendorID**: 0xFFF1 (65521), **ProductID**: 0x8000 (32768)
- **Works with**: Apple Home (same firmware, same commissioning data)

---

## Description

After a successful initial commissioning on Home Assistant (BLE → WiFi), HA attempts a second commissioning (multi-admin enhanced commissioning window, `cm=2`). This second commissioning **consistently fails**.

The device log shows that on the HA side, after `AttestationRequest successful`, HA sends `ArmFailSafe(0s)` — which immediately expires the failsafe timer and aborts the commissioning — **instead of the expected `ArmFailSafe(60s)` + `CSRRequest`** that the normal Matter commissioning flow requires.

The same firmware and factory data commission correctly with Apple Home (full multi-admin flow completes). The same setup also worked with a previous version of HA before the recent update.

---

## Exact failure sequence (from device serial log)

### Normal flow (Apple Home / previous HA — what is expected):
```
[AttestationRequest] → device responds → AttestationRequest successful
[ArmFailSafe (60s)]  ← HA refreshes timer          ✅
[CSRRequest]         ← HA proceeds with NOC         ✅
[AddTrustedRootCertificate]                          ✅
[AddNOC]                                             ✅
[CommissioningComplete]                              ✅
```

### Actual flow (latest HA — what happens):
```
[AttestationRequest] → device responds → AttestationRequest successful
[ArmFailSafe (0s)]   ← HA sends 0s TIMEOUT          ❌
→ Fail-safe timer expired
→ Commissioning failed (attempt N): 32
```

### Relevant log lines (device side):
```
I chip[ZCL]: OpCreds: AttestationRequest successful.
I chip[EM]: <<< InvokeCommandResponse (B:411)        ← device sends attestation response OK
I chip[EM]: >>> InvokeCommandRequest (B:65)          ← HA sends 65-byte command
I chip[FS]: GeneralCommissioning: Received ArmFailSafe (0s)   ← ← ← BUG
I chip[FS]: Fail-safe timer expired
E chip[SVR]: Failsafe timer expired
E chip[SVR]: Commissioning failed (attempt N): 32
```

---

## Additional context

### First commissioning (BLE, initial): SUCCEEDS
```
I chip[FP]: Added new fabric at index: 0x1
I chip[ZCL]: OpCreds: Fabric index 0x1 was committed to storage.
              VendorId 0x1384 [= HA]
I chip[SVR]: Commissioning completed successfully     ✅
```

### Multi-admin commissioning: FAILS at attestation step
After the first commissioning completes, HA opens an enhanced commissioning window (`cm=2`) and starts a new PASE session. The PASE itself succeeds, certificate chain requests (PAI, DAC) succeed, `AttestationRequest` is processed successfully by the device — but then HA aborts with `ArmFailSafe(0s)` without ever sending `CSRRequest`.

### Note on PASE retries
In at least one session, the first PASE attempt timed out (5 retransmission failures, no ACK from HA), and HA retried. The retry PASE succeeded at the protocol level, but HA then sent `ArmFailSafe(0s)` after attestation. It is unclear whether the PASE retry triggers this behavior or whether it is an independent attestation verification failure on the HA side.

### Cluster 0x0030, attribute 0x000C (TCAcceptedVersion) — NOT the cause
During the initial BLE commissioning, the device returns `err=586` (NOT_FOUND) for `GeneralCommissioning` attribute `0x000C` (`TCAcceptedVersion`). This same error is present in Apple Home commissioning (which succeeds) and in previous successful HA commissioning sessions. It is therefore **not** the root cause of this failure.

### Cluster 0x0046 (IcdManagement) — NOT the cause
HA reads cluster `0x0046` attributes (`0x0000`–`0x0007`) and receives `err=0x5c3` (unknown cluster). This is also present in all commissioning sessions (including successful ones with Apple Home) and is not the cause.

---

## Logs

See attached:
- `log_apple_casa.log` — full commissioning with Apple Home (works, reference)
- `log_home_assistant.log` — commissioning with HA (fails at multi-admin attestation step)

---

## Expected behavior

After `AttestationRequest successful`, HA should:
1. Send `ArmFailSafe(60s)` (refresh timer)
2. Send `CSRRequest`
3. Send `AddTrustedRootCertificate`
4. Send `AddNOC`
5. Send `CommissioningComplete`

## Actual behavior

After `AttestationRequest successful`, HA sends `ArmFailSafe(0s)` which aborts the commissioning immediately returning with an error on the HA side.


