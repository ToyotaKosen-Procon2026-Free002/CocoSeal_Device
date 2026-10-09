# Child-to-child seal exchange

Child encounters are displayed by both devices after a short synchronized
ESP-NOW notification. To avoid both peers starting an exchange at once, the
child with the lexicographically lower UUID initiates. Each exchange selects
the first available seal ID, and the responder selects a different seal ID
from its own trade pool.

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
5. `TRADE_RESULT`: initiator reports the final result so both devices show the
   same success or failure message. A peer with no eligible seal also sends a
   failure result without starting a trade.

Protocol packets contain both device UUIDs, a transaction UUID, and a CRC32;
the seal IDs are included in exchange messages that need them. Pending
exchange messages are retried every 1.5 seconds. The initiator updates its
inventory only after `ACK`; the responder updates only after `COMMIT`. An
exchange expires after 60 seconds. The separate
`ENCOUNTER_DISPLAY` and `TRADE_RESULT` packets coordinate the encounter screen
and communicate the final exchange outcome.

Pending protocol state survives restart. Inventory updates include a bounded
transaction-ID receipt list in the same NVS snapshot, making retransmitted
commits idempotent. The receipt list retains the latest four completed trades.
Completed exchanges are also queued as local events with both seal IDs for
future synchronization.

Intermediate exchange protocol states are not shown on the OLED. Both children
show `シールこうかん！` after success, or `こうかんに / しっぱいしました`
when no eligible seal is available or the exchange fails.

Gateway communication packets use the parent's 84-byte layout, including a
37-byte sticker/name field. Signed SOS packets use the matching 212-byte
layout. New trade messages are sent only to the encountered child's MAC
address; a parent receiving an unrecognized trade packet length should ignore
it.

This first implementation is not cryptographically authenticated or encrypted.
CRC32 detects accidental corruption but does not prevent a nearby device from
forging messages. An exchange requires both children to have different
trade-pool seals. A dedicated PlatformIO test environment seeds four dummy
seals into an empty inventory; the normal firmware environment does not.
