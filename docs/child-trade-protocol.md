# Child-to-child seal exchange

Child encounters initiate a one-for-one exchange automatically when both
devices have at least one seal in their local trade pool. To avoid both peers
starting at once, the child with the lexicographically lower UUID initiates.
Each exchange selects the first available seal ID, and the responder selects a
different seal ID from its own trade pool.

The protocol uses targeted ESP-NOW unicast on the current radio channel
(`peer.channel = 0`). When connected to Wi-Fi, ESP-NOW therefore follows the
Wi-Fi access point's channel. Devices exchanging messages must be on the same
2.4 GHz channel; if they connect to access points on different channels, they
cannot exchange ESP-NOW packets:

The PlatformIO firmware environments enable contest mode. In this mode, the
device connects to its configured Wi-Fi before initializing ESP-NOW, allowing
both interfaces to share the access point's channel. All devices participating
in the contest demo must connect to the same access point/channel. If Wi-Fi is
unavailable at boot, ESP-NOW starts on offline fallback channel 1; after Wi-Fi
reconnects, ESP-NOW follows the radio's current channel. Set `CONTEST_MODE=0`
in the build flags to disable Wi-Fi and keep ESP-NOW on channel 1.

Contest mode disables the once-per-day seal reward for gateway encounters. The
encounter can still be recorded and synchronized, but it does not add a seal
to the child's local inventory or request a gateway seal reward from the server.

1. `OFFER`: initiator proposes one trade-pool seal.
2. `ACCEPT`: responder selects its trade-pool seal.
3. `COMMIT`: initiator confirms the exact pair.
4. `ACK`: responder confirms its inventory update.

Each message contains both device UUIDs, a transaction UUID, the seal IDs, and
a CRC32. Messages are retried every 1.5 seconds. The initiator updates its
inventory only after `ACK`; the responder updates only after `COMMIT`. A
pre-commit transaction expires after 60 seconds. Once a commit has been sent,
the initiator retains and retries it until acknowledged or explicitly rejected
so the peers do not silently diverge.

Pending protocol state survives restart. Inventory updates include a bounded
transaction-ID receipt list in the same NVS snapshot, making retransmitted
commits idempotent. The receipt list retains the latest four completed trades.
Completed exchanges are also queued as local events with both seal IDs for
future synchronization.

The legacy 64-byte parent packet format is unchanged. New trade messages are
sent only to the encountered child's MAC address; a parent receiving an
unrecognized trade packet length should ignore it.

This first implementation is not cryptographically authenticated or encrypted.
CRC32 detects accidental corruption but does not prevent a nearby device from
forging messages. The exchange also requires each child to already have a
trade-pool entry. A dedicated PlatformIO test environment seeds four dummy
seals into an empty inventory; the normal firmware environment does not.
