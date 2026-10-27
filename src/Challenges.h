/*
 * mod-challenges: opt-in challenges a character takes on at the start of its life.
 *
 * A character takes them at the Shrine of Challenge in its starting area, or with .challenge,
 * while it is still at its starting level. Once taken, a challenge can't be put down.
 *
 *  - Hardcore: one life. The first death ends it and is announced; the character then plays on.
 *  - Iron Man: the classic ruleset. Grey and white gear only, no talents, no professions, no
 *    enchants, no potions, elixirs, flasks, scrolls or buff food. A death ends it like Hardcore.
 *  - Self-Found: everything comes from the character itself. No trade, auction house, guild bank,
 *    player mail or groups.
 *  - Self-Crafted: only gear the character made itself can be put on.
 *  - Slow XP: a fraction of the normal experience.
 *  - Quest XP Only: experience from quests only.
 *  - Item Quality Level: grey and white gear only.
 *
 * Only Hardcore and Iron Man end with a death; the others last for the character's life. Most can
 * be combined. The challenges taken and how they ended are kept in the characters table
 * mod_challenges_character.
 *
 * Released under the MIT License.
 */

#ifndef MOD_CHALLENGES_H
#define MOD_CHALLENGES_H

#include "DatabaseEnvFwd.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <string>
#include <vector>

class ChatHandler;
class Group;
class Item;
class Player;
class Unit;
class SpellInfo;
struct ItemTemplate;

namespace Challenges
{
    constexpr uint32 SHRINE_ENTRY = 9503800;   // gameobject_template, spawn guids 9503800-9503808
    constexpr uint32 TEXT_SHRINE = 9503800;
    constexpr uint32 TEXT_RULES = 9503801;
    constexpr uint32 TEXT_FIRST_LOGIN = 9503802;
    constexpr uint32 MENU_FIRST_LOGIN = 9503800;  // runtime gossip menu id of the first-login window

    // Bits, so a character can carry more than one. The values are stored in the database.
    enum Challenge : uint8
    {
        CHALLENGE_HARDCORE      = 0x01,
        CHALLENGE_IRONMAN       = 0x02,
        CHALLENGE_SELF_FOUND    = 0x04,
        CHALLENGE_SELF_CRAFTED  = 0x08,
        CHALLENGE_SLOW_XP       = 0x10,
        CHALLENGE_QUEST_XP_ONLY = 0x20,
        CHALLENGE_ITEM_QUALITY  = 0x40,
    };

    constexpr Challenge ALL_CHALLENGES[] = { CHALLENGE_HARDCORE, CHALLENGE_IRONMAN, CHALLENGE_SELF_FOUND,
        CHALLENGE_SELF_CRAFTED, CHALLENGE_SLOW_XP, CHALLENGE_QUEST_XP_ONLY, CHALLENGE_ITEM_QUALITY };

    // The challenges a death ends.
    constexpr uint8 ENDS_ON_DEATH = CHALLENGE_HARDCORE | CHALLENGE_IRONMAN;

    struct Config
    {
        bool enabled = true;
        uint8 allowed = 0;  // the challenges switched on
        bool announceStart = true;
        bool announceDeath = true;
        bool announceMilestone = true;
        bool loginReminder = true;
        bool firstLoginWindow = true;
        uint32 milestoneLevel = 60;
        bool battlegroundDeaths = false;
        bool ironManGroups = true;
        bool ironManDeathKnights = false;
        std::vector<uint32> ironManExtraSkills;
        bool selfFoundGroups = false;
        uint32 selfFoundMoneyAllowance = 10000;  // copper on top of the start money
        float slowXpRate = 0.5f;
    };

    Config const& GetConfig();
    void LoadConfig();
    void Load();

    char const* Name(Challenge challenge);
    char const* Command(Challenge challenge);
    std::string Rules(Challenge challenge);
    std::string Names(uint8 mask);

    bool IsAllowed(Challenge challenge);
    bool AtStartLevel(Player const* player);

    // The challenges running on the player and enforced by the config.
    uint8 Active(Player const* player);
    bool Has(Player const* player, Challenge challenge);
    // Whether the challenges on the player keep them out of groups. The guid form reads the shared
    // record and is safe from any thread; the Player form is for the player's own thread.
    bool MustPlaySolo(Player const* player);
    bool MustPlaySolo(ObjectGuid::LowType guid);
    // Self-Found by character guid, online or not. Safe from any thread.
    bool IsSelfFound(ObjectGuid::LowType guid);

    // Why the player can't take the challenge right now; empty when they can.
    std::string CanStart(Player* player, Challenge challenge);
    void Start(Player* player, Challenge challenge);

    void HandleDeath(Player* player, Unit* killer);
    void HandleLevel(Player* player);
    void HandleLogin(Player* player);
    void HandleDelete(CharacterDatabaseTransaction trans, ObjectGuid::LowType guid);
    void HandleXP(Player* player, uint32& amount, Unit* victim, uint8 source);
    void SendStatus(ChatHandler* handler, Player* player);

    // Groups that a member who must play solo was added to without an invite (playerbots adds
    // its bots straight to the group). Queued from any thread, settled on the world thread.
    void QueueGroupCheck(Group* group);
    void CheckQueuedGroups();

    // Gear and item rules.
    // Why the challenges on the player forbid wearing the item; nullptr when they don't.
    char const* GearRule(Player const* player, Item const* item);
    bool IsProfessionSkill(uint32 skill);
    bool IsProfessionSpell(SpellInfo const* spell);
    bool IsBannedItemUse(ItemTemplate const* proto);
    // Takes off what Iron Man forbids but could get on without an equip or use: enchants and
    // glyphs that playerbots' "maintenance" puts straight on an alt bot.
    void EnforceIronMan(Player* player);
    void NotifyRule(Player* player, char const* text);
}

#endif
