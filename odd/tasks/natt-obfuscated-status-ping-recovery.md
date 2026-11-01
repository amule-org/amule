# Obfuscated status-ping recovery (#1177)

Status: active. Selected by the user from got3nks's NAT-T priorities on 2026-11-01.
Scope: recover a lost obfuscated server status ping without changing unrelated NAT-T routes.

## Specs

S1. "recovery from a lost obfuscated status ping, which blocks the Lugdunum callback and the extension for hours"

## Tasks

T1 | S1 | inline investigation and design | complete: dual-key receive fallback identified
T2 | S1 | inline implementation, tests, docs | complete: focused LugdunumNatTraversalTest passed
T3 | S1 | inline verification | complete: NAT-T/uTP 209/209; default 206/206; commit: this work unit

## Log

L1. "elige uno"
L2. got3nks: "Please park it for now. The upload-side callback saves a queued LowID part of one re-ask interval, and no bandwidth. I think these items help NAT-T users more, so please take one of them first:"
L3. got3nks: "recovery from a lost obfuscated status ping, which blocks the Lugdunum callback and the extension for hours;"
L4. Evidence: while an obfuscated status ping is pending, `TryProcessServerWayNatCallback()` selects its challenge over the established server UDP key. A Lugdunum callback or extension wake encrypted with the established key cannot then decrypt. Try the pending challenge first and the established key second, without changing status-ping scheduling.
L5. The focused `LugdunumNatTraversalTest` passed in the aarch64 container, configured with `ENABLE_NATT_SERVER_COORDINATION=YES`, `ENABLE_UTP=YES`, and `ENABLE_MEDIA_PROBE=OFF` because its development image lacks MediaInfo.
L6. Full aarch64 containers passed with the three applications: NAT-T/uTP 209/209 and default flags-off 206/206. Each skipped the same 14 GUI/locale tests the headless container cannot run. Host LSP findings are the known missing wx/muleunit false positives; container compiles are the evidence.
