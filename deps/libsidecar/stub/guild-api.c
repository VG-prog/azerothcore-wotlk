#include "guild-api.h"

static GuildCreateHandler guildCreateHandler;

void SetGuildCreateHandler(GuildCreateHandler h) {
    guildCreateHandler = h;
}

GuildCreateResponse CallGuildCreateHandler(GuildCreateRequest* request) {
    if (guildCreateHandler == 0) {
        GuildCreateResponse resp = {};
        resp.errorCode = GuildCreateErrorCodeNoHandler;
        return resp;
    }

    return guildCreateHandler(request);
}