/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "TC9Sidecar.h"
#include "Config.h"
#include "GameTime.h"
#include "Group.h"
#include "GroupMgr.h"
#include "InstanceSaveMgr.h"
#include "libsidecar.h"
#include "Log.h"
#include "MapMgr.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "TC9GroupHooks.h"
#include "TC9GrpcHandler.h"
#include "TC9GuildHooks.h"
#include "UpdateTime.h"
#include "WorldSessionMgr.h"

#define AVAILABLE_MAPS_ALL_MAPS ""

MonitoringDataCollectorResponse HandleMonitoringRequest();

namespace
{
    constexpr uint64 GROUP_MEMBER_STATE_FLUSH_INTERVAL_MS = 5000;
}

ToCloud9Sidecar* ToCloud9Sidecar::instance()
{
    static ToCloud9Sidecar instance;
    return &instance;
}

ToCloud9Sidecar::ToCloud9Sidecar() : _clusterModeEnabled(false), _isCrossrealm(false)
{
    for (int i = 0; i < MAX_MAP_ID; ++i)
        _assignedMapsByID[i] = false;
}

void ToCloud9Sidecar::Init(uint16 port, int realmId)
{
    _clusterModeEnabled = sConfigMgr->GetOption<bool>("Cluster.Enabled", false);

    if (_clusterModeEnabled)
    {
        uint32* assignedMaps = nullptr;
        int assignedMapsSize = 0;

        _isCrossrealm = sConfigMgr->GetOption<bool>("Cluster.IsCrossrealm", false);

        std::string availableMaps = sConfigMgr->GetOption<std::string>("Cluster.AvailableMaps", AVAILABLE_MAPS_ALL_MAPS);
        TC9InitLib(port, realmId, _isCrossrealm, availableMaps.data(), &assignedMaps, &assignedMapsSize);

        for (int i = 0; i < MAX_MAP_ID; i++)
            _assignedMapsByID[i] = false;

        if (assignedMaps)
        {
            for (int i = 0; i < assignedMapsSize; ++i)
                if (assignedMaps[i] < MAX_MAP_ID)
                    _assignedMapsByID[assignedMaps[i]] = true;

            free(assignedMaps);
        }

        SetupHooks();
        SetupGrpcHandlers();
    }
}

void ToCloud9Sidecar::Deinit()
{
    if (_clusterModeEnabled)
    {
        FlushGroupMemberStateUpdates(true);
        TC9GracefulShutdown();
    }
}

void ToCloud9Sidecar::SetupHooks()
{
    TC9SetOnMapsReassignedHook(&ToCloud9Sidecar::OnMapsReassigned);

    TC9SetOnGuildMemberLeftHook(&ToCloud9GuildHooks::OnGuildMemberLeft);
    TC9SetOnGuildMemberAddedHook(&ToCloud9GuildHooks::OnGuildMemberAdded);
    TC9SetOnGuildMemberRemovedHook(&ToCloud9GuildHooks::OnGuildMemberRemoved);

    TC9SetOnGroupCreatedHook(&ToCloud9GroupHooks::OnGroupCreated);
    TC9SetOnGroupDisbandedHook(&ToCloud9GroupHooks::OnGroupDisbanded);
    TC9SetOnGroupMemberAddedHook(&ToCloud9GroupHooks::OnGroupMemberAdded);
    TC9SetOnGroupMemberRemovedHook(&ToCloud9GroupHooks::OnGroupMemberRemoved);
    TC9SetOnGroupLootTypeChangedHook(&ToCloud9GroupHooks::OnGroupLootTypeChanged);
    TC9SetOnGroupConvertedToRaidHook(&ToCloud9GroupHooks::OnGroupConvertedToRaid);
    TC9SetOnGroupRaidDifficultyChangedHook(&ToCloud9GroupHooks::OnGroupRaidDifficultyChanged);
    TC9SetOnGroupDungeonDifficultyChangedHook(&ToCloud9GroupHooks::OnGroupDungeonDifficultyChanged);

    TC9SetOnGroupReadyCheckStartedHook(&ToCloud9GroupHooks::OnGroupReadyCheckStarted);
    TC9SetOnGroupReadyCheckMemberStateHook(&ToCloud9GroupHooks::OnGroupReadyCheckMemberState);
    TC9SetOnGroupReadyCheckFinishedHook(&ToCloud9GroupHooks::OnGroupReadyCheckFinished);
    TC9SetOnGroupMemberSubGroupChangedHook(&ToCloud9GroupHooks::OnGroupMemberSubGroupChanged);
    TC9SetOnGroupMemberFlagsChangedHook(&ToCloud9GroupHooks::OnGroupMemberFlagsChanged);
    TC9SetOnGroupMemberStateChangedHook(&ToCloud9GroupHooks::OnGroupMemberStateChanged);
    TC9SetOnGroupInstanceResetRequestHook(&ToCloud9GroupHooks::OnGroupInstanceResetRequest);
    TC9SetOnGroupInstanceBindExtensionRequestHook(&ToCloud9GroupHooks::OnGroupInstanceBindExtensionRequest);
}

void ToCloud9Sidecar::SetupGrpcHandlers()
{
    TC9SetGetPlayerItemsByGuidsHandler(&ToCloud9GrpcHandler::GetPlayerItemsByGuids);
    TC9SetRemoveItemsWithGuidsFromPlayerHandler(&ToCloud9GrpcHandler::RemoveItemsWithGuidsFromPlayer);
    TC9SetAddExistingItemToPlayerHandler(&ToCloud9GrpcHandler::AddExistingItemToPlayer);

    TC9SetGetMoneyForPlayerHandler(&ToCloud9GrpcHandler::GetMoneyForPlayer);
    TC9SetModifyMoneyForPlayerHandler(&ToCloud9GrpcHandler::ModifyMoneyForPlayer);

    TC9SetCanPlayerInteractWithGOAndTypeHandler(&ToCloud9GrpcHandler::CanPlayerInteractWithGOAndType);
    TC9SetCanPlayerInteractWithNPCAndFlagsHandler(&ToCloud9GrpcHandler::CanPlayerInteractWithNPCAndFlags);

    TC9SetBattlegroundStartHandler(&ToCloud9GrpcHandler::StartBattleground);
    TC9SetBattlegroundAddPlayersHandler(&ToCloud9GrpcHandler::AddPlayersToBattleground);
    TC9SetCanPlayerJoinBattlegroundQueueHandler(&ToCloud9GrpcHandler::CanPlayerJoinBattlegroundQueue);
    TC9SetCanPlayerTeleportToBattlegroundHandler(&ToCloud9GrpcHandler::CanPlayerTeleportToBattleground);

    TC9SetGuildCreateHandler(&ToCloud9GrpcHandler::CreateGuild);

    TC9SetMonitoringDataCollectorHandler(&HandleMonitoringRequest);
}

void ToCloud9Sidecar::ProcessHooks()
{
    TC9ProcessEventsHooks();
}

void ToCloud9Sidecar::ProcessGrpcOrHttpRequests()
{
    TC9ProcessGRPCOrHTTPRequests();
}

void ToCloud9Sidecar::ProcessAsyncTasks()
{
    _asyncTasksProcessor.ProcessReadyCallbacks();
    FlushGroupMemberStateUpdates();
}

bool ToCloud9Sidecar::IsMapAssigned(uint32 mapId)
{
    if (mapId >= MAX_MAP_ID)
        return false;

    return _assignedMapsByID[mapId];
}

uint64 ToCloud9Sidecar::GenerateCharacterGuid(uint16 realmId)
{
    return uint64(TC9GetNextAvailableCharacterGuid(realmId));
}

uint64 ToCloud9Sidecar::GenerateItemGuid(uint16 realmId)
{
    return uint64(TC9GetNextAvailableItemGuid(realmId));
}

uint32 ToCloud9Sidecar::GenerateInstanceGuid(uint16 realmId)
{
    return uint32(TC9GetNextAvailableInstanceGuid(realmId));
}

void ToCloud9Sidecar::UpdateGroupMemberState(Player* player, bool online)
{
    if (!_clusterModeEnabled || !player)
        return;

    Group* group = player->GetGroup();
    Group* originalGroup = player->GetOriginalGroup();

    // When changing node, the local Player may not yet be attached to the cluster Group.
    // Resolve the group through CharacterCache, because AddMemberWithGuid stores it there.
    if (!group && !originalGroup)
    {
        ObjectGuid cachedGroupGuid = sCharacterCache->GetCharacterGroupGuidByGuid(player->GetGUID());
        if (cachedGroupGuid)
            group = sGroupMgr->GetGroupByGUID(cachedGroupGuid.GetCounter());
    }

    if (!group && !originalGroup)
        return;

    if (group)
        group->RefreshClusterMemberStateFromPlayer(player, online);

    if (originalGroup && originalGroup != group)
        originalGroup->RefreshClusterMemberStateFromPlayer(player, online);

    Powers powerType = player->getPowerType();

    GroupMemberStateSnapshot snapshot;
    snapshot.memberGuid = player->GetGUID().GetDBValue();
    snapshot.online = online ? 1 : 0;
    snapshot.level = player->GetLevel();
    snapshot.playerClass = player->getClass();
    snapshot.zoneId = player->GetZoneId();
    snapshot.mapId = player->GetMapId();
    snapshot.health = uint32(player->GetHealth());
    snapshot.maxHealth = uint32(player->GetMaxHealth());
    snapshot.powerType = uint8(powerType);
    snapshot.power = uint32(player->GetPower(powerType));
    snapshot.maxPower = uint32(player->GetMaxPower(powerType));

    _pendingGroupMemberStates[snapshot.memberGuid] = snapshot;

    LOG_DEBUG("server", "TC9 queued group member state: member={}, online={}, level={}, class={}, zone={}, map={}, health={}, maxHealth={}, powerType={}, power={}, maxPower={}, pending={}",
        snapshot.memberGuid,
        uint32(snapshot.online),
        uint32(snapshot.level),
        uint32(snapshot.playerClass),
        snapshot.zoneId,
        snapshot.mapId,
        snapshot.health,
        snapshot.maxHealth,
        uint32(snapshot.powerType),
        snapshot.power,
        snapshot.maxPower,
        _pendingGroupMemberStates.size());

    if (!online)
        FlushGroupMemberStateUpdates(true);
}

void ToCloud9Sidecar::FlushGroupMemberStateUpdates(bool force)
{
    if (!_clusterModeEnabled || _pendingGroupMemberStates.empty())
        return;

    uint64 const now = GameTime::GetGameTimeMS().count();
    if (!force && _lastGroupMemberStateFlushMs && now < _lastGroupMemberStateFlushMs + GROUP_MEMBER_STATE_FLUSH_INTERVAL_MS)
        return;

    size_t sent = 0;
    size_t skipped = 0;

    for (auto const& pair : _pendingGroupMemberStates)
    {
        GroupMemberStateSnapshot const& snapshot = pair.second;
        auto const last = _lastGroupMemberStates.find(snapshot.memberGuid);
        if (!force && last != _lastGroupMemberStates.end() && snapshot.Equals(last->second))
        {
            ++skipped;
            continue;
        }

        TC9UpdateGroupMemberState(
            snapshot.memberGuid,
            snapshot.online,
            snapshot.level,
            snapshot.playerClass,
            snapshot.zoneId,
            snapshot.mapId,
            snapshot.health,
            snapshot.maxHealth,
            snapshot.powerType,
            snapshot.power,
            snapshot.maxPower
        );

        _lastGroupMemberStates[snapshot.memberGuid] = snapshot;
        ++sent;
    }

    _pendingGroupMemberStates.clear();
    _lastGroupMemberStateFlushMs = now;

    LOG_DEBUG("server", "TC9 flushed group member state batch: sent={}, skipped={}", sent, skipped);
}

void ToCloud9Sidecar::StartGroupReadyCheck(Player* leader, uint32 durationMs)
{
    if (!_clusterModeEnabled || !leader)
        return;

    TC9StartGroupReadyCheck(leader->GetGUID().GetDBValue(), durationMs);
}

void ToCloud9Sidecar::SetGroupReadyCheckMemberState(Player* member, uint8 state)
{
    if (!_clusterModeEnabled || !member)
        return;

    TC9SetGroupReadyCheckMemberState(member->GetGUID().GetDBValue(), state);
}

void ToCloud9Sidecar::FinishGroupReadyCheck(Player* player)
{
    if (!_clusterModeEnabled || !player)
        return;

    TC9FinishGroupReadyCheck(player->GetGUID().GetDBValue());
}

void ToCloud9Sidecar::ChangeGroupMemberSubGroup(Player* updater, ObjectGuid memberGuid, uint8 subGroup)
{
    if (!_clusterModeEnabled || !updater || !memberGuid)
        return;

    TC9ChangeGroupMemberSubGroup(updater->GetGUID().GetDBValue(), memberGuid.GetDBValue(), subGroup);
}

void ToCloud9Sidecar::SetGroupMemberFlags(Player* updater, ObjectGuid memberGuid, uint8 flags, uint8 roles)
{
    if (!_clusterModeEnabled || !updater || !memberGuid)
        return;

    TC9SetGroupMemberFlags(updater->GetGUID().GetDBValue(), memberGuid.GetDBValue(), flags, roles);
}

void ToCloud9Sidecar::OnPlayerLeftBattleground(uint64 playerGUID, uint32 realmID, uint32 instanceID)
{
    TC9PlayerLeftBattleground(playerGUID, realmID, instanceID);
}

void ToCloud9Sidecar::OnBattlegroundStatusChanged(uint32 instanceID, uint8 status)
{
    TC9BattlegroundStatusChanged(instanceID, status);
}

void ToCloud9Sidecar::OnMapsReassigned(uint32* addedMaps, int addedMapsSize, uint32* removedMaps, int removedMapsSize)
{
    for (int i = 0; i < addedMapsSize; i++)
    {
        if (addedMaps[i] >= MAX_MAP_ID)
            continue;

        sToCloud9Sidecar->_assignedMapsByID[addedMaps[i]] = true;

        if (Map *map = sMapMgr->FindBaseNonInstanceMap(addedMaps[i]))
            map->StopPlayersRedirectKickTimer();
    }

    for (int i = 0; i < removedMapsSize; i++)
    {
        if (removedMaps[i] >= MAX_MAP_ID)
            continue;

        sToCloud9Sidecar->_assignedMapsByID[removedMaps[i]] = false;

        if (Map *map = sMapMgr->FindBaseNonInstanceMap(removedMaps[i]))
            map->StartPlayersRedirectKickTimer();
    }

    if (addedMapsSize > 0)
    {
        std::vector<uint32_t> newMapIDs;
        newMapIDs.reserve(addedMapsSize);

        for (int i = 0; i < addedMapsSize; ++i)
            if (addedMaps[i] < MAX_MAP_ID)
                newMapIDs.push_back(addedMaps[i]);

        if (newMapIDs.empty())
            return;

        auto instanceSaveStoragePtr = std::make_shared<InstanceSaveMgr::InstanceSaveHashMap>();
        auto playerBindStoragePtr = std::make_shared<PlayerBindStorage>();

        AsyncTask<bool> task(
           [instanceSaveStoragePtr, playerBindStoragePtr, newMapIDs]() -> bool {
               LOG_INFO("server", "Starting to load data for newly assigned maps...");

               sInstanceSaveMgr->LoadInstanceSavesAndBindsForMapIDs(newMapIDs, *instanceSaveStoragePtr, *playerBindStoragePtr);
               return true;
           },
           [instanceSaveStoragePtr, playerBindStoragePtr, newMapIDs](bool) {
               sInstanceSaveMgr->MergeWithNewInstanceSaves(*instanceSaveStoragePtr, *playerBindStoragePtr);
               TC9ReadyToAcceptPlayersFromMaps((uint32_t*)newMapIDs.data(), newMapIDs.size());

               LOG_INFO("server", "Finished loading data for newly assigned maps.");
           }
        );

        task.ExecuteAsync();
        sToCloud9Sidecar->_asyncTasksProcessor.AddCallback(std::move(task));
    }
}

MonitoringDataCollectorResponse HandleMonitoringRequest()
{
    MonitoringDataCollectorResponse res;
    res.errorCode         = MonitoringErrorCodeNoError;
    res.diffMean          = sWorldUpdateTime.GetAverageUpdateTime();
    res.diffMedian        = sWorldUpdateTime.GetPercentile(50);
    res.diff95Percentile  = sWorldUpdateTime.GetPercentile(95);
    res.diff99Percentile  = sWorldUpdateTime.GetPercentile(99);
    res.diffMaxPercentile = sWorldUpdateTime.GetPercentile(100);
    res.connectedPlayers  = sWorldSessionMgr->GetActiveSessionCount();
    return res;
}
