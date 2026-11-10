# Kad receiver keys

A Kad packet can carry a *receiver key*: a value the sender can only produce if it can receive at
the address it claims. It is what separates "this packet says it comes from there" from "this packet
proves it comes from there".

aMule has measured this since #2317 and #2328, and until now nothing acted on the result: a request
whose key did not prove the sender was processed exactly like one that did. That gap is issue #2315.
A network attacker able to forge a source address could publish false sources, false keyword votes
or false notes, and we would store them.

## What the preference does

`/eMule/KadEnforceReceiverKey`, off by default, governs the seven request handlers that act on a
packet's source address:

| Handler | What it does with that address |
| --- | --- |
| `KADEMLIA2_PUBLISH_SOURCE_REQ` | Stores it as the source's address |
| `KADEMLIA2_PUBLISH_KEY_REQ` | Records it as the publisher, which feeds the keyword trust value |
| `KADEMLIA2_PUBLISH_NOTES_REQ` | Records it as the publisher of the note |
| `KADEMLIA_FIREWALLED_REQ`, `KADEMLIA_FIREWALLED2_REQ` | Queues a TCP dial to it, on the port in the packet |
| `KADEMLIA_FINDBUDDY_REQ` | Queues a TCP dial to it, on the port in the packet, if we have no buddy |
| `KADEMLIA_CALLBACK_REQ` | Asks our buddy to relay a callback naming it |

**Off** changes nothing about behaviour: a request without a valid key is processed as before, and
the count of such requests is kept so the decision to turn enforcement on can be based on data.
**On** skips the handler and logs the rejection at normal level.

The count is exposed by `CKademliaUDPListener::GetInvalidReceiverKeyRequestCount()` and is readable
while the preference is off; that is the whole point of shipping the mechanism before the policy.

## Why it is off

eMule official does not send a receiver key. Rejecting requests without one by default would discard
legitimate sources, votes and buddy requests from every peer that has not been updated, which is
most of the network. The default is a compatibility decision, not an oversight.

The value acted on is the one `CClientUDPSocket` already computes — whether the key the packet
carried matches `Kademlia::CPrefs::GetUDPVerifyKey(ip)` for the sending address — forwarded unchanged
to each handler. There is deliberately no second notion of validity in the tree.

## Where it lives

- The decision is the pure function `DecideKadReceiverKeyRequest()` in
  `src/kademlia/net/KadReceiverKeyPolicy.h`, unit-tested in
  `unittests/tests/KadReceiverKeyPolicyTest.cpp` without a listener or an application.
- The count, the logging and the seven gates are in `src/kademlia/net/KademliaUDPListener.cpp`.
