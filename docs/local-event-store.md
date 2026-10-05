# Child device local event storage

Local event persistence is temporarily disabled until server synchronization is
implemented. Encounters, SOS events, and completed trades are not written to
NVS or queued for later synchronization.

On first boot with this firmware, the `events` NVS namespace removes its old
event slots (`e00` through `e31`) and boot ID. Device identity, seal inventory,
and trade receipts are stored in separate namespaces and are not cleared.
Gateway encounter rewards that previously depended on pending local events are
also paused while event persistence is disabled.

## Seal inventory

The separate `SealInventory` module stores up to 32 distinct seal IDs and each
ID's owned and trade-pool counts in one versioned NVS snapshot. Adding seals,
changing the trade-pool count, and applying a confirmed one-for-one exchange
are exposed as task-context APIs.

`exchangeOwnedSeals()` atomically decrements one offered copy and its trade-pool
count, then adds one received copy. It fails without changing the stored state
if no offered copy is in the trade pool or if the inventory cannot fit the
received seal. `applyTradeOnce()` additionally records a bounded transaction-ID
receipt with the inventory update so a retried network commit cannot apply the
same exchange twice.

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
