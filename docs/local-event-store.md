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
