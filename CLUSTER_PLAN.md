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
- Local source of truth for production changes: this repository only.
- External reference source for service contracts: read-only clone of
  `https://github.com/VG-prog/ToCloud9` at commit `aa66960` in
  `/tmp/tocloud9-reference`; source code, protobufs, SQL, and C ABI headers were
  used as evidence only.
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
  declare/define an 8-argument percentage `TC9UpdateGroupMemberState`
  signature with `healthPct` and `powerPct`, matching the verified ToCloud9 Go
  c-shared source but not the bundled AzerothCore real header.
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

1. **AzerothCore bundled real header/C++ call disagrees with verified ToCloud9 member-state ABI.**
   The C++ call in `ToCloud9Sidecar::UpdateGroupMemberState()` and
   `deps/libsidecar/include/libsidecar.h` use an 11-argument raw-value API, but
   the verified ToCloud9 Go c-shared source and bundled stub expose an
   8-argument percentage API. Stub builds still need a coordinated fix that
   matches the actual ToCloud9 contract instead of only matching the stale real
   header.

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

6. **Remote member state service ownership is still transitional and percentage-based in ToCloud9.**
   The verified ToCloud9 groupserver, gateway, NATS event, and Go c-shared
   libsidecar contract uses `healthPct` and `powerPct`. Future work should
   either restore AzerothCore to that percentage ABI or intentionally migrate
   ToCloud9 and AzerothCore together to a new raw-current/max contract before
   removing transitional worldserver-originated publication.

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

### Phase 1 — Build/libsidecar ABI consistency

- **Goal:** Make the current tree buildable in both default/stub mode and
  `USE_REAL_LIBSIDECAR=ON`, and prove that the generated real header, stub
  header, stub implementation, copied shared object, and C++ call sites agree
  before changing gameplay behavior.
- **Files/functions likely involved:**
  - `deps/libsidecar/include/libsidecar.h`
  - `deps/libsidecar/stub/libsidecar.h`
  - `deps/libsidecar/stub/libsidecar.c`
  - `deps/libsidecar/include/events-group.h`
  - `deps/libsidecar/stub/events-group.h`
  - `src/server/game/TC9Sidecar/TC9Sidecar.cpp::UpdateGroupMemberState`
  - `deps/libsidecar/CMakeLists.txt`
- **Mandatory ToCloud9 verification before behavior/API changes:**
  - Verify the ToCloud9 repo/service-contract source for generated C ABI,
    protobuf, NATS subjects, gateway behavior, and groupserver expectations
    before changing protobuf/NATS/gateway/groupserver behavior.
  - Do not invent or regenerate service contracts from the AzerothCore side
    alone.
  - Verified ToCloud9 reference paths for Phase 1:
    - `game-server/libsidecar/events-group.go::TC9UpdateGroupMemberState`
      exports the Go c-shared function with 8 arguments ending in
      `healthPct` and `powerPct`.
    - `game-server/libsidecar/libsidecar.h::TC9UpdateGroupMemberState`
      declares the same 8-argument percentage ABI.
    - `game-server/libsidecar/events-group.h::GroupMemberStateChanged`
      carries `healthPct` and `powerPct` only.
    - `api/proto/v1/group/group.proto::UpdateMemberStateRequest`,
      `apps/groupserver/server/group.go::UpdateMemberState`,
      `apps/groupserver/service/group.go::UpdateMemberState`, and
      `shared/events/events-group.go::GroupEventMemberStateChangedPayload`
      use percentage member-state fields.
    - `game-server/libsidecar-cpp/include/events-group.h` does not include the
      newer ready-check/subgroup/flags/member-state/instance hook structs, so
      it is not the current source for the Go c-shared ABI used here.
- **Exact expected behavior:**
  - Stub and real headers expose the same C ABI for all functions used by C++,
    but that ABI must be reconciled against verified ToCloud9 source instead of
    assuming the bundled real header is authoritative.
  - For the current ToCloud9 service contract, member-state ABI compatibility
    means `TC9UpdateGroupMemberState(memberGuid, online, level, class, zoneId,
    mapId, healthPct, powerPct)`.
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

### Phase 2 — GUID boundary correctness

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
- **Verified ToCloud9 reference files/functions for this phase:**
  - `api/proto/v1/guid/guid.proto::GuidService.GetGUIDPool` returns
    `GuidDiapason { start, end }` ranges for a `realmID` and `GuidType`.
  - `apps/guidserver/server/guids.go::GetGUIDPool` returns those ranges
    without object-guid packing.
  - `apps/guidserver/service/guid.go::GetGuids` allocates numeric ranges from
    per-realm caches.
  - `apps/guidserver/repo/mysql-max-guid-provider.go` initializes max values
    from `characters.guid`, `item_instance.guid`, and `instance.id`.
  - `game-server/libsidecar/guids.go::TC9GetNextAvailableCharacterGuid` and
    `TC9GetNextAvailableItemGuid` return `CrossrealmMgr.Next(realmID)` values.
  - `shared/repo/charactersdb.go::CharactersDB` stores separate DB handles and
    prepared statements by `realmID`, so ToCloud9 treats many SQL identifiers as
    realm-scoped DB values plus explicit `realmID`, not packed AzerothCore
    `ObjectGuid` raw values.
- **Mandatory ToCloud9 verification before persistence/API changes:**
  - Verify current ToCloud9 `guidserver`, character DB migrations, groupserver,
    guildserver, and any protobuf/NATS ID fields before changing generated GUID
    semantics, persisted DB values, or service field meanings.
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

### Phase 3 — Cluster group member live state updates

- **Goal:** Harden the existing cluster group member live-state path for remote
  party/raid frames while the gateway-driven architecture is designed and proven.
  This is explicitly transitional: the target architecture remains
  gateway-observed object-update extraction with bulk snapshot publication, but
  this phase may make the current worldserver-side publication path consistent,
  bounded, and testable.
- **State fields in scope:**
  - online/offline
  - level
  - class
  - zone
  - map
  - health
  - max health
  - power type
  - power
  - max power
- **Files/functions likely involved:**
  - `src/server/game/TC9Sidecar/TC9Sidecar.cpp::UpdateGroupMemberState`
  - `src/server/game/TC9Sidecar/TC9GroupHooks.cpp::OnGroupMemberStateChanged`
  - `src/server/game/Entities/Unit/Unit.cpp::SetHealth`
  - `src/server/game/Entities/Unit/Unit.cpp::SetMaxHealth`
  - `src/server/game/Entities/Unit/Unit.cpp::SetPower`
  - `src/server/game/Entities/Unit/Unit.cpp::SetMaxPower`
  - `src/server/game/Entities/Player/PlayerUpdates.cpp::UpdateZone`
  - `src/server/game/Groups/Group.cpp::SetClusterMemberState`
  - `src/server/game/Groups/Group.cpp::BuildClusterMemberStatsPacket`
- **Verified ToCloud9 reference files/functions for this phase:**
  - `api/proto/v1/group/group.proto::UpdateMemberStateRequest` uses
    `healthPct` and `powerPct`.
  - `game-server/libsidecar/events-group.go::TC9UpdateGroupMemberState` sends
    those percentages to `groupServiceClient.UpdateMemberState`.
  - `apps/groupserver/service/group.go::UpdateMemberState` clamps percentages
    to `100`, updates online state, and publishes
    `GroupEventMemberStateChangedPayload`.
  - `shared/events/events-group.go::GroupEventMemberStateChangedPayload` carries
    `HealthPct` and `PowerPct`; there is no `powerType`, `health/maxHealth`, or
    `power/maxPower` in the current service event.
  - `apps/gateway/session/group-cluster-extra.go::sendPartyMemberStats` renders
    remote member stats as percentage current values with max `100` and derives
    power type from class.
  - No ToCloud9 gateway source was found that extracts player state from
    worldserver object-update packets or bulk-publishes player snapshots today.
- **Mandatory ToCloud9 verification before contract changes:**
  - Verify ToCloud9 groupserver/gateway/protobuf/NATS expectations for member
    state before changing event schemas, generated code, NATS subjects, gateway
    parsing, or groupserver routing.
  - Confirm whether ToCloud9 expects raw current/max values or percentages, and
    confirm the identity field form before changing the sidecar ABI or event
    payload.
- **Exact expected behavior:**
  - The outbound member-state contract matches the verified ToCloud9 percentage
    contract unless ToCloud9 and AzerothCore are deliberately migrated together
    to a new raw-current/max contract.
  - For the current contract, remote party/raid frames receive online/offline,
    level, class, zone, map, `healthPct`, and `powerPct`; gateway derives
    power type from class and sends max values as `100`.
  - Logs on member-state hot paths are debug-level or sampled.
  - If keeping this path temporarily, updates are deduplicated/throttled enough to
    avoid per-regeneration spam and no new high-frequency polling loop is added.
  - The transitional path can later be disabled or removed once the gateway path
    is proven.
- **Risks:**
  - Throttling too aggressively can make remote party frames feel stale.
  - Any worldserver-side live-state publication remains an architecture
    compromise and must not become the permanent target design.
  - Service echo loops or GUID mismatches can produce stale or duplicate member
    frames if Phase 2 is incomplete.
- **Verification steps:**
  - Build stub and real worldserver targets.
  - Manual QA: login, logout, redirect, level-up, zone transition, map
    transition, health damage/heal, max-health change, power spend/regenerate,
    max-power change, and power-type change.
  - Measure log volume and number of sidecar publish calls during combat and
    regeneration.
  - Verify same-worldserver groups still use normal local behavior.
- **Rollback strategy:**
  - Revert only member-state contract/logging/throttling changes. Discrete group,
    instance, and guild phases remain independent.

### Phase 4 — Ready check

- **Goal:** Publish local gameplay-originated ready-check start, member response,
  and finish events to ToCloud9 while preserving same-node behavior.
- **Files/functions likely involved:**
  - `src/server/game/Handlers/GroupHandler.cpp::HandleRaidReadyCheckOpcode`
  - `src/server/game/Handlers/GroupHandler.cpp::HandleRaidReadyCheckFinishedOpcode`
  - `src/server/game/TC9Sidecar/TC9Sidecar.*`
  - `src/server/game/TC9Sidecar/TC9GroupHooks.cpp`
  - `deps/libsidecar/include/libsidecar.h`
  - `deps/libsidecar/stub/libsidecar.*`
- **Verified ToCloud9 reference files/functions for this phase:**
  - `api/proto/v1/group/group.proto::StartReadyCheck`,
    `SetReadyCheckMemberState`, and `FinishReadyCheck` define the service RPCs.
  - `apps/groupserver/server/group.go::StartReadyCheck`,
    `SetReadyCheckMemberState`, and `FinishReadyCheck` forward those RPCs to
    `apps/groupserver/service/group.go`.
  - `apps/groupserver/service/group.go::StartReadyCheck`,
    `SetReadyCheckMemberState`, and `FinishReadyCheck` publish ready-check
    events.
  - `shared/events/events-group.go` defines NATS subjects
    `group.readycheck.started`, `group.readycheck.member.state`, and
    `group.readycheck.finished`.
  - `apps/gateway/session/group-cluster-extra.go::HandleRaidReadyCheck` calls
    groupserver for client-originated start/response packets, and
    `HandleEventGroupReadyCheckStarted`, `HandleEventGroupReadyCheckMemberState`,
    and `HandleEventGroupReadyCheckFinished` emit client-facing packets.
- **Mandatory ToCloud9 verification before API changes:**
  - Verify ToCloud9 ready-check protobuf/NATS/service contracts and generated
    sidecar functions before adding or changing outbound ready-check APIs.
- **Exact expected behavior:**
  - A local ready-check start publishes one ToCloud9 event and still displays to
    local members.
  - A local ready-check response publishes the member state and still displays to
    eligible local leader/assistant clients.
  - A local ready-check finish publishes one finish event and still displays
    locally.
  - Non-cluster and same-worldserver behavior remains unchanged.
- **Risks:**
  - Echo loops if inbound ToCloud9 events call local mutators that publish again.
  - Duplicate packets to local players if both local path and inbound echo fire.
- **Verification steps:**
  - `rg -n 'ReadyCheck|TC9' src/server/game/Handlers src/server/game/Groups src/server/game/TC9Sidecar deps/libsidecar`
  - Build stub and real worldserver targets.
  - Manual two-worldserver QA: ready-check start, answer, timeout/finish, and no
    duplicate local-only packets.
- **Rollback strategy:**
  - Revert outbound ready-check sidecar calls and any new declarations. Inbound
    ready-check hooks remain as before.

### Phase 5 — Raid subgroup movement

- **Goal:** Publish local subgroup changes and subgroup swaps to ToCloud9 while
  preserving same-node behavior.
- **Files/functions likely involved:**
  - `src/server/game/Handlers/GroupHandler.cpp::HandleGroupChangeSubGroupOpcode`
  - `src/server/game/Handlers/GroupHandler.cpp::HandleGroupSwapSubGroupOpcode`
  - `src/server/game/Groups/Group.cpp::ChangeMembersGroup`
  - `src/server/game/TC9Sidecar/TC9Sidecar.*`
  - `src/server/game/TC9Sidecar/TC9GroupHooks.cpp`
  - `deps/libsidecar/include/libsidecar.h`
  - `deps/libsidecar/stub/libsidecar.*`
- **Verified ToCloud9 reference files/functions for this phase:**
  - `api/proto/v1/group/group.proto::ChangeMemberSubGroup` defines the service
    RPC.
  - `apps/groupserver/server/group.go::ChangeMemberSubGroup` forwards to
    `apps/groupserver/service/group.go::ChangeMemberSubGroup`, which validates
    leader/assistant permission, persists `member.SubGroup`, and publishes
    `GroupMemberSubGroupChanged`.
  - `shared/events/events-group.go` defines the `group.member.subgroup.changed`
    NATS subject and `GroupEventMemberSubGroupChangedPayload`.
  - `apps/gateway/session/group-cluster-extra.go::HandleGroupChangeSubGroup`
    calls groupserver; `HandleEventGroupMemberSubGroupChanged` refreshes the
    group update for local clients.
- **Mandatory ToCloud9 verification before API changes:**
  - Verify ToCloud9 subgroup movement protobuf/NATS/service contracts and
    generated sidecar functions before adding or changing outbound subgroup APIs.
- **Exact expected behavior:**
  - Local subgroup changes and swaps publish final affected member subgroup values
    once per changed member.
  - Remote nodes update local cached slots and client frames through inbound
    events.
  - Non-cluster and same-worldserver behavior remains unchanged.
- **Risks:**
  - Echo loops if inbound subgroup events republish.
  - Counter drift if local subgroup counts are mutated twice or applied out of
    order.
- **Verification steps:**
  - Build stub and real worldserver targets.
  - Manual two-worldserver QA: subgroup move, subgroup swap, invalid subgroup,
    full subgroup, reconnect after movement, and no duplicate local-only packets.
- **Rollback strategy:**
  - Revert outbound subgroup sidecar calls and any new declarations. Inbound
    subgroup hooks remain as before.

### Phase 6 — Assistant leader / main tank / main assist flags

- **Goal:** Publish local assistant leader, main tank, and main assist flag changes
  to ToCloud9 while preserving uniqueness and same-node behavior.
- **Files/functions likely involved:**
  - `src/server/game/Handlers/GroupHandler.cpp::HandleGroupAssistantLeaderOpcode`
  - `src/server/game/Handlers/GroupHandler.cpp::HandlePartyAssignmentOpcode`
  - `src/server/game/Groups/Group.cpp::SetGroupMemberFlag`
  - `src/server/game/Groups/Group.cpp::RemoveUniqueGroupMemberFlag`
  - `src/server/game/TC9Sidecar/TC9Sidecar.*`
  - `src/server/game/TC9Sidecar/TC9GroupHooks.cpp`
  - `deps/libsidecar/include/libsidecar.h`
  - `deps/libsidecar/stub/libsidecar.*`
- **Verified ToCloud9 reference files/functions for this phase:**
  - `api/proto/v1/group/group.proto::SetMemberFlags` defines one RPC carrying
    final `flags` and `roles`.
  - `apps/groupserver/server/group.go::SetMemberFlags` forwards to
    `apps/groupserver/service/group.go::SetMemberFlags`, which validates
    leader/assistant permission, persists member flags/roles, and publishes
    `GroupMemberFlagsChanged`.
  - `shared/events/events-group.go` defines the `group.member.flags.changed`
    NATS subject and `GroupEventMemberFlagsChangedPayload`.
  - `apps/gateway/session/group-cluster-extra.go::HandleGroupAssistantLeader`,
    `HandlePartyAssignment`, and `setGroupMemberFlag` calculate final flags and
    call groupserver; `HandleEventGroupMemberFlagsChanged` refreshes local group
    state.
- **Mandatory ToCloud9 verification before API changes:**
  - Verify ToCloud9 flag/role protobuf/NATS/service contracts and generated
    sidecar functions before adding or changing outbound flag APIs.
- **Exact expected behavior:**
  - Assistant/main tank/main assist changes publish final flags/roles once.
  - Main tank and main assist uniqueness is preserved across local and remote
    group members.
  - Non-cluster and same-worldserver behavior remains unchanged.
- **Risks:**
  - Echo loops if inbound flag events republish.
  - Duplicate or out-of-order uniqueness changes can leave more than one main
    tank/main assist in cached state.
- **Verification steps:**
  - Build stub and real worldserver targets.
  - Manual two-worldserver QA: assistant toggle, MT toggle, MA toggle, uniqueness
    replacement, demotion/removal, reconnect, and no duplicate local-only packets.
- **Rollback strategy:**
  - Revert outbound flag sidecar calls and any new declarations. Inbound flag
    hooks remain as before.

### Phase 7 — Clustered instance reset

- **Goal:** Route local instance reset requests through ToCloud9 when cluster mode
  is enabled, while retaining local behavior in non-cluster mode.
- **Files/functions likely involved:**
  - `src/server/game/Handlers/MiscHandler.cpp::HandleResetInstancesOpcode`
  - Dungeon/raid difficulty reset flows in `MiscHandler.cpp`
  - `src/server/game/Groups/Group.cpp::ResetInstances`
  - `src/server/game/Entities/Player/PlayerMisc.cpp::ResetInstances`
  - `src/server/game/TC9Sidecar/TC9GroupHooks.cpp::OnGroupInstanceResetRequest`
- **Verified ToCloud9 reference files/functions for this phase:**
  - `api/proto/v1/group/group.proto::ResetInstance` defines the service RPC.
  - `apps/groupserver/server/group.go::ResetInstance` forwards to
    `apps/groupserver/service/group.go::ResetInstance`, which validates group
    leader permission when grouped and publishes `GroupInstanceResetRequest`.
  - `shared/events/events-group.go` defines the `group.instance.reset.request`
    NATS subject and `GroupEventInstanceResetRequestPayload`.
  - `apps/gateway/session/group-cluster-extra.go::HandleResetInstances` calls
    groupserver for client-originated reset requests.
  - `game-server/libsidecar/events-group.go::GroupInstanceResetRequest` maps the
    NATS payload into the C hook request.
- **Mandatory ToCloud9 verification before API changes:**
  - Verify ToCloud9 instance-reset protobuf/NATS/service contracts and generated
    sidecar functions before adding or changing outbound reset APIs.
- **Exact expected behavior:**
  - In cluster mode, a leader reset request is validated locally enough to avoid
    obvious invalid requests, then published once to ToCloud9 for group-scoped
    routing.
  - Inbound reset events perform local reset/unbind only for local affected
    players and report expected client feedback.
  - Non-cluster behavior remains the original direct local reset path.
- **Risks:**
  - Reset semantics are sensitive: applying locally before service fanout can
    diverge from remote nodes; applying only after service fanout can delay client
    feedback.
- **Verification steps:**
  - Build stub and real worldserver targets.
  - Manual QA with two worldservers: normal reset, difficulty-change reset,
    permission failure, no group, group leader, and non-leader.
- **Rollback strategy:**
  - Revert outbound reset routing changes. Local direct reset behavior should
    return unchanged.

### Phase 8 — Clustered instance bind extension

- **Goal:** Route local saved-instance bind extension requests through ToCloud9
  when cluster mode is enabled, while retaining local behavior in non-cluster
  mode.
- **Files/functions likely involved:**
  - `src/server/game/Handlers/CalendarHandler.cpp::HandleSetSavedInstanceExtend`
  - `src/server/game/TC9Sidecar/TC9GroupHooks.cpp::OnGroupInstanceBindExtensionRequest`
  - `src/server/game/Instances/InstanceSaveMgr.cpp::ClusterSetPlayerBindExtension`
- **Verified ToCloud9 reference files/functions for this phase:**
  - `api/proto/v1/group/group.proto::SetInstanceBindExtension` defines the
    service RPC.
  - `apps/groupserver/server/group.go::SetInstanceBindExtension` forwards to
    `apps/groupserver/service/group.go::SetInstanceBindExtension`, which
    publishes `GroupInstanceBindExtensionRequest`.
  - `shared/events/events-group.go` defines the
    `group.instance.bind.extension.request` NATS subject and
    `GroupEventInstanceBindExtensionRequestPayload`.
  - `apps/gateway/session/group-cluster-extra.go::HandleSetSavedInstanceExtend`
    calls groupserver for client-originated saved-instance extension requests.
  - `game-server/libsidecar/events-group.go::GroupInstanceBindExtensionRequest`
    maps the NATS payload into the C hook request.
- **Mandatory ToCloud9 verification before API/DB changes:**
  - Verify ToCloud9 bind-extension protobuf/NATS/service contracts, generated
    sidecar functions, and character DB realm/GUID expectations before adding or
    changing outbound bind-extension APIs or DB ownership fields.
- **Exact expected behavior:**
  - Bind-extension changes are published to ToCloud9 and applied consistently to
    local and remote members.
  - DB writes use the proven crossrealm GUID form from Phase 2.
  - Non-cluster behavior remains the original direct local DB update path.
- **Risks:**
  - DB realm-context assumptions must be verified before changing local writes.
  - Applying stale events can re-toggle a bind after the player changes it again.
- **Verification steps:**
  - Build stub and real worldserver targets.
  - Manual QA with two worldservers: saved instance extension on/off, relog,
    reconnect, and DB verification of `character_instance.extended` and bind rows
    for crossrealm test characters.
- **Rollback strategy:**
  - Revert outbound bind-extension routing changes. Local direct extension
    behavior should return unchanged.

### Phase 9 — Guild creation verification/completion

- **Goal:** Verify and, if needed, complete the existing sidecar-driven guild
  creation path without changing the normal petition workflow unexpectedly.
- **Files/functions likely involved:**
  - `src/server/game/TC9Sidecar/TC9GrpcHandler.cpp::CreateGuild`
  - `src/server/game/Guilds/Guild.cpp::Create`
  - `src/server/game/Handlers/GuildHandler.cpp::HandleGuildCreateOpcode`
  - `src/server/game/TC9Sidecar/TC9GuildHooks.cpp`
  - `deps/libsidecar/include/guild-api.h`
  - `deps/libsidecar/stub/guild-api.h`
- **Verified ToCloud9 reference files/functions for this phase:**
  - `api/proto/v1/worldserver/worldserver.proto::WorldServerService.CreateGuild`
    is the ToCloud9 RPC for guild creation.
  - `game-server/libsidecar/grpc-api.go` registers `CreateGuild` on the
    worldserver gRPC bindings.
  - `game-server/libsidecar/guild-api.go::CreateGuildHandler` calls the C
    `GuildCreateHandler`; `game-server/libsidecar/guild-api.h` defines
    `GuildCreateRequest`, `GuildCreateResponse`, and error codes.
  - `game-server/libsidecar/grpcapi/server-guild.go::CreateGuild` maps C handler
    responses to gRPC statuses.
  - `api/proto/v1/guilds/guilds.proto::GuildService` has no `CreateGuild` RPC,
    so guildserver is not the creation entrypoint in this ToCloud9 snapshot.
- **Mandatory ToCloud9 verification before API/DB changes:**
  - Verify ToCloud9 guildserver/guild-create contracts, generated sidecar API,
    duplicate-name rules, error-code mapping, and character DB ownership before
    changing guild creation behavior or persistence.
- **Exact expected behavior:**
  - ToCloud9 can request guild creation for a connected local leader and receive
    deterministic error codes for invalid name, duplicate name, missing leader,
    leader already in guild, and internal failure.
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

### Phase 10 — Future architecture/design only

- **Goal:** Design future cross-service systems without implementing production
  code in this phase. This phase captures target architecture, service contracts,
  risks, and migration plans for larger systems after Phases 1-9 are stable.
- **Topics in scope:**
  - Gateway-driven member-state snapshots.
  - Guild bank.
  - Auction house.
  - Arenas.
  - LFG.
- **Gateway-driven member-state target architecture:**
  - Gateway observes server-to-client object update packets that already pass
    through it, extracts conservative presentational player snapshots, stores the
    latest state locally, and bulk-publishes changes to ToCloud9 services.
  - Groupserver routes group-scoped member-state patches, and gateway sends
    client-facing remote group/raid frame updates to authorized local sessions.
  - AzerothCore/worldserver should stop publishing continuous HP/power changes
    from hot setters once this path is proven, while retaining discrete
    gameplay-intent hooks for actions gateway cannot infer safely.
- **Mandatory ToCloud9 verification before any future implementation:**
  - Inventory the ToCloud9 repo with `rg` for gateway packet parsing, object
    update decoding, protobuf definitions, generated API, NATS subjects,
    groupserver routing, guildserver contracts, auction/arena/LFG ownership, and
    SQL migrations before changing protobuf/NATS/gateway/groupserver behavior.
  - Do not modify generated files unless the generator and expected committed
    outputs are proven in the ToCloud9 repo.
- **Deliverables:**
  - Architecture decision records or updates to this plan.
  - Evidence maps for each future system.
  - Contract sketches clearly marked as proposals until verified against
    ToCloud9 source.
- **Risks:**
  - Implementing these systems before stabilizing ABI/GUID/group basics would
    compound existing identity and service-contract uncertainty.
  - Gateway packet extraction can accidentally trust or leak state if
    authorization and packet decoding are not proven first.
- **Verification steps:**
  - Documentation review only in this phase.
  - No production code changes until a future implementation phase is explicitly
    approved.
- **Rollback strategy:**
  - Revert documentation/design updates only. No runtime or DB state is changed.

## Verified ToCloud9 answers and remaining questions

Reference source: read-only clone of `https://github.com/VG-prog/ToCloud9` at
commit `aa66960`. Source code, protobuf, SQL, and generated C headers were used
as evidence; README/issues were not used as instructions.

### Resolved answers

1. **ToCloud9 `guidserver` returns realm-scoped numeric DB GUID ranges, not
   packed AzerothCore `ObjectGuid` raw values.**
   Evidence: `api/proto/v1/guid/guid.proto::GetGUIDPool` returns
   `GuidDiapason { start, end }` for a `realmID` and `GuidType`;
   `apps/guidserver/server/guids.go::GetGUIDPool` forwards the ranges returned
   by `apps/guidserver/service/guid.go::GetGuids`;
   `apps/guidserver/repo/mysql-max-guid-provider.go` initializes character,
   item, and instance maxima from `SELECT COALESCE(MAX(guid), 0) FROM
   characters`, `SELECT COALESCE(MAX(guid), 0) FROM item_instance`, and
   `SELECT COALESCE(MAX(id), 0) FROM instance`;
   `game-server/libsidecar/guids.go::TC9GetNextAvailableCharacterGuid` and
   `TC9GetNextAvailableItemGuid` return the next value from a per-realm
   `CrossrealmMgr`. No inspected GUID path packs high-guid/realm bits into an
   AzerothCore raw `ObjectGuid`.

2. **ToCloud9 groupserver exposes outbound RPCs and NATS events for ready
   check, subgroup changes, member flags/roles, member state, instance reset,
   and instance bind extension.**
   Evidence: `api/proto/v1/group/group.proto::GroupService` declares
   `StartReadyCheck`, `SetReadyCheckMemberState`, `FinishReadyCheck`,
   `ChangeMemberSubGroup`, `SetMemberFlags`, `UpdateMemberState`,
   `ResetInstance`, and `SetInstanceBindExtension`;
   `apps/groupserver/server/group.go` forwards each RPC to
   `apps/groupserver/service/group.go`; that service publishes corresponding
   `GroupEvent...` payloads through `shared/events/producer-group.go`;
   `shared/events/events-group.go::SubjectName` maps them to NATS subjects
   `group.readycheck.started`, `group.readycheck.member.state`,
   `group.readycheck.finished`, `group.member.subgroup.changed`,
   `group.member.flags.changed`, `group.member.state.changed`,
   `group.instance.reset.request`, and
   `group.instance.bind.extension.request`.

3. **ToCloud9 member-state contract currently expects percentages, not raw
   health/maxHealth/power/maxPower plus powerType.**
   Evidence: `api/proto/v1/group/group.proto::UpdateMemberStateRequest` has
   `healthPct` and `powerPct`;
   `apps/groupserver/service/group.go::UpdateMemberState` accepts
   `healthPct, powerPct uint16` and clamps both at `100`;
   `shared/events/events-group.go::GroupEventMemberStateChangedPayload` carries
   `HealthPct` and `PowerPct`;
   `game-server/libsidecar/events-group.go::TC9UpdateGroupMemberState` exports
   an 8-argument C function ending in `healthPct` and `powerPct`;
   `game-server/libsidecar/events-group.h::GroupMemberStateChanged` and
   `game-server/libsidecar/libsidecar.h::TC9UpdateGroupMemberState` use the
   same percentage fields.

4. **ToCloud9 DB identity columns are mostly realm-scoped low DB values plus an
   explicit `realmID`; no DB column was proven to store packed AzerothCore raw
   `ObjectGuid` values.**
   Evidence: `shared/repo/charactersdb.go::CharactersDB` maintains DB handles
   and prepared statements per `realmID`; charserver queries `characters.guid`
   and joins `guild_member.guid` by realm DB in
   `apps/charserver/repo/characters_mysql.go`; groupserver writes/reads
   `groups.guid`, `groups.leaderGuid`, `groups.looterGuid`,
   `groups.masterLooterGuid`, and `group_member.memberGuid` through
   realm-scoped prepared statements in `apps/groupserver/repo/stmt.go`;
   guildserver writes/reads `guild.guildid`, `guild.leaderguid`, and
   `guild_member.guid` through realm-scoped statements in
   `apps/guildserver/repo/stmts.go` and `apps/guildserver/repo/guilds_mysql.go`;
   ToCloud9 migrations create `guild_invites.charGuid`, `guild_invites.guildId`,
   `group_invites.invited`, `group_invites.inviter`, and
   `group_invites.groupId` as `int unsigned`. `channels_members.playerGUID` is
   `BIGINT UNSIGNED`, but chat/char caches still key it with explicit
   `realmID`, so it is not evidence of a packed raw `ObjectGuid` contract by
   itself.

5. **Guild creation is intended to enter AzerothCore/worldserver through the
   worldserver/libsidecar gRPC API, not guildserver `CreateGuild`, and ToCloud9
   does not show a petition workflow implementation for creation.**
   Evidence: `api/proto/v1/worldserver/worldserver.proto::WorldServerService`
   declares `CreateGuild`; `game-server/libsidecar/grpc-api.go` registers
   `CreateGuild: CreateGuildHandler`;
   `game-server/libsidecar/guild-api.go::CreateGuildHandler` calls the C
   `GuildCreateHandler`; `game-server/libsidecar/grpcapi/server-guild.go` maps
   C handler error codes to gRPC `CreateGuildResponse` statuses. In contrast,
   `api/proto/v1/guilds/guilds.proto::GuildService` has invite, roster, rank,
   note, message, leave, and kick RPCs, but no `CreateGuild` RPC.

6. **The current ToCloud9 implementation still supports transitional
   worldserver-side member-state publication, but the target architecture should
   replace continuous HP/power publication with gateway-driven snapshots once a
   gateway extraction contract exists.**
   Evidence: `game-server/libsidecar/events-group.go::TC9UpdateGroupMemberState`
   synchronously sends percentages to groupserver today, and groupserver/gateway
   consume `GroupEventMemberStateChangedPayload` to render remote party member
   stats. However, no inspected ToCloud9 gateway code extracts HP/power/zone
   snapshots from object updates or publishes bulk player-state snapshots. Until
   that future contract exists, removing all worldserver-side publication would
   likely regress remote party/raid frames.

### Remaining unresolved items

1. **No original open question remains unresolved by the ToCloud9 reference
   audit.** The current service-side contracts are identifiable in the reference
   repository. The authoritative c-shared source for the current ABI is
   `game-server/libsidecar`; `game-server/libsidecar-cpp` is a separate C++
   implementation whose public group header lacks the newer group-extra hook
   structs and should not be used to infer the current Go c-shared ABI.
2. **Gateway-driven bulk player-state snapshots remain future work, not a hidden
   existing contract.** The ToCloud9 repo has gateway packet handling and group
   event rendering, but no proven object-update extraction, snapshot store, bulk
   flush event, or groupserver routing contract for that design.

## Implementation log

- Audited local `AGENTS.md` instructions.
- Confirmed repository branch/status with `git status --short --branch`.
- Searched local custom cluster paths with `rg` across `src/server/game` and
  `deps/libsidecar`.
- Inspected sidecar CMake, real/stub headers, stub C implementation, TC9 sidecar,
  group hooks, guild hooks, group handlers, instance handlers, GUID helpers,
  player/item creation paths, and hot member-state hooks.
- Did not modify production code.
- Reordered implementation milestones per user priority: build/libsidecar ABI,
  GUID boundaries, transitional live member-state hardening, discrete group
  features, instance features, guild creation, then future design-only work.
- ToCloud9 reference audit proved that the current ToCloud9 Go c-shared
  libsidecar and groupserver use the 8-argument percentage member-state ABI.
- The reference clone was read-only at `/tmp/tocloud9-reference` commit
  `aa66960`.
- Updated this plan with verified ToCloud9 source/proto/SQL answers; no
  production code, gameplay code, generated real headers, stubs, or the
  pre-existing modified `deps/libsidecar/libsidecar.so` were modified.

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
sed -n '1,260p' CLUSTER_PLAN.md
sed -n '261,520p' CLUSTER_PLAN.md
sed -n '190,340p' CLUSTER_PLAN.md
sed -n '340,620p' CLUSTER_PLAN.md
python3 - <<'PY'
# updated only the CLUSTER_PLAN.md milestones/log text
PY
rg -n "^### Phase|Mandatory ToCloud9|gateway-driven|transitional|worldserver-side|Did not modify production code|Reordered implementation" CLUSTER_PLAN.md
git diff --check -- CLUSTER_PLAN.md
```

Commands run during Phase 1 implementation:

```bash
git diff --stat
git diff --check -- deps/libsidecar/stub/libsidecar.h deps/libsidecar/stub/libsidecar.c CLUSTER_PLAN.md
cmake -S . -B build-phase1-stub -DUSE_REAL_LIBSIDECAR=OFF -DSCRIPTS=static -DMODULES=static -DBUILD_TESTING=OFF
cc -std=c11 -Ideps/libsidecar/stub -fsyntax-only deps/libsidecar/stub/libsidecar.c
cc -std=c11 -Ideps/libsidecar/stub -fsyntax-only deps/libsidecar/stub/events-group.c
diff -u <(rg -n "TC9UpdateGroupMemberState" deps/libsidecar/include/libsidecar.h) <(rg -n "TC9UpdateGroupMemberState" deps/libsidecar/stub/libsidecar.h)
stat -c '%n %s bytes' deps/libsidecar/libsidecar.so
readelf -h deps/libsidecar/libsidecar.so || true
nm -D deps/libsidecar/libsidecar.so | rg 'TC9UpdateGroupMemberState|TC9SetOnGroupReady|TC9SetGuildCreateHandler' || true
```

Phase 1 results:

- `git diff --check` passed for the Phase 1 text/stub changes.
- Stub syntax checks passed for `deps/libsidecar/stub/libsidecar.c` and
  `deps/libsidecar/stub/events-group.c`.
- The first Phase 1 real/stub header comparison had no diff after aligning the
  stub to the bundled real header; the later ToCloud9 audit proved this matched
  the wrong source of truth because the service-side Go c-shared ABI is
  percentage-based.
- Stub CMake configure reached dependency discovery and reported
  `Use stub for libsidecar: Yes`, then failed because Boost development
  components are missing in this environment.
- `deps/libsidecar/libsidecar.so` remains a pre-existing modified/corrupt
  working-tree artifact. `readelf` still reports section headers past EOF and
  `nm -D` still cannot read dynamic symbols. Regenerate it from ToCloud9 with:

```bash
cd /mnt/c/Users/VicenteyMar/ToCloud9
go build -o bin/libsidecar.so -buildmode=c-shared ./game-server/libsidecar/
cp bin/libsidecar.so /mnt/c/Users/VicenteyMar/azerothcore-wotlk/deps/libsidecar/libsidecar.so
sudo cp bin/libsidecar.so /usr/lib/libsidecar.so
```

Commands run during ToCloud9 reference audit:

```bash
git clone --depth 1 https://github.com/VG-prog/ToCloud9 /tmp/tocloud9-reference
cd /tmp/tocloud9-reference && git rev-parse --short HEAD
rg -n "guidserver|Guid|GUID|NextAvailable|CreateGuild|GuildCreate|groupserver|ReadyCheck|SubGroup|Assistant|MainTank|MainAssist|InstanceReset|BindExtension|MemberState|UpdateGroupMemberState|healthPct|powerPct|maxHealth|powerType|NATS|nats|protobuf|proto|gateway|game-load-balancer" apps game-server sql -g '!**/node_modules/**'
rg -n "TC9UpdateGroupMemberState|StartReadyCheck|ReadyCheck|SubGroup|SetMemberFlags|ResetInstance|BindExtension|HealthPct|PowerPct|healthPct|powerPct|maxHealth|powerType" game-server/libsidecar game-server/libsidecar-cpp/include apps/gateway -g '!**/*_test.go'
rg -n "CreateGuild|GuildCreate|create guild|Petition|petition|leaderGuid|GuildName|guild name|NameExists|Leader" apps/guildserver game-server/libsidecar api/proto/v1/guilds sql/characters/mysql shared/events -g '!**/*_test.go'
rg -n "type CharactersDB|DBByRealm|PreparedStatement\(realmID|mapKeyForRealmAndGuid|playerGUID BIGINT|guild_invites|group_invites|channels_members|channels_bans" shared apps/chatserver sql/characters/mysql -g '*.go' -g '*.sql'
git diff --check --cached
git diff --check -- CLUSTER_PLAN.md
```

ToCloud9 reference audit results:

- Direct `git clone` succeeded and resolved ToCloud9 commit `aa66960`.
- Resolved the open GUID, groupserver API/event, member-state percentage vs raw,
  DB identity, guild creation, and transitional member-state questions above.
- Confirmed no production code, gameplay code, generated real headers, or stubs
  were changed; only this plan was updated.
- `git diff --check -- CLUSTER_PLAN.md` passed after the documentation update.

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

- This follow-up audit updates only `CLUSTER_PLAN.md`.
- To roll back this follow-up, revert the `CLUSTER_PLAN.md` documentation change.
- The follow-up did not modify production code, gameplay code, generated real
  headers, stubs, or the pre-existing modified `deps/libsidecar/libsidecar.so`.
