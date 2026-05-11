# CLUSTER_PLAN.md

## Objective

Audit the existing ToCloud9 ↔ AzerothCore cluster integration in this branch and
plan small, independently buildable implementation phases. This plan deliberately
avoids production-code changes until a phase is approved.

Primary targets:

1. Build correctness with `USE_REAL_LIBSIDECAR` and with default/stub builds.
2. `libsidecar` generated header, stub header, and C/C++ call-site consistency.
3. Raw GUID vs DB-value GUID correctness at every ToCloud9 boundary.
4. Party/raid cluster behavior for ready checks, subgroup changes, assistant /
   main tank / main assist flags, and remote member state.
5. Instance reset and instance bind extension.
6. Guild creation.
7. Duplicate, dead, unsafe, or misplaced cluster hooks.

## Current branch/repositories

- Repository: `/workspace/azerothcore-wotlk`.
- Branch: `work`.
- Local source of truth: this repository only. The expected ToCloud9 checkout at
  `/mnt/c/Users/VicenteyMar/ToCloud9` is not present in this environment, so this
  audit did not inspect upstream/walkline service code.
- Existing working tree note: `deps/libsidecar/libsidecar.so` was already
  modified before this audit and was not changed by this audit.

## Evidence map

### Build and `libsidecar`

- `deps/libsidecar/CMakeLists.txt` selects the static stub on Windows or when
  `USE_REAL_LIBSIDECAR` is off, and imports `deps/libsidecar/libsidecar.so` plus
  `deps/libsidecar/include` when `USE_REAL_LIBSIDECAR` is on.
- `src/server/apps/CMakeLists.txt` and `src/server/scripts/CMakeLists.txt` link
  against `libsidecar`.
- `src/server/game/TC9Sidecar/TC9Sidecar.cpp` calls `TC9InitLib`, registers all
  group/guild/map/monitoring hooks, registers gRPC handlers, and calls
  `TC9UpdateGroupMemberState` with the real-header 11-argument signature.
- `deps/libsidecar/include/libsidecar.h` declares the real `TC9UpdateGroupMemberState`
  signature as:
  `memberGuid, online, level, class, zoneId, mapId, health, maxHealth, powerType,
  power, maxPower`.
- `deps/libsidecar/stub/libsidecar.h` and `deps/libsidecar/stub/libsidecar.c`
  declare/define an older 8-argument `TC9UpdateGroupMemberState` signature with
  `healthPct` and `powerPct`.
- `deps/libsidecar/libsidecar.so` has an ELF header but `readelf` reports that
  section headers extend past the end of the file, and `nm -D` cannot read it.
  This strongly suggests the checked-out shared object is truncated/corrupt or
  otherwise not linkable in the current environment.
- CMake configure with `-DUSE_REAL_LIBSIDECAR=ON` starts correctly and reports
  `Use stub for libsidecar: No`, but configuration stops before linking because
  Boost development headers/libraries are missing in this environment.

### GUID model

- `ObjectGuid::GetRawValue()` returns the full packed/raw object GUID.
- `ObjectGuid::GetDBValue()` returns the raw value for player GUIDs with a realm
  id and the low counter for non-crossrealm player GUIDs; non-player values use
  the low counter.
- `ObjectGuid::CreatePlayerFromDBValue(uint64)` accepts either a raw player GUID
  or a low DB counter and normalizes it to an `ObjectGuid`.
- `ObjectGuidGenerator<HighGuid::Player>::Generate()` and
  `ObjectGuidGenerator<HighGuid::Item>::Generate()` call ToCloud9 GUID services
  in cluster mode but return `ObjectGuid::LowType`, so any 64-bit raw GUID from
  ToCloud9 is narrowed to the low counter before callers build an `ObjectGuid`.
- `CharacterHandler` creates new players from
  `sObjectMgr->GetGenerator<HighGuid::Player>().Generate()` without passing a
  realm id.
- `Item::CreateItem` passes the player realm id into item GUID generation when
  `Cluster.IsCrossrealm` is true, but the generated result is still narrowed to
  `LowType`.
- `TC9GroupHooks` and `TC9GuildHooks` consistently convert inbound ToCloud9
  player ids with `CreatePlayerFromDBValue`.
- `ToCloud9Sidecar::UpdateGroupMemberState` sends `player->GetGUID().GetRawValue()`
  to ToCloud9, not `GetDBValue()`.

### Party/raid group cluster flow

- `ToCloud9Sidecar::SetupHooks()` registers inbound hooks for group create,
  disband, member add/remove, loot, difficulty, raid conversion, ready check,
  subgroup, flags/roles, member state, instance reset, and instance bind extension.
- `TC9GroupHooks.cpp` applies inbound group events to local `Group` objects and
  local sessions.
- `Group::AddMemberWithGuid()` creates remote/local member slots, caches name,
  level, class, and placeholder live state, updates `CharacterCache`, and sends
  local group updates when requested.
- `Group::BuildClusterMemberStatsPacket()` synthesizes
  `SMSG_PARTY_MEMBER_STATS` / `SMSG_PARTY_MEMBER_STATS_FULL` from either a real
  connected `Player` or cached `MemberSlot` cluster state.
- `Group::SendClusterReadyCheckStarted`,
  `Group::SendClusterReadyCheckMemberState`, and
  `Group::SendClusterReadyCheckFinished` only send client packets for inbound
  ToCloud9 events.
- `WorldSession::HandleRaidReadyCheckOpcode`,
  `WorldSession::HandleRaidReadyCheckFinishedOpcode`,
  `WorldSession::HandleGroupChangeSubGroupOpcode`,
  `WorldSession::HandleGroupSwapSubGroupOpcode`,
  `WorldSession::HandleGroupAssistantLeaderOpcode`, and
  `WorldSession::HandlePartyAssignmentOpcode` currently perform local group
  operations only; no outbound ToCloud9 ready-check/subgroup/flag APIs are called
  from these handlers.
- `Group::SetGroupMemberFlag` and `Group::RemoveUniqueGroupMemberFlag` update
  local state only. Persistence is disabled when cluster mode is enabled, but no
  replacement sidecar call is visible in this repo.

### Remote member state flow

- Inbound `GroupMemberStateChanged` carries raw health/max-health/power fields in
  both real and stub `events-group.h`.
- `TC9GroupHooks::OnGroupMemberStateChanged()` applies inbound remote state to
  `Group::SetClusterMemberState()`.
- `ToCloud9Sidecar::UpdateGroupMemberState()` refreshes local `Group` member
  caches and calls `TC9UpdateGroupMemberState()`.
- Outbound member-state hooks are installed in high-frequency worldserver paths:
  `Unit::SetHealth`, `Unit::SetMaxHealth`, `Unit::SetPower`,
  `Unit::SetMaxPower`, and `Player::UpdateZone` call
  `UpdateGroupMemberState()` when cluster mode is enabled.
- Login/redirect/logout-ish paths also call `UpdateGroupMemberState()` from
  `CharacterHandler` and `WorldSession`.

### Instance reset and bind extension

- Inbound ToCloud9 events are registered through
  `TC9SetOnGroupInstanceResetRequestHook` and
  `TC9SetOnGroupInstanceBindExtensionRequestHook`.
- `TC9GroupHooks::OnGroupInstanceResetRequest()` performs local reset/unbind work
  for local players or local group members.
- `TC9GroupHooks::OnGroupInstanceBindExtensionRequest()` delegates to
  `InstanceSaveMgr::ClusterSetPlayerBindExtension()` and sends raid info to a
  local connected player.
- `WorldSession::HandleResetInstancesOpcode()` and dungeon/raid difficulty reset
  flows call local `Player::ResetInstances` / `Group::ResetInstances`; no
  outbound ToCloud9 instance-reset request is visible in this repo.
- `WorldSession::HandleSetSavedInstanceExtend()` updates the local bind and DB
  directly; no outbound ToCloud9 bind-extension request is visible in this repo.
- `HandleSetSavedInstanceExtend()` still writes `GetPlayer()->GetGUID().GetCounter()`
  while `ClusterSetPlayerBindExtension()` uses `GetDBValue()` with realm context
  in crossrealm mode.

### Guild creation

- `ToCloud9Sidecar::SetupGrpcHandlers()` registers
  `ToCloud9GrpcHandler::CreateGuild` as the `GuildCreateHandler`.
- `ToCloud9GrpcHandler::CreateGuild()` validates the request, finds the connected
  leader by `CreatePlayerFromDBValue(request->leaderGuid)`, creates the guild,
  adds it to `GuildMgr`, and returns a service error code/guild id.
- `Guild::Create()` uses realm context in crossrealm mode, but writes the leader
  as `m_leaderGuid.GetCounter()`.
- `WorldSession::HandleGuildCreateOpcode()` intentionally logs guild creation
  packets as a cheating attempt; normal WotLK guild creation likely remains the
  petition workflow, while ToCloud9 guild creation enters through the registered
  sidecar/gRPC handler.

## Audit findings

### P0: build blocker / crash / data corruption

1. **Stub/default build cannot compile the current member-state call path.**
   The C++ call in `ToCloud9Sidecar::UpdateGroupMemberState()` uses the
   11-argument real API, but the stub header and stub C implementation expose an
   8-argument API. Any build that selects the stub (`USE_REAL_LIBSIDECAR=OFF`, or
   Windows per CMake) should fail compilation at the call site.

2. **The checked-out `deps/libsidecar/libsidecar.so` is likely corrupt/truncated.**
   It has an ELF header and a non-zero size, but `readelf` reports that section
   header reads extend past the end of the file, and `nm -D` reports an
   unrecognized file format. A `USE_REAL_LIBSIDECAR=ON` link is likely to fail or
   produce a broken runtime unless the real shared object is rebuilt/copied.

3. **Cluster GUID generation may truncate ToCloud9 64-bit IDs.**
   In cluster mode, player and item generators call ToCloud9 GUID services but
   return `ObjectGuid::LowType`. If the service returns a crossrealm/raw 64-bit
   GUID, the high realm/type bits are lost before player/item creation. This is a
   data-corruption risk for crossrealm identity and DB ownership.

### P1: feature cannot work correctly

1. **Ready check outbound cluster flow is missing.**
   Inbound ready-check hooks can deliver remote ToCloud9 events to local clients,
   but local client ready-check start, response, and finish handlers only build
   and broadcast local packets. Cross-worldserver ready checks cannot originate
   correctly from this worldserver without outbound sidecar calls.

2. **Subgroup and swap outbound cluster flow is missing.**
   Inbound `GroupMemberSubGroupChanged` can update local slots, but local
   subgroup-change/swap handlers call `Group::ChangeMembersGroup()` only. Remote
   worldservers/services will not learn about the change from this code path.

3. **Assistant/main tank/main assist outbound cluster flow is missing.**
   Inbound `GroupMemberFlagsChanged` can update local slots, but local assistant
   and party-assignment handlers update local flags only. In cluster mode, DB
   persistence is skipped and no visible replacement sidecar call exists.

4. **Instance reset and saved-instance extension only have inbound service hooks.**
   Local reset and bind-extension handlers update local state directly and do not
   visibly publish requests to ToCloud9. Cross-worldserver group members cannot be
   coordinated from these local actions.

5. **Live member state currently relies on worldserver hot-path publication.**
   HP/power/max/zone updates are emitted from `Unit`/`Player` setters. This can
   make remote frames update, but it violates the target architecture and risks
   per-player/per-stat fanout from gameplay hot paths.

6. **Remote member state API/contract appears split between percentage and raw
   values.**
   The stub `TC9UpdateGroupMemberState` still exposes percentage fields while the
   real header, inbound struct, and C++ call use raw current/max values plus
   power type. This makes non-real builds diverge from real builds and obscures
   which contract groupserver/gateway should own.

### P2: unsafe / incomplete / risky

1. **No local ToCloud9 service repo is available for contract verification.**
   This audit could not verify whether real ToCloud9 currently publishes the
   inbound event shapes or expects the outbound raw-vs-percentage API.

2. **Member-state logging is too noisy for hot paths.**
   `UpdateGroupMemberState()` and inbound member-state handling log at info level,
   while health/power changes can be frequent. These should be debug/sampled if
   the path remains temporarily.

3. **`Group::SendClusterMemberStats` broadcasts to every local connected group
   slot without an explicit authorization check beyond group membership.**
   This is probably acceptable for current synthetic group state, but gateway or
   service-routed future state should enforce group-scoped recipients explicitly.

4. **`Group::SetClusterMemberSubGroup()` may decrement subgroup counters using a
   member's existing group without first proving counter state is initialized and
   in range.**
   The function guards the target subgroup but not the current stored subgroup.

5. **Crossrealm guild persistence mixes realm context with low-counter leader
   storage.**
   `Guild::Create()` sets realm context but inserts `m_leaderGuid.GetCounter()`.
   This may be intended for realm-scoped SQL, but it should be verified against
   the ToCloud9 character DB schema before relying on it.

### P3: cleanup / refactor

1. **`TC9Percent()` is now dead code.**
   The helper remains in `TC9Sidecar.cpp`, but the real member-state call sends
   raw current/max values rather than percentages.

2. **`TC9SetMonitoringDataCollectorHandler` stub panic message names the wrong
   function.**
   The stub calls `panicWithTC9Unavailable("TC9SetOnMapsReassignedHook")`, which
   is misleading during debugging.

3. **`ObjectGuid.h` includes `TC9Sidecar.h`.**
   This couples low-level GUID generation to cluster services and increases
   rebuild/circular-dependency risk. It is not a phase-1 blocker but should be
   considered after GUID semantics are stabilized.

## Architecture decision

Short term: stabilize the current sidecar ABI and build before changing feature
behavior. Keep current worldserver-originated member-state hooks only as a
transitional compatibility path until the ToCloud9 gateway/groupserver contract is
verified.

Target direction: move continuous remote party/raid frame presentation away from
worldserver hot paths and toward gateway-observed object-update extraction with
bulk snapshot publication. AzerothCore should keep discrete gameplay-originated
cluster hooks only where gateway cannot infer authoritative intent: ready checks,
subgroup changes, role/flag changes, instance reset/extension, and guild creation.

## Milestones

### Phase 1 — Build and ABI guardrail

- **Goal:** Make the current tree buildable in both default/stub mode and
  `USE_REAL_LIBSIDECAR=ON`, without changing gameplay behavior.
- **Files/functions likely involved:**
  - `deps/libsidecar/include/libsidecar.h`
  - `deps/libsidecar/stub/libsidecar.h`
  - `deps/libsidecar/stub/libsidecar.c`
  - `deps/libsidecar/include/events-group.h`
  - `deps/libsidecar/stub/events-group.h`
  - `src/server/game/TC9Sidecar/TC9Sidecar.cpp::UpdateGroupMemberState`
  - `deps/libsidecar/CMakeLists.txt`
- **Exact expected behavior:**
  - Stub and real headers expose the same C ABI for all functions used by C++.
  - Stub functions compile and fail safely only when called with cluster mode
    enabled under the stub, as they already intend.
  - The checked-in or copied `libsidecar.so` is a valid Linux x86_64 shared
    object with readable dynamic symbols when `USE_REAL_LIBSIDECAR=ON` is used.
  - No group/guild/instance behavior changes.
- **Risks:**
  - If the local real `libsidecar.so` is generated from a different ToCloud9
    contract than the generated header, matching only the stub may hide a real
    service mismatch.
  - Replacing `libsidecar.so` should not overwrite user work without confirming
    the generation source.
- **Verification steps:**
  - `git diff -- deps/libsidecar/include deps/libsidecar/stub src/server/game/TC9Sidecar/TC9Sidecar.cpp`
  - `stat -c '%n %s bytes' deps/libsidecar/libsidecar.so`
  - `readelf -h deps/libsidecar/libsidecar.so`
  - `nm -D deps/libsidecar/libsidecar.so | rg 'TC9UpdateGroupMemberState|TC9SetOnGroupReady|TC9SetGuildCreateHandler'`
  - `cmake -S . -B build-stub -DSCRIPTS=static -DMODULES=static -DBUILD_TESTING=OFF`
  - `cmake --build build-stub -j$(nproc) --target worldserver`
  - `cmake -S . -B build-real -DUSE_REAL_LIBSIDECAR=ON -DSCRIPTS=static -DMODULES=static -DBUILD_TESTING=OFF`
  - `cmake --build build-real -j$(nproc) --target worldserver`
- **Rollback strategy:**
  - Revert only the header/stub/CMake/libsidecar artifact changes from this
    phase. No DB or runtime state changes are introduced.

### Phase 2 — GUID boundary audit and fix

- **Goal:** Make player/item GUID values unambiguous and prevent raw 64-bit
  ToCloud9 GUID truncation or accidental DB low-counter use at service/DB
  boundaries.
- **Files/functions likely involved:**
  - `src/server/game/Entities/Object/ObjectGuid.h`
  - `src/server/game/Handlers/CharacterHandler.cpp`
  - `src/server/game/Entities/Item/Item.cpp`
  - `src/server/game/TC9Sidecar/TC9GroupHooks.cpp::TC9PlayerGuid`
  - `src/server/game/TC9Sidecar/TC9GuildHooks.cpp`
  - `src/server/game/TC9Sidecar/TC9GrpcHandler.cpp`
  - `src/server/game/Guilds/Guild.cpp::Create`
  - SQL prepared statement usage around player/guild/instance ownership.
- **Exact expected behavior:**
  - Every sidecar field name and call site documents whether it is raw object
    GUID, DB-value GUID, or low counter.
  - New character/item creation preserves ToCloud9-generated identity exactly or
    explicitly requests low counters only.
  - DB writes use `GetDBValue()` plus realm context when crossrealm schemas expect
    raw/crossrealm values, and use `GetCounter()` only when proven realm-scoped.
  - Inbound ToCloud9 IDs continue to normalize through `CreatePlayerFromDBValue`.
- **Risks:**
  - Changing persisted GUID semantics can corrupt data if the ToCloud9 DB schema
    expects the current low-counter behavior.
  - Item GUID high bits may not be supported by all client packet paths.
- **Verification steps:**
  - `rg -n 'GetRawValue|GetDBValue|GetCounter|CreatePlayerFromDBValue|GenerateCharacterGuid|GenerateItemGuid' src/server/game deps/libsidecar`
  - Create a character in non-cluster mode and verify traditional low GUID DB
    values.
  - Create a character in cluster/crossrealm mode and verify realm/id round-trip
    through login, group membership, guild creation, and item ownership.
  - Run `worldserver` startup in both cluster disabled and enabled modes.
- **Rollback strategy:**
  - Revert GUID generation and persistence changes as one isolated commit.
  - If data was generated during manual testing, delete only the test characters,
    items, guilds, and related rows created during the test.

### Phase 3 — Discrete group feature outbound hooks

- **Goal:** Publish local gameplay-originated ready check, subgroup, assistant,
  main tank, and main assist changes to ToCloud9 while preserving local same-node
  behavior.
- **Files/functions likely involved:**
  - `src/server/game/Handlers/GroupHandler.cpp::HandleRaidReadyCheckOpcode`
  - `src/server/game/Handlers/GroupHandler.cpp::HandleRaidReadyCheckFinishedOpcode`
  - `src/server/game/Handlers/GroupHandler.cpp::HandleGroupChangeSubGroupOpcode`
  - `src/server/game/Handlers/GroupHandler.cpp::HandleGroupSwapSubGroupOpcode`
  - `src/server/game/Handlers/GroupHandler.cpp::HandleGroupAssistantLeaderOpcode`
  - `src/server/game/Handlers/GroupHandler.cpp::HandlePartyAssignmentOpcode`
  - `src/server/game/Groups/Group.cpp::ChangeMembersGroup`
  - `src/server/game/Groups/Group.cpp::SetGroupMemberFlag`
  - `src/server/game/TC9Sidecar/TC9Sidecar.*`
  - `deps/libsidecar/include/libsidecar.h`
  - `deps/libsidecar/stub/libsidecar.*`
- **Exact expected behavior:**
  - A local ready-check start publishes one ToCloud9 event and still displays to
    local members.
  - A local ready-check response publishes the member state and still displays to
    eligible local leader/assistant clients.
  - A local ready-check finish publishes one finish event and still displays
    locally.
  - Local subgroup changes and swaps publish the final affected member subgroup
    values once per changed member.
  - Assistant/main tank/main assist changes publish final flags/roles once, with
    main tank/main assist uniqueness preserved.
  - Non-cluster and same-worldserver behavior remains unchanged.
- **Risks:**
  - Echo loops if inbound ToCloud9 events call local mutators that publish again.
  - Duplicate packets to local players if both local path and inbound echo fire.
  - Service contract may not yet expose outbound functions matching inbound hook
    structs.
- **Verification steps:**
  - `rg -n 'ReadyCheck|ChangeSubGroup|SwapSubGroup|Assistant|PartyAssignment|SetGroupMemberFlag|TC9' src/server/game/Handlers src/server/game/Groups src/server/game/TC9Sidecar deps/libsidecar`
  - Build stub and real worldserver targets.
  - Manual two-worldserver QA: ready-check start/answer/finish, subgroup move,
    subgroup swap, assistant toggle, MT toggle, MA toggle.
  - Confirm no duplicate member packets in local-only groups.
- **Rollback strategy:**
  - Revert outbound sidecar calls and any new sidecar function declarations for
    this phase. Inbound hooks remain as before.

### Phase 4 — Instance reset and bind-extension outbound hooks

- **Goal:** Route local instance reset and saved-instance extension requests
  through ToCloud9 when cluster mode is enabled, while retaining local behavior in
  non-cluster mode.
- **Files/functions likely involved:**
  - `src/server/game/Handlers/MiscHandler.cpp::HandleResetInstancesOpcode`
  - Dungeon/raid difficulty reset flows in `MiscHandler.cpp`
  - `src/server/game/Handlers/CalendarHandler.cpp::HandleSetSavedInstanceExtend`
  - `src/server/game/Groups/Group.cpp::ResetInstances`
  - `src/server/game/Entities/Player/PlayerMisc.cpp::ResetInstances`
  - `src/server/game/TC9Sidecar/TC9GroupHooks.cpp::OnGroupInstanceResetRequest`
  - `src/server/game/TC9Sidecar/TC9GroupHooks.cpp::OnGroupInstanceBindExtensionRequest`
  - `src/server/game/Instances/InstanceSaveMgr.cpp::ClusterSetPlayerBindExtension`
- **Exact expected behavior:**
  - In cluster mode, a leader reset request is validated locally enough to avoid
    obvious invalid requests, then published once to ToCloud9 for group-scoped
    routing.
  - Inbound reset events perform local reset/unbind only for local affected
    players and report expected client feedback.
  - Bind-extension changes are published to ToCloud9 and applied consistently to
    local and remote members.
  - Non-cluster behavior remains the original direct local reset/DB update path.
- **Risks:**
  - Reset semantics are sensitive: applying locally before service fanout can
    diverge from remote nodes; applying only after service fanout can delay client
    feedback.
  - DB realm-context assumptions must be verified before changing local writes.
- **Verification steps:**
  - Build stub and real worldserver targets.
  - Manual QA with two worldservers: normal reset, difficulty-change reset,
    saved instance extension on/off, permission failure, no group, group leader,
    non-leader.
  - Verify `character_instance.extended` and bind rows for crossrealm test
    characters.
- **Rollback strategy:**
  - Revert outbound routing changes. Local direct reset/extension behavior should
    return unchanged.

### Phase 5 — Transitional member-state hardening

- **Goal:** Make current worldserver-published member state less unsafe while the
  gateway-driven design is prepared.
- **Files/functions likely involved:**
  - `src/server/game/TC9Sidecar/TC9Sidecar.cpp::UpdateGroupMemberState`
  - `src/server/game/TC9Sidecar/TC9GroupHooks.cpp::OnGroupMemberStateChanged`
  - `src/server/game/Entities/Unit/Unit.cpp::SetHealth`
  - `src/server/game/Entities/Unit/Unit.cpp::SetMaxHealth`
  - `src/server/game/Entities/Unit/Unit.cpp::SetPower`
  - `src/server/game/Entities/Unit/Unit.cpp::SetMaxPower`
  - `src/server/game/Entities/Player/PlayerUpdates.cpp::UpdateZone`
  - `src/server/game/Groups/Group.cpp::SetClusterMemberState`
- **Exact expected behavior:**
  - Logs on member-state hot paths are debug-level or sampled.
  - The outbound member-state contract is one consistent raw-current/max contract
    or one consistent percent contract; no stub/real divergence.
  - If keeping this path temporarily, updates are deduplicated/throttled enough to
    avoid per-regeneration spam.
  - No move to a new high-frequency polling loop is introduced.
- **Risks:**
  - Throttling too aggressively can make remote party frames feel stale.
  - Any worldserver-side state publication remains a temporary architecture
    compromise.
- **Verification steps:**
  - Build stub and real worldserver targets.
  - Manual QA: health damage/heal, power spend/regenerate, level-up, zone/map
    transition, login/logout/redirect.
  - Measure log volume and number of sidecar publish calls during combat/regeneration.
- **Rollback strategy:**
  - Revert only throttling/log-level/member-state contract changes. Discrete group
    feature phases remain independent.

### Phase 6 — Gateway-driven remote frame contract design

- **Goal:** Define the next architecture-compatible contract for gateway-extracted
  object-update snapshots and groupserver-routed group member patches.
- **Files/functions likely involved:**
  - AzerothCore files only if a temporary compatibility hook must be removed or
    feature-flagged.
  - ToCloud9 gateway/groupserver/protobuf/NATS files once the ToCloud9 repo is
    available.
- **Exact expected behavior:**
  - Bulk snapshots identify realm, worldserver, gateway, raw player DB/value GUID,
    online state, level, class, zone, map, health percent or raw health contract,
    power percent or raw power contract, and timestamp/sequence.
  - Gateway publishes snapshots in bulk on a fixed cadence; groupserver routes
    group-scoped patches; gateway sends client-facing packets.
  - AzerothCore no longer publishes HP/power changes from hot worldserver setters.
- **Risks:**
  - Requires ToCloud9 repo/service contract verification not available in this
    audit environment.
  - Packet extraction must not trust client input or mutate original packets.
- **Verification steps:**
  - Inventory ToCloud9 gateway/groupserver/protobuf/NATS paths with `rg` once the
    repo is available.
  - Unit-test snapshot extraction and routing.
  - Manual two-worldserver party/raid frame QA for online, level, class, zone,
    map, health, and power.
- **Rollback strategy:**
  - Keep Phase 5 transitional member-state path behind a config flag until the
    gateway path is proven; rollback by disabling gateway snapshot publication.

### Phase 7 — Guild creation integration verification

- **Goal:** Verify and, if needed, complete the existing sidecar-driven guild
  creation path without changing the normal petition workflow unexpectedly.
- **Files/functions likely involved:**
  - `src/server/game/TC9Sidecar/TC9GrpcHandler.cpp::CreateGuild`
  - `src/server/game/Guilds/Guild.cpp::Create`
  - `src/server/game/Handlers/GuildHandler.cpp::HandleGuildCreateOpcode`
  - `src/server/game/TC9Sidecar/TC9GuildHooks.cpp`
  - `deps/libsidecar/include/guild-api.h`
  - `deps/libsidecar/stub/guild-api.h`
- **Exact expected behavior:**
  - ToCloud9 can request guild creation for a connected local leader and receive
    deterministic error codes for invalid name, duplicate name, missing leader,
    and internal failure.
  - Crossrealm DB persistence stores the leader/guild fields using the schema's
    expected ID form.
  - Local guild cache, character cache, leader rank, and client state are updated.
  - Non-cluster petition/guild behavior remains unchanged.
- **Risks:**
  - `Guild::Create()` may use low-counter DB writes intentionally under realm
    context, but this must be verified against ToCloud9 migrations.
  - Returning `InternalError` for leader already in guild may need a more precise
    service error if ToCloud9 expects one.
- **Verification steps:**
  - Build stub and real worldserver targets.
  - Manual/service QA: create guild, duplicate name, leader not found, leader
    already in guild, DB persistence, relog visibility.
- **Rollback strategy:**
  - Revert only guild API/handler changes. Existing local guild creation remains.

### Phase 8 — Hardening and regression matrix

- **Goal:** Validate the cluster behavior end-to-end and remove temporary unsafe
  hooks once the gateway path is functional.
- **Files/functions likely involved:**
  - All files touched in Phases 1-7.
  - Test scripts or docs added for repeatable QA.
- **Exact expected behavior:**
  - Single-worldserver grouping/guild/instance behavior remains unchanged.
  - Cluster-disabled builds and startup work.
  - Cluster-enabled builds and startup work with real `libsidecar`.
  - Two-worldserver party/raid/guild/instance behavior matches the verification
    matrix in `AGENTS.md`.
- **Risks:**
  - Environment gaps (MySQL/Redis/NATS/ToCloud9 services) can hide integration
    failures until manual QA.
- **Verification steps:**
  - Full build with default/stub and `USE_REAL_LIBSIDECAR=ON`.
  - `ctest --output-on-failure` if unit tests are enabled.
  - Manual QA matrix from `AGENTS.md` sections 10.1-10.5.
- **Rollback strategy:**
  - Roll back by phase in reverse order. Each phase should be one small commit or
    PR segment with no unrelated refactors.

## Open questions

1. What exact ID form does current ToCloud9 `guidserver` return for character and
   item GUIDs in this branch: low counter, DB-value GUID, or full raw object GUID?
2. Does current ToCloud9 groupserver already expose outbound APIs for ready-check,
   subgroup, flags/roles, instance reset, and bind extension, or are only inbound
   hooks generated in AzerothCore?
3. Does current ToCloud9 member-state contract expect raw health/max/power or
   percentages? The AzerothCore real header and stub disagree.
4. Which DB columns in the ToCloud9 character schema are realm-scoped low counters
   vs full crossrealm/raw IDs?
5. Is guild creation intended to be sidecar/gRPC-only, petition-only, or both?
6. Should transitional worldserver member-state publication be retained behind a
   config flag while gateway extraction is implemented?

## Implementation log

- Audited local `AGENTS.md` instructions.
- Confirmed repository branch/status with `git status --short --branch`.
- Searched local custom cluster paths with `rg` across `src/server/game` and
  `deps/libsidecar`.
- Inspected sidecar CMake, real/stub headers, stub C implementation, TC9 sidecar,
  group hooks, guild hooks, group handlers, instance handlers, GUID helpers,
  player/item creation paths, and hot member-state hooks.
- Did not modify production code.

## Verification log

Commands run during this audit:

```bash
git status --short --branch
find .. -name AGENTS.md -print
rg -n "TC9|ToCloud9|sidecar|libsidecar|Cluster" deps src --glob '!deps/libsidecar/libsidecar.so'
rg -n "ReadyCheck|SubGroup|Assistant|MainTank|MainAssist|MemberState|InstanceReset|BindExtension|GuildCreate|UpdateGroupMemberState|DBValue|GetCounter|GetGUIDLow|GetGUID\(\)\.Get" src/server/game deps/libsidecar/include deps/libsidecar/stub
diff -u <(rg -n "TC9UpdateGroupMemberState|TC9SetOnGroup|TC9SetGuildCreateHandler|TC9InitLib" deps/libsidecar/include/libsidecar.h) <(rg -n "TC9UpdateGroupMemberState|TC9SetOnGroup|TC9SetGuildCreateHandler|TC9InitLib" deps/libsidecar/stub/libsidecar.h) || true
stat -c '%n %s bytes' deps/libsidecar/libsidecar.so
nm -D deps/libsidecar/libsidecar.so | rg "TC9UpdateGroupMemberState|TC9SetOnGroupReady|TC9SetOnGroupMemberFlags|TC9SetGuildCreateHandler" || true
readelf -h deps/libsidecar/libsidecar.so || true
cmake -S . -B build-audit-real -DUSE_REAL_LIBSIDECAR=ON -DSCRIPTS=static -DMODULES=static -DBUILD_TESTING=OFF
rm -rf build-audit-real
```

Results:

- `git status` showed branch `work` and a pre-existing modified
  `deps/libsidecar/libsidecar.so`.
- `rg` searches found the current sidecar, group, guild, instance, GUID, and
  member-state paths listed above.
- Header diff confirmed the real/stub `TC9UpdateGroupMemberState` signature
  mismatch.
- `stat` reported `deps/libsidecar/libsidecar.so` is 24,776,754 bytes.
- `readelf` reported section headers extend past EOF; `nm -D` could not read the
  shared object.
- CMake configure with `USE_REAL_LIBSIDECAR=ON` reached dependency discovery and
  reported `Use stub for libsidecar: No`, then failed because Boost development
  components were missing in this environment. The temporary build directory was
  removed.

## Rollback notes

- This audit adds only `CLUSTER_PLAN.md`.
- To roll back this audit, remove `CLUSTER_PLAN.md`.
- The audit did not modify production code or the pre-existing modified
  `deps/libsidecar/libsidecar.so`.
