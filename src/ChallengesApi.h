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
 * Released under the MIT License.
 */

#ifndef MOD_CHALLENGES_API_H
#define MOD_CHALLENGES_API_H

#include "ObjectGuid.h"

namespace Challenges
{
    // Whether the character runs the Self-Found challenge, online or not. Safe from any thread.
    bool IsSelfFound(ObjectGuid::LowType guid);
}

#endif
