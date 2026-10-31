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
 * is not 0 (Hardcore and Boaring Adventure: 1x). Milestones(guid) has a bit (the Challenge
 * values: 1 Hardcore, 2 Iron Man, 4 Self-Found, 8 Self-Crafted, 16 Slow XP, 32 Quest XP Only,
 * 64 Item Quality Level, 128 Boaring Adventure, 256 Exhaustion) for each challenge the character
 * reached its milestone in, so a module can reward it. Running(guid) has the same bits for the
 * challenges the character runs now, for a module with rules of its own (account-wide items that
 * some challenges keep to the character).
 *
 * Released under the MIT License.
 */

#ifndef MOD_CHALLENGES_API_H
#define MOD_CHALLENGES_API_H

#include "ObjectGuid.h"

// Milestones() is here; a module can test this before calling it, so it still builds against an
// older copy of this header.
#define CHALLENGES_API_HAS_MILESTONES
// Running() is here.
#define CHALLENGES_API_HAS_RUNNING

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

    // The challenges the character reached its milestone in, as Challenge bits: level
    // Challenges.MilestoneLevel while running, Hardcore's first milestone alive, or Boaring
    // Adventure finished. Kept when the challenge ends later. Online or not; safe from any thread.
    uint16 Milestones(ObjectGuid::LowType guid);

    // The challenges the character runs now, as Challenge bits (as Milestones has them): not the
    // ended ones, nor those turned off on the realm. Online or not; safe from any thread.
    uint16 Running(ObjectGuid::LowType guid);

    // Whether two characters may mail, trade or group under Hardcore's level rule. A module that
    // sends mail or items between characters itself (a bot's mail command) checks it. Safe from
    // any thread.
    bool InLevelRange(ObjectGuid::LowType a, uint8 levelA, ObjectGuid::LowType b, uint8 levelB);
}

#endif
