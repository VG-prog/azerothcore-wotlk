//
//  TC9GroupHooks.cpp
//  game
//
//  Created by Anton Popovichenko on 26/08/2023.
//

#include "TC9GroupHooks.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "Player.h"

void ToCloud9GroupHooks::OnGroupCreated(EventObjectGroup *group)
{
    LOG_INFO("server", "Group created. ID: {}; Leader: {}.", group->guid, group->leader);

    Group* g = new Group();
    g->m_guid = ObjectGuid(HighGuid::Group, group->guid);
    g->m_leaderGuid = ObjectGuid(group->leader);
    g->m_dungeonDifficulty = Difficulty(group->difficulty);
    g->m_raidDifficulty = Difficulty(group->raidDifficulty);
    g->m_lootMethod = LootMethod(group->lootMethod);
    g->m_lootThreshold = ItemQualities(group->lootThreshold);
    g->m_masterLooterGuid = ObjectGuid(group->masterLooterGuid);
    g->m_groupType = GroupType(group->groupType);

    for (int i = 0; i < group->membersSize; i++)
        g->AddMemberWithGuid(ObjectGuid(group->members[i]));

    sGroupMgr->AddGroup(g);
}

void ToCloud9GroupHooks::OnGroupDisbanded(uint32 group)
{
    LOG_INFO("server", "Group disbanded. ID: {}.", group);

    if (Group* g = sGroupMgr->GetGroupByGUID(group))
        g->ForcedDisband(true);
}

void ToCloud9GroupHooks::OnGroupMemberAdded(uint32 group, uint64 member)
{
    LOG_INFO("server", "Group member added. ID: {}; Member: {}.", group, member);

    if (Group* g = sGroupMgr->GetGroupByGUID(group))
        g->AddMemberWithGuid(ObjectGuid(member));
}

void ToCloud9GroupHooks::OnGroupMemberRemoved(uint32 group, uint64 member, uint64 newLeader)
{
    LOG_INFO("server", "Group member removed. ID: {}; Member: {}; NewLeader: {}.", group, member, newLeader);

    if (Group* g = sGroupMgr->GetGroupByGUID(group))
        g->RemoveMember(ObjectGuid(member));
}

void ToCloud9GroupHooks::OnGroupLootTypeChanged(uint32 group, uint8 lootType, uint64 looter, uint8 lootThreshold)
{
    LOG_INFO("server", "Group loot type changed. ID: {}; LootType: {}; Looter: {}; LootThreshold: {}.",
             group, lootType, looter, lootThreshold);

    if (Group* g = sGroupMgr->GetGroupByGUID(group))
    {
        g->SetLootMethod((LootMethod)lootType);
        g->SetMasterLooterGuid(ObjectGuid(looter));
        g->SetLootThreshold((ItemQualities)lootThreshold);
    }
}

void ToCloud9GroupHooks::OnGroupConvertedToRaid(uint32 group)
{
    LOG_INFO("server", "Group converted to raid. ID: {}.", group);

    if (Group* g = sGroupMgr->GetGroupByGUID(group))
        g->ConvertToRaid();
}

void ToCloud9GroupHooks::OnGroupRaidDifficultyChanged(uint32 group, uint8 difficulty)
{
    LOG_INFO("server", "Raid difficulty changed. ID: {}; Difficulty: {}.", group, difficulty);

    if (Group* g = sGroupMgr->GetGroupByGUID(group))
        g->SetRaidDifficulty((Difficulty)difficulty);
}

void ToCloud9GroupHooks::OnGroupDungeonDifficultyChanged(uint32 group, uint8 difficulty)
{
    LOG_INFO("server", "Dungeon difficulty changed. ID: {}; Difficulty: {}.", group, difficulty);

    if (Group* g = sGroupMgr->GetGroupByGUID(group))
        g->SetDungeonDifficulty((Difficulty)difficulty);
}

void ToCloud9GroupHooks::OnGroupReadyCheckStarted(GroupReadyCheckStarted* request)
{
    if (!request)
        return;

    Group* group = sGroupMgr->GetGroupByGUID(request->groupGuid);
    if (!group)
        return;

    group->SendClusterReadyCheckStarted(ObjectGuid(request->leaderGuid), request->durationMs);
}

void ToCloud9GroupHooks::OnGroupReadyCheckMemberState(GroupReadyCheckMemberState* request)
{
    if (!request)
        return;

    Group* group = sGroupMgr->GetGroupByGUID(request->groupGuid);
    if (!group)
        return;

    group->SendClusterReadyCheckMemberState(ObjectGuid(request->memberGuid), request->state);
}

void ToCloud9GroupHooks::OnGroupReadyCheckFinished(GroupReadyCheckFinished* request)
{
    if (!request)
        return;

    Group* group = sGroupMgr->GetGroupByGUID(request->groupGuid);
    if (!group)
        return;

    group->SendClusterReadyCheckFinished();
}

void ToCloud9GroupHooks::OnGroupMemberSubGroupChanged(GroupMemberSubGroupChanged* request)
{
    if (!request)
        return;

    Group* group = sGroupMgr->GetGroupByGUID(request->groupGuid);
    if (!group)
        return;

    group->SetClusterMemberSubGroup(ObjectGuid(request->memberGuid), request->subGroup);
}

void ToCloud9GroupHooks::OnGroupMemberFlagsChanged(GroupMemberFlagsChanged* request)
{
    if (!request)
        return;

    Group* group = sGroupMgr->GetGroupByGUID(request->groupGuid);
    if (!group)
        return;

    group->SetClusterMemberFlags(ObjectGuid(request->memberGuid), request->flags, request->roles);
}

void ToCloud9GroupHooks::OnGroupMemberStateChanged(GroupMemberStateChanged* request)
{
    if (!request)
        return;

    Group* group = sGroupMgr->GetGroupByGUID(request->groupGuid);
    if (!group)
        return;

    group->SetClusterMemberState(
        ObjectGuid(request->memberGuid),
        request->online != 0,
        request->level,
        request->playerClass,
        request->zoneId,
        request->mapId,
        request->healthPct,
        request->powerPct
    );
}

void ToCloud9GroupHooks::OnGroupInstanceResetRequest(GroupInstanceResetRequest* request)
{
    if (!request)
        return;

    ObjectGuid playerGuid(request->playerGuid);

    if (Player* player = ObjectAccessor::FindConnectedPlayer(playerGuid))
    {
        Difficulty difficulty = Difficulty(request->difficulty);

        if (InstancePlayerBind* bind = sInstanceSaveMgr->PlayerGetBoundInstance(playerGuid, request->mapId, difficulty))
        {
            if (bind->save)
                sInstanceSaveMgr->PlayerUnbindInstance(playerGuid, request->mapId, difficulty, true, player);
        }

        player->SendRaidInfo();
    }
}

void ToCloud9GroupHooks::OnGroupInstanceBindExtensionRequest(GroupInstanceBindExtensionRequest* request)
{
    if (!request)
        return;

    ObjectGuid playerGuid(request->playerGuid);
    Difficulty difficulty = Difficulty(request->difficulty);

    sInstanceSaveMgr->ClusterSetPlayerBindExtension(
        playerGuid,
        request->mapId,
        difficulty,
        request->extended != 0
    );

    if (Player* player = ObjectAccessor::FindConnectedPlayer(playerGuid))
        player->SendRaidInfo();
}
