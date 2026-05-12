#ifndef __EVENT_GROUP__
#define __EVENT_GROUP__

#include <stdint.h>
#include <stdlib.h>

typedef uint32_t TC9RawGroupGuid;
typedef uint64_t TC9RawPlayerGuid;

enum GroupStatus {
    GroupHookStatusOK = 0,
    GroupHookStatusNoHook = 1
};

typedef struct {
    TC9RawGroupGuid guid;
    TC9RawPlayerGuid leader;
    uint8_t lootMethod;
    TC9RawPlayerGuid looterGuid;
    uint8_t lootThreshold;
    uint8_t groupType;
    uint8_t difficulty;
    uint8_t raidDifficulty;
    TC9RawPlayerGuid masterLooterGuid;
    TC9RawPlayerGuid *members;
    uint8_t membersSize;
} EventObjectGroup;

typedef void (*OnGroupCreatedHook) (EventObjectGroup *group);
void SetOnGroupCreatedHook(OnGroupCreatedHook h);
int CallOnGroupCreatedHook(EventObjectGroup *group);

typedef void (*OnGroupMemberAddedHook) (TC9RawGroupGuid guid, TC9RawPlayerGuid newMemberGuid);
void SetOnGroupMemberAddedHook(OnGroupMemberAddedHook h);
int CallOnGroupMemberAddedHook(TC9RawGroupGuid guid, TC9RawPlayerGuid newMemberGuid);

typedef void (*OnGroupMemberRemovedHook) (TC9RawGroupGuid guid, TC9RawPlayerGuid removedMemberGuid, TC9RawPlayerGuid newLeaderGuid);
void SetOnGroupMemberRemovedHook(OnGroupMemberRemovedHook h);
int CallOnGroupMemberRemovedHook(TC9RawGroupGuid guid, TC9RawPlayerGuid removedMemberGuid, TC9RawPlayerGuid newLeaderGuid);

typedef void (*OnGroupDisbandedHook) (TC9RawGroupGuid guid);
void SetOnGroupDisbandedHook(OnGroupDisbandedHook h);
int CallOnGroupDisbandedHook(TC9RawGroupGuid guid);

typedef void (*OnGroupLootTypeChangedHook) (TC9RawGroupGuid guid, uint8_t lootMethod, TC9RawPlayerGuid looter, uint8_t lootThreshold);
void SetOnGroupLootTypeChangedHook(OnGroupLootTypeChangedHook h);
int CallOnGroupLootTypeChangedHook(TC9RawGroupGuid guid, uint8_t lootMethod, TC9RawPlayerGuid looter, uint8_t lootThreshold);

typedef void (*OnGroupDungeonDifficultyChangedHook) (TC9RawGroupGuid guid, uint8_t difficulty);
void SetOnGroupDungeonDifficultyChangedHook(OnGroupDungeonDifficultyChangedHook h);
int CallOnGroupDungeonDifficultyChangedHook(TC9RawGroupGuid guid, uint8_t difficulty);

typedef void (*OnGroupRaidDifficultyChangedHook) (TC9RawGroupGuid guid, uint8_t difficulty);
void SetOnGroupRaidDifficultyChangedHook(OnGroupRaidDifficultyChangedHook h);
int CallOnGroupRaidDifficultyChangedHook(TC9RawGroupGuid guid, uint8_t difficulty);

typedef void (*OnGroupConvertedToRaidHook) (TC9RawGroupGuid guid);
void SetOnGroupConvertedToRaidHook(OnGroupConvertedToRaidHook h);
int CallOnGroupConvertedToRaidHook(TC9RawGroupGuid guid);


typedef struct {
    TC9RawGroupGuid groupGuid;
    TC9RawPlayerGuid leaderGuid;
    uint32_t durationMs;
} GroupReadyCheckStarted;

typedef struct {
    TC9RawGroupGuid groupGuid;
    TC9RawPlayerGuid memberGuid;
    uint8_t state; // 0 = waiting, 1 = ready, 2 = not ready
} GroupReadyCheckMemberState;

typedef struct {
    TC9RawGroupGuid groupGuid;
} GroupReadyCheckFinished;

typedef struct {
    TC9RawGroupGuid groupGuid;
    TC9RawPlayerGuid memberGuid;
    uint8_t subGroup;
} GroupMemberSubGroupChanged;

typedef struct {
    TC9RawGroupGuid groupGuid;
    TC9RawPlayerGuid memberGuid;
    uint8_t flags;
    uint8_t roles;
} GroupMemberFlagsChanged;

typedef struct {
    TC9RawGroupGuid groupGuid;
    TC9RawPlayerGuid memberGuid;
    uint8_t online;
    uint8_t level;
    uint8_t playerClass;
    uint32_t zoneId;
    uint32_t mapId;
    uint32_t health;
    uint32_t maxHealth;
    uint8_t powerType;
    uint32_t power;
    uint32_t maxPower;
} GroupMemberStateChanged;

typedef struct {
    TC9RawGroupGuid groupGuid;
    TC9RawPlayerGuid playerGuid;
    uint32_t mapId;
    uint8_t difficulty;
} GroupInstanceResetRequest;

typedef struct {
    TC9RawGroupGuid groupGuid;
    TC9RawPlayerGuid playerGuid;
    uint32_t mapId;
    uint8_t difficulty;
    uint8_t extended;
} GroupInstanceBindExtensionRequest;

typedef void (*OnGroupReadyCheckStartedHook)(GroupReadyCheckStarted* request);
void SetOnGroupReadyCheckStartedHook(OnGroupReadyCheckStartedHook h);
int CallOnGroupReadyCheckStartedHook(GroupReadyCheckStarted* request);

typedef void (*OnGroupReadyCheckMemberStateHook)(GroupReadyCheckMemberState* request);
void SetOnGroupReadyCheckMemberStateHook(OnGroupReadyCheckMemberStateHook h);
int CallOnGroupReadyCheckMemberStateHook(GroupReadyCheckMemberState* request);

typedef void (*OnGroupReadyCheckFinishedHook)(GroupReadyCheckFinished* request);
void SetOnGroupReadyCheckFinishedHook(OnGroupReadyCheckFinishedHook h);
int CallOnGroupReadyCheckFinishedHook(GroupReadyCheckFinished* request);

typedef void (*OnGroupMemberSubGroupChangedHook)(GroupMemberSubGroupChanged* request);
void SetOnGroupMemberSubGroupChangedHook(OnGroupMemberSubGroupChangedHook h);
int CallOnGroupMemberSubGroupChangedHook(GroupMemberSubGroupChanged* request);

typedef void (*OnGroupMemberFlagsChangedHook)(GroupMemberFlagsChanged* request);
void SetOnGroupMemberFlagsChangedHook(OnGroupMemberFlagsChangedHook h);
int CallOnGroupMemberFlagsChangedHook(GroupMemberFlagsChanged* request);

typedef void (*OnGroupMemberStateChangedHook)(GroupMemberStateChanged* request);
void SetOnGroupMemberStateChangedHook(OnGroupMemberStateChangedHook h);
int CallOnGroupMemberStateChangedHook(GroupMemberStateChanged* request);

typedef void (*OnGroupInstanceResetRequestHook)(GroupInstanceResetRequest* request);
void SetOnGroupInstanceResetRequestHook(OnGroupInstanceResetRequestHook h);
int CallOnGroupInstanceResetRequestHook(GroupInstanceResetRequest* request);

typedef void (*OnGroupInstanceBindExtensionRequestHook)(GroupInstanceBindExtensionRequest* request);
void SetOnGroupInstanceBindExtensionRequestHook(OnGroupInstanceBindExtensionRequestHook h);
int CallOnGroupInstanceBindExtensionRequestHook(GroupInstanceBindExtensionRequest* request);

#endif
