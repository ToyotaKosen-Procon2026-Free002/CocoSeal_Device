# Child device local event storage

Encounter, SOS, and completed-trade events are queued in RAM and persisted to
the `events` NVS namespace. The queue holds up to 32 events. Encounter and
completed-trade events are uploaded to the server one at a time after Wi-Fi and
trusted network time are available. The device removes an event only after the
server confirms success; failures leave it queued for retry. Gateway sticker
awards are applied locally before the matching encounter can be removed.

Events receive a stable UUID and record both uptime and a boot UUID. If network
time was unavailable when an event was created, its timestamp can be reconstructed
after NTP synchronization only while the device has not rebooted. At startup,
events from a previous boot without a trusted timestamp are discarded because
their original time cannot be recovered and the server requires a signed event
timestamp. Events with a timestamp and events from the current boot are retained.
Event schema version 4 stores the partner's advertised display name and the
full 36-character gateway sticker UUID. Version 3 events are migrated at
startup; older or corrupt records are removed because their stored format is
incompatible.

SOS events remain local for now. The server SOS endpoint requires a gateway ID
and receive timestamp that are not available in the current child-device event.
The current API contract needed for encounter and trade synchronization is
documented in `server-sync-api-contract.md`.

## Seal inventory

The separate `SealInventory` module stores up to 32 distinct seal IDs and each
ID's owned and trade-pool counts in one versioned NVS snapshot. Adding seals,
changing the trade-pool count, and applying a confirmed one-for-one exchange
are exposed as task-context APIs.

The seal inventory uses a dedicated `nvs_seals` partition so a full pending
event queue cannot prevent server trade-pool updates. On the first boot with
this partition layout, an existing inventory snapshot in the default NVS
partition is copied and verified before its old copy is removed. The default
NVS partition remains in place for pending events and other preferences.

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
deduplication record are persisted together in the inventory snapshot.

This reward is disabled in contest builds (`CONTEST_MODE=1`), and synchronized
gateway encounters do not request a seal reward from the server in that mode.

Daily deduplication retains up to ten gateway IDs. A new day can reuse the
oldest prior-day slot; if all ten slots have already been used on the current
day, another gateway's reward is deferred rather than risking duplicate awards.
