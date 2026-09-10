# IPv6 family activation contract

**IPv4-only remains the default.** IPv6 P2P activation belongs to a future
functional **piece-4 PR**, not this document. That PR must deliver persisted
family intent, startup application, restart semantics, UI, and EC replication
together. This document defines the contract; it adds no runtime behavior.

## Decisions

| Area | Contract |
| --- | --- |
| Default | Preserve IPv4-only operation, including configurations without a saved family intent. |
| Persistence | Piece 4 adds P2P family intent to `CPreferences`; no setting is introduced here. |
| Activation | Piece 4 applies that intent in `CamuleApp::ReinitializeNetwork()` before any P2P sockets open. |
| Changes | Saving a different intent requires a restart of the core application; it does not switch live sockets. |
| Controls | Matching UI and EC replication ship with the functional change, not as disconnected controls or tags. |
| Legacy resolution | `amuleIPV4Address::Hostname()` remains deterministic IPv4-first when returning one legacy address. This is not the P2P advertised-address fallback policy. |
| Scope | No per-family preferences or RFC 8305 / Happy Eyeballs yet. IPv4-only protocol boundaries remain intact. |

## Lifecycle

### Current behavior

[`amuleIPV4Address::Hostname()`](../src/LibSocketAsio.cpp) parses IPv4 literals
and explicitly requests IPv4 DNS results. It fails when no IPv4 address is
available rather than storing IPv6 in a legacy IPv4 endpoint. “Deterministic
IPv4-first” describes family selection, not a fixed ordering among multiple A
records; it does not promise an IPv6 fallback in this API.

[`CamuleApp::ReinitializeNetwork()`](../src/amule.cpp) is the network setup
entry point. The persisted family intent and activation lifecycle below are
future requirements, not claims about existing implementation.

### Intended piece-4 behavior

1. Load P2P family intent through
   [`CPreferences`](../src/Preferences.h), with persistence in
   [`Preferences.cpp`](../src/Preferences.cpp). Missing intent preserves IPv4-only.
2. Apply the loaded intent in `CamuleApp::ReinitializeNetwork()` **before P2P
   sockets open**, so socket creation uses the selected policy from the start.
3. Expose and replicate that same intent through the UI and EC. A saved change
   must be presented as requiring restart, not as already active.
4. Restart the core application to activate a changed intent. Restarting only
   the remote GUI does not activate it in the daemon.

## Compatibility boundaries

- Keep Kad IPv4-only until later work explicitly widens its addressing and
  protocol support.
- Preserve eD2k 32-bit address fields and every other boundary that cannot
  represent IPv6. Do not truncate, reinterpret, or silently insert IPv6 there.
- Keep single-address legacy hostname resolution separate from selection or
  fallback among P2P advertised addresses. This contract does not define that
  fallback algorithm.
- Extend UI and EC together with persistence and activation. Document the new
  replication and older-peer compatibility behavior in the existing
  [EC protocol reference](EC_Protocol.md) when implemented; this document
  allocates no tags and claims no existing IPv6 EC support.

## Non-goals

- Implementing activation, preference keys, enum values, UI controls, or EC tags
  in this documentation change.
- Adding independent per-family preferences, live family switching, or
  RFC 8305 / Happy Eyeballs connection racing.
- Widening Kad, eD2k 32-bit fields, or other IPv4-only interfaces implicitly.
- Treating DNS result order or legacy `Hostname()` behavior as the P2P
  advertised-address fallback policy.

## Acceptance checklist for piece 4

These are future implementation gates, not completed validation results.

- [ ] New and existing configurations without saved intent remain IPv4-only.
- [ ] P2P family intent persists through `CPreferences` and a core restart.
- [ ] `ReinitializeNetwork()` applies intent before any P2P socket opens.
- [ ] Saving intent leaves the running family policy unchanged until restart.
- [ ] UI and EC replicate the same intent and communicate restart requirements;
      older-peer behavior is defined and tested.
- [ ] Legacy hostname resolution retains IPv4-first family selection, including
      mixed A/AAAA results and failure when no IPv4 result exists.
- [ ] Kad, eD2k 32-bit fields, and other IPv4-only boundaries retain their contracts.
- [ ] Monolithic, daemon, and remote GUI builds and relevant tests are validated.

## Next implementation step

Deliver one functional piece-4 PR coupling `CPreferences` persistence,
pre-socket application in `CamuleApp::ReinitializeNetwork()`, restart semantics,
and matching UI + EC replication. Include tests for the checklist above and
update this document to distinguish implemented behavior from remaining work.
