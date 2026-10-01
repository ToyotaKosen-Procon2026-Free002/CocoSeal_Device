# Child device local event store

The child stores events that have not yet been synchronized in ESP32 NVS.
The store currently records:

- Encounter events received over ESP-NOW, including peer ID, peer type, and
  received sticker ID.
- SOS events sent by this child and SOS events received from another child.

Each record has a UUID event ID, a schema version, a boot UUID, and the
`millis()` uptime when the event was observed. Uptime is not a wall-clock
timestamp; events cannot be assigned a real date until a trusted clock source
is added.

At most 32 events are retained. The store never overwrites a pending event:
when all slots are occupied, new events remain queued in RAM and storage
failures are reported over Serial. The ESP-NOW callback only queues events;
NVS writes run from the main loop.

`getPendingLocalEventCount()` and `getPendingLocalEvent()` expose records for a
future synchronizer. Call `markLocalEventSynced()` only after the server has
confirmed that a particular event was accepted. The record is then removed
from the pending store.

The store does not yet manage the child's seal inventory or trade pool. Current
firmware sends a hard-coded sticker ID and does not implement a completed
two-device trade, so inventory changes are not inferred from encounters.

## Seal inventory

The separate `SealInventory` module stores up to 32 distinct seal IDs and each
ID's owned and trade-pool counts in one versioned NVS snapshot. Adding seals,
changing the trade-pool count, and applying a confirmed one-for-one exchange
are exposed as task-context APIs.

`exchangeOwnedSeals()` atomically decrements one offered copy and its trade-pool
count, then adds one received copy. It fails without changing the stored state
if no offered copy is in the trade pool or if the inventory cannot fit the
received seal. Encounter packets do not call this API: exchange completion and
the initial inventory bootstrap still need to be defined.

## Gateway sticker awards

Gateway encounter events remain pending for rewards until SNTP confirms the
current Japan-local date. The firmware then adds the received sticker at most
once per gateway per calendar day. The owned seal addition and the gateway/day
deduplication record are persisted together in the inventory snapshot. Encounter
events received while offline are considered when trusted time becomes
available; since the wire packet has no event timestamp, such deferred awards
use the day on which time is synchronized.

Daily deduplication retains up to ten gateway IDs. A new day can reuse the
oldest prior-day slot; if all ten slots have already been used on the current
day, another gateway's reward is deferred rather than risking duplicate awards.
