# AGENTS.md — AzerothCore + ToCloud9 Cluster Implementation Guide

This file provides durable guidance for Codex when working on AzerothCore
3.3.5a and the ToCloud9 clustering integration. Put this file at the root of
the active repository as `AGENTS.md`.

The goal is not just to make a patch compile. The goal is to finish a robust,
reviewable, testable cluster implementation where multiple worldservers can
serve the same realm and players can interact across worldserver boundaries.

---

## 0. Prime directive

Finish the ToCloud9 cluster integration for AzerothCore with minimal,
well-contained changes to AzerothCore and maximum offloading of cluster logic to
ToCloud9 services.

Prefer this architecture:

1. AzerothCore/worldserver remains authoritative for local gameplay, local map
   simulation, player sessions, object state, and existing packet generation.
2. ToCloud9 services own cluster coordination, cross-worldserver group/guild
   state, routing, registries, GUID services, and cross-server messaging.
3. Gateway services should do as much cross-server presentation work as possible,
   especially for remote group/raid frame data.
4. Avoid high-frequency live-state polling or per-player spam from worldserver.
   If live player state is needed by the cluster, prefer gateway extraction from
   already-produced object update packets, aggregate snapshots, and bulk flushes.
5. Design every step to be restartable, observable, and testable with two or
   more worldservers attached to the same logical realm.

Do not implement speculative rewrites. First prove the current code path with
search evidence, then patch the smallest viable subsystem, then validate.

---

## 1. Operating mode for Codex

### 1.1 Before editing

For any non-trivial task, do this first:

1. Read this `AGENTS.md`.
2. Identify the exact repository and branch.
3. Search the relevant code paths with `rg`.
4. Produce a short evidence summary before editing:
   - files/functions found;
   - current control flow;
   - where the current behavior breaks;
   - what will be changed;
   - what will not be changed.
5. If the task is large, create or update `CLUSTER_PLAN.md` before coding.

Do not guess missing APIs, packet names, proto messages, SQL tables, config keys,
or service contracts. Verify them in the tree.

### 1.2 Required reasoning style

When investigating or reviewing, answer in this structure:

1. Question
2. Relevant Evidence
3. Reasoning Chain
4. Most Likely Conclusion
5. Competing Hypotheses
6. Verification Step
7. Final Recommendation

Keep the reasoning code-local. Cite file paths, functions, structs, messages,
SQL tables, config keys, and commands. Avoid generic advice.

### 1.3 Patch rules

- Make small, reversible patches.
- Preserve upstream style and naming.
- Do not mix unrelated systems in the same patch.
- Do not format entire files unless the task is formatting.
- Do not hide behavior changes inside refactors.
- Do not add dependencies unless explicitly required and justified.
- Do not delete user work.
- Do not use `git reset --hard`, `git clean -fdx`, or destructive migrations.
- Do not modify generated files unless the repo expects that generated output to
  be committed.
- If generated files are needed, identify the generator command.

### 1.4 Communication style

- Code, comments, commit messages, and PR text should be in English.
- User-facing summaries may be in Spanish if the user writes in Spanish.
- Be concise but complete.
- If a build or test cannot be run, say exactly why and provide the exact command
  that should be run locally.

---

## 2. Project context

### 2.1 AzerothCore

AzerothCore is an open-source MMORPG server emulator for World of Warcraft patch
3.3.5a. It is mainly C++, uses CMake, and stores data in MySQL.

Main executables:

- `authserver`: authentication and realm selection.
- `worldserver`: gameplay, sessions, maps, entities, packets, spells, scripts.

Important directories:

- `src/common/`: shared libraries, networking, logging, config, threading,
  collision, utilities.
- `src/server/game/`: core gameplay systems.
- `src/server/game/Entities/`: `Player`, `Creature`, `Unit`, `Item`,
  `GameObject`.
- `src/server/game/Handlers/`: client packet handlers on `WorldSession`.
- `src/server/game/Maps/`: maps, grids, instances.
- `src/server/game/Groups/`: group and raid logic.
- `src/server/game/Guilds/`: guild logic.
- `src/server/game/Server/`: `World`, `WorldSession`, opcodes.
- `src/server/shared/`: shared auth/world definitions and packet support.
- `src/server/scripts/`: scripted content.
- `src/server/database/`: database layer and updater.
- `deps/`: bundled dependencies and ToCloud9 sidecar glue if present.

AzerothCore databases:

- `acore_auth`: accounts, realm list, bans.
- `acore_characters`: characters, inventories, progress.
- `acore_world`: game content.

SQL rules:

- New SQL updates belong in `data/sql/updates/pending_*` under the correct DB
  subdirectory until merged.
- Pending SQL filenames should follow the repository convention.
- Do not edit SQL outside update/pending folders unless explicitly required by
  the repository maintainers.

### 2.2 ToCloud9

ToCloud9 provides clustering services around AzerothCore. Treat ToCloud9 as the
preferred home for cluster coordination.

Known service areas include, but are not limited to:

- authserver
- charserver
- chatserver
- game-load-balancer
- servers-registry
- guidserver
- guildserver
- groupserver
- mailserver
- gateway / game server sidecar integration

Known ToCloud9 locations used in this environment:

- Windows root: `C:\Users\VicenteyMar\ToCloud9`
- WSL root: `/mnt/c/Users/VicenteyMar/ToCloud9`
- WSL bin: `/mnt/c/Users/VicenteyMar/ToCloud9/bin`
- AzerothCore root: `/mnt/c/Users/VicenteyMar/azerothcore-wotlk`

Never suggest `/mnt/c/dev/` for this user.

Important ToCloud9 areas to inspect before editing:

- `apps/groupserver/`
- `apps/guildserver/`
- `apps/*server/`
- `game-server/`
- `game-server/libsidecar/`
- `sql/characters/mysql/`
- `bin/config.yml` and `bin/config.yml.example`
- protobuf / generated API directories, if present
- NATS event definitions and service clients, if present

Do not invent ToCloud9 layout. Search first.

---

## 3. Cluster architecture target

### 3.1 Desired direction

The cluster should not rely on worldserver doing constant live-state fanout for
remote group members.

Preferred high-level data path for group/raid frame updates:

1. Worldserver produces normal object update packets for players it simulates.
2. Gateway observes server-to-client object updates that already pass through it.
3. Gateway extracts relevant player state for connected players:
   - player GUID / raw DB guid;
   - realm/server identity;
   - online state;
   - level;
   - class;
   - zone;
   - map;
   - health percentage;
   - power percentage;
   - optional later: aura state or compact aura hints.
4. Gateway stores latest snapshots in local in-memory storage keyed by player.
5. Gateway flushes latest snapshots in bulk on a fixed cadence, initially around
   every 5 seconds, plus immediate flushes for important transitions if needed.
6. Group service subscribes to bulk player-state events.
7. Group service determines which groups/raids need which member updates.
8. Group service publishes group-scoped update events.
9. Gateway subscribes to group update events and emits the correct client-facing
   packets for local sessions.
10. Worldserver is not involved in the high-frequency remote frame update loop.

This avoids overloading the game event loop and keeps cluster presentation logic
closer to gateway/group services.

### 3.2 AzerothCore responsibilities

AzerothCore/worldserver should still own:

- local player simulation;
- local combat and aura application;
- map/grid/instance simulation;
- local group packet generation when all involved players are local;
- authenticated player sessions;
- original gameplay security checks;
- canonical local object state.

AzerothCore can call sidecar APIs for discrete cluster events when the event
originates in gameplay code and cannot be inferred safely by gateway, such as:

- group creation/disband/member changes;
- party to raid conversion;
- leader/assistant changes;
- ready checks;
- subgroup movement;
- main tank/main assist flags;
- instance reset requests;
- saved instance extension requests;
- guild creation requests;
- mail/cross-service actions, if applicable.

AzerothCore should not do continuous remote group member state broadcasting from
`Player::Update`, `Map::Update`, `World::Update`, or similar high-frequency loops.

### 3.3 ToCloud9 responsibilities

ToCloud9 should own:

- cluster service discovery;
- realm/worldserver/gateway identity;
- cross-worldserver group and guild state;
- NATS event contracts;
- Redis/cache persistence where appropriate;
- routing remote group events to gateways;
- fanout minimization and deduplication;
- resilience when a worldserver or gateway disconnects.

### 3.4 Gateway responsibilities

Gateway should own:

- observing existing traffic where safe;
- extracting presentational state from object updates;
- maintaining latest local player snapshots;
- bulk publishing player state to services;
- receiving group-scoped remote update events;
- sending packet updates to clients connected to that gateway;
- never trusting unauthenticated client input for authoritative gameplay state.

Gateway extraction must be conservative. If a field cannot be decoded reliably,
do not fake it. Add evidence, tests, and logging.

---

## 4. Non-negotiable cluster rules

1. Do not create worldserver tick spam for group member state.
2. Do not send one NATS event per player per small stat change if bulk snapshots
   can solve it.
3. Do not poll the DB for live HP, power, aura, or zone state.
4. Do not block worldserver gameplay threads on remote service calls unless the
   existing architecture already treats that action as synchronous and safe.
5. Do not duplicate source of truth between AzerothCore and ToCloud9 without a
   clear ownership rule.
6. Do not conflate raw DB GUIDs, packed object GUIDs, cross-realm GUIDs, and
   service IDs. Normalize explicitly at service boundaries.
7. Do not assume opcodes, update fields, packet layouts, or protobuf names.
   Verify them in the current source.
8. Do not introduce cluster behavior that breaks single-worldserver mode.
9. Do not make Windows-only assumptions. Cluster mode is expected to run under
   WSL/Linux for AzerothCore even if some ToCloud9 binaries are built on Windows.
10. Do not silently swallow cross-service errors. Log enough context to debug.

---

## 5. Known current integration concerns

The user is actively working on group/raid cluster behavior. Some current or
recent behavior:

- Remote party/raid members can appear online.
- HP/power/aura/live state may not update correctly across worldservers.
- Some implementation attempts moved too much live-state logic into worldserver.
- The preferred redesign is gateway-driven object-update extraction with bulk
  publication to group service.

Treat these as hypotheses to verify in the current branch, not as guaranteed
facts. Search and prove the current code path before patching.

Likely areas to inspect:

```bash
rg -n "TC9|ToCloud9|sidecar|libsidecar|Cluster|group|Group" deps src/server/game apps game-server
rg -n "ReadyCheck|SubGroup|Assistant|MainTank|MainAssist|MemberState|InstanceReset|BindExtension" .
rg -n "NATS|nats|Redis|redis|gateway|Gateway|ObjectUpdate|UpdateObject" .
rg -n "GroupMemberState|PlayerState|healthPct|powerPct|zoneId|mapId|rawGuid|realm" .
```

---

## 6. Implementation phases for the cluster rewrite

Use these phases for large work. Do not jump to phase 5 before phases 0-2 are
proven in the tree.

### Phase 0 — Inventory and proof

Goal: map the current implementation.

Tasks:

- Identify all AzerothCore patches related to ToCloud9.
- Identify all libsidecar headers, stubs, and real implementations.
- Identify all ToCloud9 groupserver/guildserver/gateway event paths.
- Identify current NATS subjects and message schemas.
- Identify current protobuf definitions and generated files.
- Identify current DB migrations.
- Identify where live member state is currently produced and consumed.

Deliverable:

- A short evidence map with file paths and functions.
- A list of broken or overloaded paths.
- A minimal next-phase plan.

### Phase 1 — Contract design

Goal: define stable service contracts before changing behavior.

Tasks:

- Define or locate the existing bulk player-state event.
- Define snapshot identity fields explicitly:
  - realm ID;
  - worldserver ID;
  - gateway ID;
  - player raw DB guid;
  - packed/object GUID if needed;
  - timestamp or sequence.
- Define update fields:
  - online;
  - level;
  - class;
  - zone ID;
  - map ID;
  - health percentage;
  - power percentage.
- Exclude auras initially unless there is already a safe compact contract.
- Decide whether group service stores latest state or only routes updates.
- Define dedupe/throttle behavior.
- Define logs and metrics.

Contract sketch, adapt to existing repo conventions:

```text
BulkPlayerStateUpdated
  sourceGatewayId
  sourceWorldServerId
  realmId
  snapshots[]
    playerGuidRaw
    playerGuidPacked optional
    online
    level
    class
    zoneId
    mapId
    healthPct
    powerPct
    timestampMs
```

Do not copy this blindly if the repo already has a better schema.

### Phase 2 — Gateway extraction

Goal: extract player snapshots from gateway-observed server-to-client object
updates.

Tasks:

- Locate gateway packet parsing for object update packets.
- Locate update-field decoding utilities.
- Extract only fields that are already present and reliably decoded.
- Normalize GUIDs at extraction time.
- Store latest state in a concurrency-safe gateway storage.
- Add debug logs behind existing debug config/log levels.
- Add unit tests if gateway code has test infrastructure.

Rules:

- Never trust client-provided HP/power/position as source of truth.
- Do not mutate original packets during extraction.
- Do not decode more fields than needed.
- If a packet does not contain a field, preserve previous known value only if the
  contract explicitly allows partial updates.

### Phase 3 — Bulk publication

Goal: publish snapshots efficiently.

Tasks:

- Add a periodic flush loop or integrate into existing gateway scheduler.
- Initial cadence: 5 seconds unless existing config says otherwise.
- Publish only changed snapshots when possible.
- Bound batch sizes.
- Include gateway/worldserver/realm identity.
- Add reconnect-safe behavior if NATS is temporarily unavailable.
- Add logs for publish count, snapshot count, and failures.

Rules:

- Avoid per-change event spam.
- Avoid unbounded queues.
- Prefer last-value-wins for HP/power frame state.
- Online/offline transitions may be flushed immediately if needed.

### Phase 4 — Group service routing

Goal: groupserver decides which groups need player-state patches.

Tasks:

- Subscribe to bulk player-state events.
- Maintain or query mapping: player -> group(s).
- For each changed player, find affected group/raid.
- Produce group-scoped member state patches.
- Avoid sending updates to gateways with no interested local recipients.
- Preserve existing group membership authority.
- Add tests around routing, dedupe, offline state, and missing members.

Rules:

- Group service should not require worldserver to tell it HP/power every tick.
- Group service should tolerate out-of-order or stale snapshots.
- Prefer timestamp/sequence checks if available.

### Phase 5 — Gateway client updates

Goal: local clients receive remote member frame updates.

Tasks:

- Locate existing packet builders used for group/raid member stats.
- Verify exact WotLK 3.3.5a opcodes and packet structures in the source.
- Build client-facing updates from group-scoped events.
- Ensure updates only go to sessions that should know that group state.
- Test party and raid UI behavior with players on different worldservers.

Rules:

- Do not invent packet names. Verify in opcode/packet code.
- Do not send private state to non-members.
- Keep single-worldserver behavior unchanged.

### Phase 6 — Discrete group features

Goal: finish non-live-state group features.

Feature checklist:

- ready check start/member response/finish;
- subgroup changes;
- assistant leader;
- main tank/main assist flags;
- role flags;
- party to raid conversion;
- invite/accept/decline/kick;
- offline/online transitions;
- reset instances;
- extend saved instances.

For each feature:

1. Identify current AzerothCore local flow.
2. Identify existing sidecar event/API if any.
3. Decide source of truth.
4. Implement service contract.
5. Implement worldserver hook only if required.
6. Implement gateway/client fanout.
7. Test same-worldserver and cross-worldserver paths.

### Phase 7 — Guild features

Goal: finish crossrealm guild creation and guild state integration.

Feature checklist:

- create guild request;
- leader validation;
- name uniqueness;
- error mapping;
- DB persistence;
- client response packet;
- cross-worldserver visibility.

Known sidecar concepts to verify in the tree:

- `GuildCreateHandler`
- `GuildCreateRequest`
- `GuildCreateResponse`
- create error codes such as name exists and leader not found

Do not assume exact names. Search first.

### Phase 8 — Hardening

Goal: make the system reliable enough for review and QA.

Tasks:

- Add debug logging with correlation IDs where useful.
- Add integration test scripts or manual QA scripts.
- Validate service restarts.
- Validate gateway reconnect.
- Validate worldserver crash/reconnect.
- Validate stale player cleanup.
- Validate no memory growth from snapshot maps.
- Validate single-worldserver mode.
- Validate disabled-cluster mode.

---

## 7. Sidecar/API integration rules

Likely sidecar areas:

- `deps/libsidecar/include/`
- `deps/libsidecar/stub/`
- `game-server/libsidecar/`
- Go c-shared build output: `libsidecar.so`

When adding a sidecar API:

1. Update the public C/C++ header.
2. Update the stub implementation.
3. Update the real Go c-shared implementation.
4. Keep ABI-safe C types at the boundary.
5. Avoid passing C++ ownership across the C ABI.
6. Add null/default behavior for disabled cluster mode.
7. Add logging around failures.
8. Rebuild `libsidecar.so` before linking AzerothCore.

Known sidecar concepts from recent work to verify:

- group ready check events;
- subgroup changed events;
- member flags/roles changed events;
- group member state changed events;
- instance reset request events;
- instance bind extension request events;
- guild create API;
- `TC9UpdateGroupMemberState` or equivalent.

Prefer not to expand sidecar for high-frequency HP/power if gateway extraction can
solve it.

---

## 8. Build and run commands

### 8.1 AzerothCore configure/build under WSL

Skip building unless explicitly requested or needed for verification.

```bash
cd /mnt/c/Users/VicenteyMar/azerothcore-wotlk
mkdir -p build
cd build
cmake .. \
  -DCMAKE_INSTALL_PREFIX=$HOME/azeroth-server \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSCRIPTS=static \
  -DMODULES=static \
  -DUSE_REAL_LIBSIDECAR=ON
make -j$(nproc)
make install
```

### 8.2 Unit tests

```bash
cd /mnt/c/Users/VicenteyMar/azerothcore-wotlk/build
cmake .. -DBUILD_TESTING=ON
make -j$(nproc)
./src/test/unit_tests
ctest --output-on-failure
```

### 8.3 Build ToCloud9 libsidecar under WSL

```bash
cd /mnt/c/Users/VicenteyMar/ToCloud9
go build -o bin/libsidecar.so -buildmode=c-shared ./game-server/libsidecar/
cp bin/libsidecar.so /mnt/c/Users/VicenteyMar/azerothcore-wotlk/deps/libsidecar/libsidecar.so
sudo cp bin/libsidecar.so /usr/lib/libsidecar.so
```

If the linker reports `file too short` for `libsidecar.so`, inspect the file size
and rebuild/copy the real shared object. Do not chmod a broken/truncated file as a
fix.

### 8.4 ToCloud9 services

Discover current build commands from the ToCloud9 repo before changing them.
Known Windows output location:

```text
C:\Users\VicenteyMar\ToCloud9\bin
```

Known service binaries may include:

```text
authserver.exe
charserver.exe
chatserver.exe
game-load-balancer.exe
servers-registry.exe
guidserver.exe
guildserver.exe
groupserver.exe
mailserver.exe
```

Do not assume this list is complete.

### 8.5 Required services/configuration

Cluster testing generally needs:

- MySQL 8.x with AzerothCore schemas.
- Redis running under WSL/Linux.
- NATS server running.
- ToCloud9 `bin/config.yml` copied from example and configured with DB DSNs.
- ToCloud9 character DB migrations applied to `acore_characters`.
- AzerothCore built with real libsidecar enabled.

---

## 9. Code style and PR conventions

### 9.1 C++ style

- 4-space indentation.
- No tabs.
- UTF-8.
- LF line endings.
- Prefer existing AzerothCore style over personal style.
- Avoid braces around single-line statements if the local file follows that
  style.
- Keep lines reasonably short; follow local conventions.
- CI may compile with warnings as errors.

### 9.2 Go style

- Run `gofmt` on changed Go files.
- Keep service contracts explicit.
- Prefer context-aware calls where the repo uses `context.Context`.
- Do not introduce goroutine leaks.
- Bound queues and timers.
- Handle NATS disconnects/reconnects using existing patterns.

### 9.3 Commit messages

Use AzerothCore conventional commit style when preparing commits:

```text
Type(Scope/Subscope): Short description
```

Examples:

```text
fix(Core/Groups): Route cluster ready checks through sidecar
feat(Core/Cluster): Add group member state sidecar hook
fix(DB/Characters): Add ToCloud9 group migration
```

Common types:

- `feat`
- `fix`
- `refactor`
- `style`
- `docs`
- `test`
- `chore`

AI tool usage must be disclosed in PRs if required by the target repository.

---

## 10. Verification matrix

A cluster patch is not done until the relevant rows are checked or explicitly
marked not applicable.

### 10.1 Compile/startup

- AzerothCore builds with cluster disabled/default mode.
- AzerothCore builds with `-DUSE_REAL_LIBSIDECAR=ON`.
- `authserver` starts.
- `worldserver` starts.
- ToCloud9 libsidecar builds.
- ToCloud9 touched services build.
- NATS starts.
- Redis starts.
- DB migrations apply cleanly.

### 10.2 Group/raid behavior

Test with at least two players connected through the cluster, preferably on
different worldservers:

- invite;
- accept;
- leave;
- kick;
- leader change;
- convert party to raid;
- subgroup move;
- assistant leader flag;
- main tank/main assist flag;
- ready check start;
- ready check member response;
- ready check finish;
- online/offline state;
- level/class/zone/map visibility;
- HP percentage update;
- power percentage update;
- no stale ghost member after disconnect;
- no duplicate members after reconnect.

### 10.3 Instance behavior

- reset normal instance;
- reset heroic instance if applicable;
- extend saved instance;
- verify errors when player lacks permission;
- verify cross-worldserver group members receive expected feedback.

### 10.4 Guild behavior

- create guild with local leader;
- create guild with cross-worldserver constraints;
- duplicate name error;
- invalid leader error;
- DB persistence;
- client response correctness.

### 10.5 Regression behavior

- Single-worldserver normal grouping still works.
- Non-cluster build still works.
- Cluster services down: worldserver fails gracefully or disables cluster paths
  according to config.
- No new high-frequency worldserver CPU spike.
- No unbounded gateway/groupserver memory growth.

---

## 11. Logging and observability

For cluster work, add logs only where they help diagnose cross-service flow.
Prefer existing logging facilities and levels.

Useful log fields:

- realm ID;
- worldserver ID;
- gateway ID;
- group GUID;
- player raw GUID;
- event subject/name;
- sequence/timestamp;
- batch size;
- error code;
- remote service name.

Avoid logging private data, credentials, auth tokens, or full DB DSNs.

For noisy paths such as player-state snapshots, logs must be debug-level or
sampled. Do not log every HP/power update at info level.

---

## 12. Data ownership rules

### 12.1 GUIDs

Always distinguish:

- AzerothCore low/raw player DB guid;
- AzerothCore `ObjectGuid` / packed GUID;
- ToCloud9 cross-realm GUID;
- group GUID;
- guild GUID;
- service instance IDs.

At every boundary, name fields explicitly. Do not use a generic `guid` field if
there are multiple possible GUID types.

### 12.2 Live player state

- Authoritative gameplay state comes from worldserver.
- Gateway may extract presentational snapshots from worldserver-produced packets.
- Group service may route snapshots but should not invent gameplay state.
- Clients receive remote presentational state only if they are authorized group or
  raid members.

### 12.3 Persistence

- Persistent membership/guild state must follow existing ToCloud9/AzerothCore DB
  ownership rules.
- Live HP/power snapshot state should normally be ephemeral.
- Do not persist transient HP/power unless there is a proven need.

---

## 13. Anti-patterns to reject

Reject these approaches unless the user explicitly asks for them and the tradeoff
is documented:

- Adding `Player::Update` hooks that publish live state every tick.
- Adding map-wide scans just to find group members for cluster frames.
- Calling remote services synchronously inside hot packet handlers without
  timeout/error strategy.
- Adding DB writes for every HP/power change.
- Sending full group state repeatedly when a compact patch is enough.
- Creating a second, inconsistent group implementation inside AzerothCore.
- Adding packet hacks that only work for one client scenario.
- Treating same-worldserver success as proof that cross-worldserver works.
- Assuming two players are on different worldservers without logging/proving it.
- Adding broad `catch`/empty error handling in services.

---

## 14. Search-first commands

Use `rg` aggressively before editing.

AzerothCore examples:

```bash
cd /mnt/c/Users/VicenteyMar/azerothcore-wotlk
rg -n "class Group|Group::|WorldSession::Handle.*Group|ReadyCheck|SubGroup|Assistant|MainTank|MainAssist" src/server/game
rg -n "SMSG_|CMSG_|Opcode|Party|Raid|Group" src/server/game src/server/shared
rg -n "ObjectGuid|GetGUID|GetGUIDLow|GetGUID().*Counter|PackedGuid" src/server/game deps
rg -n "TC9|ToCloud9|sidecar|libsidecar" .
rg -n "ResetInstances|InstanceSave|Bind|Extend" src/server/game
rg -n "Guild::|GuildMgr|HandleGuild|Petition" src/server/game
```

ToCloud9 examples:

```bash
cd /mnt/c/Users/VicenteyMar/ToCloud9
rg -n "groupserver|GroupService|ReadyCheck|SubGroup|Assistant|MainTank|MainAssist" .
rg -n "gateway|Gateway|ObjectUpdate|UpdateObject|SMSG|CMSG|opcode" .
rg -n "NATS|nats|Subscribe|Publish|subject|Redis|redis" .
rg -n "PlayerState|MemberState|healthPct|powerPct|zoneId|mapId|rawGuid|realm" .
rg -n "GuildCreate|CreateGuild|guildserver|GuildService" .
rg -n "migrate|sql/characters|schemaType|config.yml" .
```

---

## 15. `CLUSTER_PLAN.md` protocol

For multi-phase work, create or update `CLUSTER_PLAN.md`. It must be a living
plan and must remain useful if another agent resumes the task later.

Required sections:

```markdown
# CLUSTER_PLAN.md

## Objective

## Current branch/repositories

## Evidence map

## Architecture decision

## Milestones

## Open questions

## Implementation log

## Verification log

## Rollback notes
```

Rules:

- Keep the plan self-contained.
- Update it after each meaningful discovery or patch.
- Record why decisions were made.
- Record commands run and their result.
- Record what remains unverified.
- Do not use the plan as a substitute for code evidence.

---

## 16. Done definition

A task is done only when all are true:

1. The relevant current code paths were inspected.
2. The patch is minimal and follows local style.
3. The source of truth is clear.
4. Single-worldserver behavior is preserved.
5. Cluster-enabled behavior is implemented or the remaining gap is explicitly
   documented.
6. Build/test commands were run, or exact reasons for not running them are given.
7. Manual QA steps are provided for any behavior that cannot be unit tested.
8. Logs are sufficient to debug cross-service failures.
9. No secrets, machine-specific hacks, or destructive commands were added.
10. The final response lists changed files, verification performed, and remaining
    risks.

---

## 17. Useful task prompts

Use these prompts with Codex when starting work.

### 17.1 Inventory current cluster implementation

```text
Using AGENTS.md, inspect the current AzerothCore + ToCloud9 cluster integration.
Do not edit files. Build an evidence map of all group/guild/sidecar/gateway/NATS
paths relevant to remote group member updates, ready checks, subgroup changes,
MT/MA flags, reset instances, saved instance extension, and guild creation.
Return the required reasoning format and propose the smallest next patch.
```

### 17.2 Redesign remote group frame updates

```text
Using AGENTS.md, design and implement the next smallest phase toward gateway-driven
remote group member frame updates. Prefer extracting player state from gateway
object update packets, storing latest snapshots, bulk flushing to groupserver,
routing group-scoped patches from groupserver, and sending client updates from
gateway. Do not add worldserver high-frequency live-state publishing. Update
CLUSTER_PLAN.md and include verification commands.
```

### 17.3 Review a proposed patch

```text
Using AGENTS.md, review the current diff for correctness, cluster architecture,
performance risk, ABI safety, and single-worldserver regression risk. Do not make
changes. Use the required reasoning format. Flag any hot-loop worldserver fanout,
GUID confusion, blocking service calls, missing stubs, missing generated code, or
unverified packet assumptions.
```

### 17.4 Fix a specific failing feature

```text
Using AGENTS.md, fix this failing cluster feature: <describe exact symptom>.
First prove the current control flow with rg and file evidence. Then make the
smallest patch. Do not refactor unrelated systems. Verify same-worldserver and
cross-worldserver behavior, or provide exact manual QA steps.
```
