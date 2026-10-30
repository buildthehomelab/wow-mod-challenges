/*
 * mod-challenges: opt-in challenges a character takes on at the start of its life.
 *
 * A character takes them at the Shrine of Challenge in its starting area, or with .challenge,
 * while it is still at its starting level. Once taken, a challenge can't be put down.
 *
 *  - Hardcore: one life. The first death ends it and is announced; the character then plays on.
 *    While it runs: mail, trade and groups only within a few levels, no auction house, no Dungeon
 *    Finder, no experience above 1x. Reaching 60, 70 and 80 alive are milestones; from the first
 *    one on the character may finish the challenge, which lifts the rules.
 *  - Iron Man: the classic ruleset. Grey and white gear only, no talents, no professions, no
 *    enchants, no potions, elixirs, flasks, scrolls or buff food. A death ends it like Hardcore.
 *  - Self-Found: everything comes from the character itself. No trade, auction house, guild bank,
 *    player mail or groups.
 *  - Self-Crafted: only gear the character made itself can be put on.
 *  - Slow XP: a fraction of the normal experience.
 *  - Quest XP Only: experience from quests only.
 *  - Item Quality Level: grey and white gear only.
 *  - Boaring Adventure: experience from killing boars only. Herds of boars that only its
 *    characters see fill the levels and zones the world has none for. It finishes by itself at
 *    level 60, with a title and a mount.
 *  - Exhaustion: no rested experience.
 *
 * Only Hardcore and Iron Man end with a death; the others last for the character's life. Most can
 * be combined. The challenges taken and how they ended are kept in the characters table
 * mod_challenges_character.
 *
 * Hardcore earns account-wide achievements, each with a title: one for every milestone reached
 * alive, and one for the level band a death at level 10 or higher ends it in. The account keeps
 * them in mod_challenges_account_reward and every character of it is given them as it loads.
 * Boaring Adventure's titles (the Hambringer, and That Which Has No Life with Exhaustion and Slow XP
 * too) are account-wide the same way.
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
    constexpr uint32 TEXT_MILESTONE = 9503803;
    constexpr uint32 MENU_MILESTONE = 9503801;    // runtime gossip menu id of the milestone window

    // Boaring Adventure: the server-side aura that puts its characters in the herds' phase (bit 26).
    constexpr uint32 SPELL_BOARING_PHASE = 90700;

    // Bits, so a character can carry more than one. The values are stored in the database.
    enum Challenge : uint16
    {
        CHALLENGE_HARDCORE      = 0x01,
        CHALLENGE_IRONMAN       = 0x02,
        CHALLENGE_SELF_FOUND    = 0x04,
        CHALLENGE_SELF_CRAFTED  = 0x08,
        CHALLENGE_SLOW_XP       = 0x10,
        CHALLENGE_QUEST_XP_ONLY = 0x20,
        CHALLENGE_ITEM_QUALITY  = 0x40,
        CHALLENGE_BOARING       = 0x80,
        CHALLENGE_EXHAUSTION    = 0x100,
    };

    constexpr Challenge ALL_CHALLENGES[] = { CHALLENGE_HARDCORE, CHALLENGE_IRONMAN, CHALLENGE_SELF_FOUND,
        CHALLENGE_SELF_CRAFTED, CHALLENGE_SLOW_XP, CHALLENGE_QUEST_XP_ONLY, CHALLENGE_ITEM_QUALITY, CHALLENGE_BOARING,
        CHALLENGE_EXHAUSTION };

    // The challenges a death ends.
    constexpr uint16 ENDS_ON_DEATH = CHALLENGE_HARDCORE | CHALLENGE_IRONMAN;

    // The challenges the character select screen knows: characters.challenges keeps seven bits for
    // the running ones and seven for the ended ones (the core puts bit 23 of its marker, between
    // them, to the favourite star).
    constexpr uint16 SELECT_MARKER_CHALLENGES = 0x7F;

    struct Config
    {
        bool enabled = true;
        uint16 allowed = 0;  // the challenges switched on
        bool announceStart = true;
        bool announceDeath = true;
        bool announceMilestone = true;
        bool loginReminder = true;
        bool firstLoginWindow = true;
        uint32 milestoneLevel = 60;
        std::vector<uint32> hardcoreMilestones;  // ascending
        uint32 hardcoreLevelRange = 5;           // 0 = no level rule
        bool hardcoreAuctionHouse = false;
        bool hardcoreDungeonFinder = false;
        float hardcoreMaxXpRate = 1.0f;          // 0 = no cap
        bool hardcoreRewards = true;
        bool battlegroundDeaths = false;
        bool ironManGroups = true;
        bool ironManDeathKnights = false;
        std::vector<uint32> ironManExtraSkills;
        bool selfFoundGroups = false;
        uint32 selfFoundMoneyAllowance = 10000;  // copper on top of the start money
        float slowXpRate = 0.5f;
        uint32 boaringFinishLevel = 60;          // 0 = it never finishes
        bool boaringRewards = true;
        uint32 boaringMountSpell = 400624;       // Wild Goretusk
        uint32 boaringSwordItem = 9507000;       // Sword of a Thousand Truths
        std::vector<uint32> boaringModels;       // CreatureModelData ids that are boars
    };

    Config const& GetConfig();
    void LoadConfig();
    void Load();

    char const* Name(Challenge challenge);
    char const* Command(Challenge challenge);
    std::string Rules(Challenge challenge);
    std::string Names(uint16 mask);

    bool IsAllowed(Challenge challenge);
    bool AtStartLevel(Player const* player);

    // The challenges running on the player and enforced by the config.
    uint16 Active(Player const* player);
    bool Has(Player const* player, Challenge challenge);
    // Whether the challenges on the player keep them out of groups. The guid form reads the shared
    // record and is safe from any thread; the Player form is for the player's own thread.
    bool MustPlaySolo(Player const* player);
    bool MustPlaySolo(ObjectGuid::LowType guid);
    // Self-Found by character guid, online or not. Safe from any thread.
    bool IsSelfFound(ObjectGuid::LowType guid);
    // Whether the challenges keep the character away from every auction house (Self-Found,
    // Hardcore). The guid form is safe from any thread.
    bool AuctionsClosed(Player const* player);
    bool IsAuctionClosed(ObjectGuid::LowType guid);
    // The highest experience multiplier the character's challenges allow; 0 when any is fine.
    // Safe from any thread.
    float XpRateCap(ObjectGuid::LowType guid);

    // Hardcore's level rule. Why two characters may not mail, trade or group, by guid and level
    // (online or not); empty when they may. Safe from any thread.
    std::string LevelRule(ObjectGuid::LowType a, uint8 levelA, ObjectGuid::LowType b, uint8 levelB);
    // The same for a character joining a group: checked against every member.
    std::string GroupLevelRule(Group const* group, ObjectGuid::LowType guid, uint8 level);
    bool InLevelRange(ObjectGuid::LowType a, uint8 levelA, ObjectGuid::LowType b, uint8 levelB);
    // Whether any member of the group runs Hardcore. Safe from any thread.
    bool GroupHasHardcore(Group const* group);

    // Why the player can't take the challenge right now; empty when they can.
    std::string CanStart(Player* player, Challenge challenge);
    void Start(Player* player, Challenge challenge);
    // Finishing Hardcore once a milestone is reached: the rules lift, the rewards stay.
    std::string CanFinish(Player* player);
    void Finish(Player* player);

    void HandleDeath(Player* player, Unit* killer);
    void HandleLevel(Player* player);
    // Hardcore milestones reached alive (a level-up, or a login after a level set offline).
    // Returns the milestone newly reached, 0 when none.
    uint8 HandleHardcoreLevel(Player* player);
    void HandleLogin(Player* player);
    // While the player loads: gives them the account's achievements quietly.
    void HandleLoad(Player* player);
    void HandleDelete(CharacterDatabaseTransaction trans, ObjectGuid::LowType guid);
    void HandleXP(Player* player, uint32& amount, Unit* victim, uint8 source);
    // Boaring Adventure: whether a kill counts. Boars are the Boar beast family and anything wearing a
    // boar model (Helboars, Felboars, the undead Battle Boars).
    bool IsBoar(Unit const* victim);
    // Boaring Adventure at its finish level: the rewards, and the challenge finishes.
    void HandleBoaringLevel(Player* player);
    // The herds' phase aura on Boaring Adventure characters, and off everyone else.
    void UpdateBoaringPhase(Player* player);
    void SendStatus(ChatHandler* handler, Player* player);

    // Groups that a member who must play solo was added to without an invite (playerbots adds
    // its bots straight to the group). Queued from any thread, settled on the world thread.
    // With a subject (a member that just joined, levelled or logged in), Hardcore's level rule is
    // checked around it too: the subject leaves the group, or, when it leads the group, the
    // members it is out of range of do.
    void QueueGroupCheck(Group* group, ObjectGuid subject = ObjectGuid::Empty);
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
