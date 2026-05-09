//
//  TC9GroupHooks.h
//  game
//
//  Created by Anton Popovichenko on 26/08/2023.
//

#ifndef _TC9_GROUP_HOOKS_H
#define _TC9_GROUP_HOOKS_H

#include "Common.h"
#include "libsidecar.h"

class ToCloud9GroupHooks
{
public:
    ToCloud9GroupHooks() {};
    ~ToCloud9GroupHooks() {};

    static void OnGroupCreated(EventObjectGroup *group);
    static void OnGroupDisbanded(uint32 group);
    static void OnGroupMemberAdded(uint32 group, uint64 member);
    static void OnGroupMemberRemoved(uint32 group, uint64 member, uint64 newLeader);
    static void OnGroupLootTypeChanged(uint32 group, uint8 lootType, uint64 looter, uint8 lootThreshold);
    static void OnGroupConvertedToRaid(uint32 group);
    static void OnGroupRaidDifficultyChanged(uint32 group, uint8 difficulty);
    static void OnGroupDungeonDifficultyChanged(uint32 group, uint8 difficulty);
    static void OnGroupReadyCheckStarted(GroupReadyCheckStarted* request);
    static void OnGroupReadyCheckMemberState(GroupReadyCheckMemberState* request);
    static void OnGroupReadyCheckFinished(GroupReadyCheckFinished* request);
    static void OnGroupMemberSubGroupChanged(GroupMemberSubGroupChanged* request);
    static void OnGroupMemberFlagsChanged(GroupMemberFlagsChanged* request);
    static void OnGroupMemberStateChanged(GroupMemberStateChanged* request);
    static void OnGroupInstanceResetRequest(GroupInstanceResetRequest* request);
    static void OnGroupInstanceBindExtensionRequest(GroupInstanceBindExtensionRequest* request);
};

#endif /* TC9GroupHooks_h */
