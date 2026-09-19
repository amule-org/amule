# IPv6 upload queue address migration

## Goal
Migrate upload queue peer lookups from legacy IPv4 integers to canonical `CNetworkAddress` values without enabling IPv6 transport.

## Scope
- Change per-peer upload-cap lookup to use canonical user addresses.
- Change UDP waiting-client lookup to accept canonical addresses.
- Preserve existing IPv4 wire formats and LowID/absent behavior.
- Add focused regression tests for native IPv6, mapped IPv4, and absent addresses.

## Non-goals
- Do not enable IPv6 listeners or UDP handlers.
- Do not change GUI_ID/chat/EC/API identity formats.
- Do not change uTP or NAT-T behavior.

## Tasks
- [x] Inspect and adapt UploadQueue APIs and callers.
- [x] Add regression coverage.
- [x] Run CI-equivalent build, tests, format, and diff checks.
- [x] Review diff and report push readiness; do not push without explicit follow-up.
