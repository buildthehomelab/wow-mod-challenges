/*
 * mod-challenges: what other modules may ask.
 *
 * A module that hands out items shared between characters (an account bank, an account wallet, an
 * auction of its own) can keep Self-Found characters out with:
 *
 *     #if __has_include("ChallengesApi.h")
 *     #include "ChallengesApi.h"
 *     #endif
 *     ...
 *     #if __has_include("ChallengesApi.h")
 *         if (Challenges::IsSelfFound(player->GetGUID().GetCounter()))
 *             return; // Self-Found: no shared items
 *     #endif
 *
 * An auction of a module's own closes for IsAuctionClosed(guid) instead (Self-Found and Hardcore),
 * and anything that raises experience keeps its multiplier at or below XpRateCap(guid) when that
 * is not 0 (Hardcore and Boaring Adventure: 1x).
 *
 * Released under the MIT License.
 */

#ifndef MOD_CHALLENGES_API_H
#define MOD_CHALLENGES_API_H

#include "ObjectGuid.h"

namespace Challenges
{
    // Whether the character runs the Self-Found challenge, online or not. Safe from any thread.
    bool IsSelfFound(ObjectGuid::LowType guid);

    // Whether the character's challenges keep it away from every auction house, online or not.
    // Safe from any thread.
    bool IsAuctionClosed(ObjectGuid::LowType guid);

    // The highest experience multiplier the character's challenges allow, online or not; 0 when
    // they allow any. Safe from any thread.
    float XpRateCap(ObjectGuid::LowType guid);

    // Whether two characters may mail, trade or group under Hardcore's level rule. A module that
    // sends mail or items between characters itself (a bot's mail command) checks it. Safe from
    // any thread.
    bool InLevelRange(ObjectGuid::LowType a, uint8 levelA, ObjectGuid::LowType b, uint8 levelB);
}

#endif
