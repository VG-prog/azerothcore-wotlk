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

#ifndef _TC9_SIDECAR_H
#define _TC9_SIDECAR_H

#include "AsyncCallbackProcessor.h"
#include "AsyncTask.h"
#include "Common.h"
#include <unordered_map>

#define MAX_MAP_ID 800 // Probably too much, but let's lean towards caution.
#define DEFAULT_NON_CROSSREALM_REALM_ID 0

class Player;
class ObjectGuid;

class ToCloud9Sidecar
{
private:
    ToCloud9Sidecar();
    ~ToCloud9Sidecar() {};

public:
    static ToCloud9Sidecar* instance();

    void Init(uint16 port, int realmId);
    void Deinit();

    bool ClusterModeEnabled() { return _clusterModeEnabled; }
    bool IsCrossrealm() { return _isCrossrealm; }

    bool IsMapAssigned(uint32 mapId);

    void SetupHooks();
    void SetupGrpcHandlers();

    void ProcessHooks();
    void ProcessGrpcOrHttpRequests();
    void ProcessAsyncTasks();

    void UpdateGroupMemberState(Player* player, bool online);
    void FlushGroupMemberStateUpdates(bool force = false);
    void StartGroupReadyCheck(Player* leader, uint32 durationMs);
    void SetGroupReadyCheckMemberState(Player* member, uint8 state);
    void FinishGroupReadyCheck(Player* player);
    void ChangeGroupMemberSubGroup(Player* updater, ObjectGuid memberGuid, uint8 subGroup);
    void SetGroupMemberFlags(Player* updater, ObjectGuid memberGuid, uint8 flags, uint8 roles);

    uint64 GenerateCharacterGuid(uint16 realmId = DEFAULT_NON_CROSSREALM_REALM_ID);
    uint64 GenerateItemGuid(uint16 realmId = DEFAULT_NON_CROSSREALM_REALM_ID);
    uint32 GenerateInstanceGuid(uint16 realmId = DEFAULT_NON_CROSSREALM_REALM_ID);

    void OnPlayerLeftBattleground(uint64 playerGUID, uint32 realmID, uint32 instanceID);
    void OnBattlegroundStatusChanged(uint32 instanceID, uint8 status);

private:
    static void OnMapsReassigned(uint32* addedMaps, int addedMapsSize, uint32* removedMaps, int removedMapsSize);

    bool _clusterModeEnabled;
    bool _isCrossrealm;

    bool _assignedMapsByID[MAX_MAP_ID];

    struct GroupMemberStateSnapshot
    {
        uint64 memberGuid = 0;
        uint8 online = 0;
        uint8 level = 0;
        uint8 playerClass = 0;
        uint32 zoneId = 0;
        uint32 mapId = 0;
        uint32 health = 0;
        uint32 maxHealth = 0;
        uint8 powerType = 0;
        uint32 power = 0;
        uint32 maxPower = 0;

        bool Equals(GroupMemberStateSnapshot const& other) const
        {
            return memberGuid == other.memberGuid
                && online == other.online
                && level == other.level
                && playerClass == other.playerClass
                && zoneId == other.zoneId
                && mapId == other.mapId
                && health == other.health
                && maxHealth == other.maxHealth
                && powerType == other.powerType
                && power == other.power
                && maxPower == other.maxPower;
        }
    };

    std::unordered_map<uint64, GroupMemberStateSnapshot> _pendingGroupMemberStates;
    std::unordered_map<uint64, GroupMemberStateSnapshot> _lastGroupMemberStates;
    uint64 _lastGroupMemberStateFlushMs = 0;

    AsyncCallbackProcessor<AsyncTask<bool>> _asyncTasksProcessor;
};

#define sToCloud9Sidecar ToCloud9Sidecar::instance()

#endif // _TC9_SIDECAR_H
