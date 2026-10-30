/*
 * mod-challenges: state, opt-in, deaths and the rules.
 *
 * Every row of mod_challenges_character is loaded at startup (one per character and challenge,
 * so the table stays small) and kept in a locked map. A player copies their own entry into
 * Player::CustomData the first time a hook asks, so the hooks that run for every player (bots
 * included) never take the lock. Writes go to the database asynchronously. The account-wide
 * Hardcore and Boaring Adventure achievements (mod_challenges_account_reward) are loaded the
 * same way, per account.
 *
 * Released under the MIT License.
 */

#include "Challenges.h"
#include "ChallengesApi.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DataMap.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Bag.h"
#include "Item.h"
#include "LFGMgr.h"
#include "Log.h"
#include "Mail.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "Tokenize.h"
#include "World.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>
#include <iterator>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

namespace Challenges
{
    namespace
    {
        struct Definition
        {
            Challenge id;
            char const* name;
            char const* command;
            char const* configKey;
        };

        constexpr Definition DEFINITIONS[] =
        {
            { CHALLENGE_HARDCORE,      "Hardcore",           "hardcore",    "Challenges.Hardcore.Enable" },
            { CHALLENGE_IRONMAN,       "Iron Man",           "ironman",     "Challenges.IronMan.Enable" },
            { CHALLENGE_SELF_FOUND,    "Self-Found",         "selffound",   "Challenges.SelfFound.Enable" },
            { CHALLENGE_SELF_CRAFTED,  "Self-Crafted",       "selfcrafted", "Challenges.SelfCrafted.Enable" },
            { CHALLENGE_SLOW_XP,       "Slow XP",            "slowxp",      "Challenges.SlowXP.Enable" },
            { CHALLENGE_QUEST_XP_ONLY, "Quest XP Only",      "questxp",     "Challenges.QuestXPOnly.Enable" },
            { CHALLENGE_ITEM_QUALITY,  "Item Quality Level", "itemquality", "Challenges.ItemQuality.Enable" },
            { CHALLENGE_BOARING,       "Boaring Adventure",  "boaring",     "Challenges.Boaring.Enable" },
            { CHALLENGE_EXHAUSTION,    "Exhaustion",         "exhaustion",  "Challenges.Exhaustion.Enable" },
        };

        Definition const* Find(Challenge challenge)
        {
            for (Definition const& definition : DEFINITIONS)
                if (definition.id == challenge)
                    return &definition;
            return nullptr;
        }

        // Iron Man already holds gear to white and has no professions to craft with. Boaring Adventure
        // and Quest XP Only leave no experience at all between them.
        uint16 Excludes(Challenge challenge)
        {
            switch (challenge)
            {
                case CHALLENGE_IRONMAN:       return CHALLENGE_SELF_CRAFTED | CHALLENGE_ITEM_QUALITY;
                case CHALLENGE_SELF_CRAFTED:  return CHALLENGE_IRONMAN;
                case CHALLENGE_ITEM_QUALITY:  return CHALLENGE_IRONMAN;
                case CHALLENGE_BOARING:       return CHALLENGE_QUEST_XP_ONLY;
                case CHALLENGE_QUEST_XP_ONLY: return CHALLENGE_BOARING;
                default:                      return 0;
            }
        }

        // Class tools that the core teaches as trade skills; they are not professions.
        constexpr uint32 SPELL_RUNEFORGING = 53428;
        constexpr uint32 SPELL_POISONS = 2842;
        constexpr uint32 SPELL_BEAST_TRAINING = 5149;

        // Death Knight runes (Razorice, Fallen Crusader, Cinderglacier, Lichbane, Spellbreaking,
        // Spellshattering, Swordbreaking, Swordshattering, Stoneskin Gargoyle, Nerubian Carapace):
        // the enchant ids the Runeforging spells write.
        constexpr uint32 RUNE_ENCHANTS[] = { 3365, 3366, 3367, 3368, 3369, 3370, 3594, 3595, 3847, 3883 };

        constexpr uint16 GEAR_CHALLENGES = CHALLENGE_IRONMAN | CHALLENGE_SELF_CRAFTED | CHALLENGE_ITEM_QUALITY;

        // Hardcore's account-wide achievements (9260-9270), each with a title (CharTitles 178-188)
        // through achievement_reward. Their rows are in the world SQL and the client patch.
        struct Reward
        {
            uint32 achievement;
            uint8 minLevel;  // reached alive, or the lowest level of a death's band
            uint8 maxLevel;
        };

        constexpr Reward SURVIVAL_REWARDS[] =
        {
            { 9260, 60, 60 },  // the Immortal
            { 9261, 70, 70 },  // the Deathless
            { 9262, 80, 80 },  // the Eternal
        };

        constexpr Reward DEATH_REWARDS[] =
        {
            { 9263, 10, 19 },   // of the Ash
            { 9264, 20, 29 },   // of the Ember
            { 9265, 30, 39 },   // of the Flame
            { 9266, 40, 49 },   // of the Blaze
            { 9267, 50, 59 },   // of the Hellfire
            { 9268, 60, 69 },   // of the Inferno
            { 9269, 70, 79 },   // of the Firestorm
            { 9270, 80, 255 },  // of the Phoenix
        };

        // Boaring Adventure's account-wide achievements: the Hambringer (CharTitles 189) at its finish
        // level, and That Which Has No Life (190) when Exhaustion and Slow XP ran with it.
        constexpr uint32 ACHIEVEMENT_HAMBRINGER = 9271;
        constexpr uint32 ACHIEVEMENT_NO_LIFE = 9272;
        constexpr time_t NOTICE_INTERVAL = 5;
        constexpr uint8 LEARN_DEPTH = 3;

        struct Entry
        {
            uint16 active = 0;     // taken and still running
            uint16 ended = 0;      // taken and ended by a death
            uint16 finished = 0;   // taken and finished (Hardcore after a milestone, Boaring Adventure)
            uint16 milestone = 0;  // reached the milestone level while running
            uint8 hardcoreMilestone = 0;  // the highest Hardcore milestone level reached alive
        };

        // The player's own copy of their Entry. Only touched from the player's map thread.
        struct PlayerState : public DataMap::Base
        {
            Entry entry;
            time_t lastNotice = 0;
        };

        char const* const STATE_KEY = "mod-challenges";

        Config config;
        std::shared_mutex lock;
        std::unordered_map<ObjectGuid::LowType, Entry> entries;
        std::unordered_set<uint32> questStartItems;  // filled at startup, read-only after
        std::unordered_map<uint32, std::unordered_set<uint32>> accountRewards;  // account -> achievements, under lock

        struct GroupCheck
        {
            ObjectGuid group;
            ObjectGuid subject;  // checked for Hardcore's level rule; empty for the solo rule only
        };

        std::mutex groupLock;
        std::vector<GroupCheck> pendingGroups;

        // The Evermore core's characters.challenges column, which its character list sends to the
        // character select screen. Checked at startup; a core without it gets no writes. The INT
        // column has the running challenges in its low 16 bits and the ended ones in its high 16; the
        // SMALLINT one of an older core a byte each, for the first seven challenges only.
        bool selectColumn = false;
        bool selectWide = false;

        uint32 SelectShift()
        {
            return selectWide ? 16 : 8;
        }

        uint32 SelectMask()
        {
            return selectWide ? 0xFFFF : SELECT_MARKER_CHALLENGES;
        }

        PlayerState& State(Player const* player)
        {
            Player* mutablePlayer = const_cast<Player*>(player);
            if (PlayerState* state = mutablePlayer->CustomData.Get<PlayerState>(STATE_KEY))
                return *state;

            PlayerState* state = new PlayerState();
            {
                std::shared_lock<std::shared_mutex> guard(lock);
                auto itr = entries.find(player->GetGUID().GetCounter());
                if (itr != entries.end())
                    state->entry = itr->second;
            }
            mutablePlayer->CustomData.Set(STATE_KEY, state);
            return *state;
        }

        // Applies a change to both the player's copy and the shared map.
        template<typename Fn>
        void Update(Player* player, Fn&& change)
        {
            change(State(player).entry);
            std::unique_lock<std::shared_mutex> guard(lock);
            change(entries[player->GetGUID().GetCounter()]);
        }

        // characters.challenges: the challenges running in the low half, the ones a death ended in the
        // high half, a finished one in both (a finished Hardcore is 0x00010001). Load() writes the same
        // from the table.
        void SaveSelectMarker(Player* player)
        {
            if (!selectColumn)
                return;

            Entry const& entry = State(player).entry;
            uint32 const marker = (uint32(entry.active | entry.finished) & SelectMask()) |
                ((uint32(entry.ended | entry.finished) & SelectMask()) << SelectShift());
            CharacterDatabase.Execute("UPDATE `characters` SET `challenges` = {} WHERE `guid` = {}", marker,
                player->GetGUID().GetCounter());
        }

        uint16 Enforced()
        {
            return config.enabled ? config.allowed : 0;
        }

        uint16 AuctionMask()
        {
            uint16 mask = CHALLENGE_SELF_FOUND;
            if (!config.hardcoreAuctionHouse)
                mask |= CHALLENGE_HARDCORE;
            return mask & Enforced();
        }

        uint16 SoloMask()
        {
            uint16 mask = 0;
            if (!config.selfFoundGroups)
                mask |= CHALLENGE_SELF_FOUND;
            if (!config.ironManGroups)
                mask |= CHALLENGE_IRONMAN;
            return mask & Enforced();
        }

        // The running challenges of a character, online or not, from the shared map.
        uint16 ActiveByGuid(ObjectGuid::LowType guid)
        {
            uint16 const enforced = Enforced();
            if (!enforced)
                return 0;
            std::shared_lock<std::shared_mutex> guard(lock);
            auto itr = entries.find(guid);
            return itr != entries.end() ? itr->second.active & enforced : 0;
        }

        bool HardcoreByGuid(ObjectGuid::LowType guid)
        {
            return (ActiveByGuid(guid) & CHALLENGE_HARDCORE) != 0;
        }

        // The character cache follows every level-up, and knows characters that are offline.
        uint8 CachedLevel(ObjectGuid::LowType guid)
        {
            return sCharacterCache->GetCharacterLevelByGuid(ObjectGuid::Create<HighGuid::Player>(guid));
        }

        bool LevelGap(uint8 a, uint8 b)
        {
            return config.hardcoreLevelRange && uint32(std::abs(int32(a) - int32(b))) > config.hardcoreLevelRange;
        }

        std::string RangeText()
        {
            return Acore::StringFormat("Hardcore: mail, trade and groups only between characters within {} levels of each "
                "other.", config.hardcoreLevelRange);
        }

        LocaleConstant Locale()
        {
            return sWorld->GetDefaultDbcLocale();
        }

        std::string Who(Player* player)
        {
            ChrRacesEntry const* race = sChrRacesStore.LookupEntry(player->getRace());
            ChrClassesEntry const* cls = sChrClassesStore.LookupEntry(player->getClass());
            return Acore::StringFormat("{}, a level {} {} {},", player->GetName(), player->GetLevel(),
                race ? race->name[Locale()] : "", cls ? cls->name[Locale()] : "");
        }

        std::string ZoneName(Player* player)
        {
            AreaTableEntry const* zone = sAreaTableStore.LookupEntry(player->GetZoneId());
            return zone ? zone->area_name[Locale()] : "";
        }

        std::string Tag(uint16 mask)
        {
            return "|cffff4040[" + Names(mask) + "]|r ";
        }

        // The format overload goes through every session; the plain string one needs a session.
        void Announce(std::string const& text)
        {
            ChatHandler(nullptr).SendWorldText("{}", text);
        }

        bool IsBannedEnchant(uint8 slot, uint32 enchantId)
        {
            if (!enchantId || slot == TEMP_ENCHANTMENT_SLOT || slot >= MAX_INSPECTED_ENCHANTMENT_SLOT)
                return false;
            return std::find(std::begin(RUNE_ENCHANTS), std::end(RUNE_ENCHANTS), enchantId) == std::end(RUNE_ENCHANTS);
        }

        // Shirts and tabards are for looks, and no fishing pole can be crafted.
        bool IsSelfCraftedExempt(ItemTemplate const* proto)
        {
            return proto->InventoryType == INVTYPE_BODY || proto->InventoryType == INVTYPE_TABARD ||
                (proto->Class == ITEM_CLASS_WEAPON && proto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE);
        }

        // Calls fn on every item the player owns outside the mailbox: worn, carried, banked and
        // waiting to be bought back.
        template<typename Fn>
        Item* FindItem(Player* player, Fn&& fn)
        {
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < BUYBACK_SLOT_END; ++slot)
            {
                Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                if (!item)
                    continue;
                if (fn(item))
                    return item;
                if (Bag* bag = item->ToBag())
                    for (uint32 bagSlot = 0; bagSlot < bag->GetBagSize(); ++bagSlot)
                        if (Item* inner = bag->GetItemByPos(uint8(bagSlot)))
                            if (fn(inner))
                                return inner;
            }
            return nullptr;
        }

        // Mounts and companions a starting character knows can only have come from its account
        // (modules that share them across characters teach them at the first login), or from a
        // vendor at its own expense. Self-Found gives them back; the account keeps them for its other
        // characters. Class mounts (a Death Knight's Deathcharger) are the character's own.
        uint32 TakeBackSharedCollections(Player* player)
        {
            std::vector<uint32> shared;
            for (auto const& [spellId, spell] : player->GetSpellMap())
            {
                if (spell->State == PLAYERSPELL_REMOVED)
                    continue;
                SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
                for (auto itr = bounds.first; itr != bounds.second; ++itr)
                    if ((itr->second->SkillLine == SKILL_MOUNTS || itr->second->SkillLine == SKILL_COMPANIONS) &&
                        !itr->second->ClassMask)
                    {
                        shared.push_back(spellId);
                        break;
                    }
            }
            for (uint32 spellId : shared)
                player->removeSpell(spellId, SPEC_MASK_ALL, false);
            return uint32(shared.size());
        }

        // Grey and white only for Iron Man and Item Quality Level, and nothing on it that an enchanter
        // or a gem put there for Iron Man. Checking at equip keeps enchants off worn gear: an Iron Man
        // can't enchant, and the trade window's "will not be traded" slot, the only way another player
        // can enchant their items, is refused to them. EnforceIronMan covers the rest. Death Knight
        // runes are the class's own and stay. Self-Crafted takes only what the player made, but for
        // shirts, tabards and fishing poles. Bags and quivers never count.
        char const* GearRule(Player const* player, Item const* item, uint16 mask)
        {
            mask &= GEAR_CHALLENGES;
            if (!mask)
                return nullptr;

            ItemTemplate const* proto = item->GetTemplate();
            if (proto->InventoryType == INVTYPE_BAG || proto->InventoryType == INVTYPE_QUIVER)
                return nullptr;

            if (proto->Quality > ITEM_QUALITY_NORMAL)
            {
                if (mask & CHALLENGE_IRONMAN)
                    return "Iron Man: grey and white gear only.";
                if (mask & CHALLENGE_ITEM_QUALITY)
                    return "Item Quality Level: grey and white gear only.";
            }

            if (mask & CHALLENGE_IRONMAN)
                for (uint8 slot = PERM_ENCHANTMENT_SLOT; slot < MAX_INSPECTED_ENCHANTMENT_SLOT; ++slot)
                    if (IsBannedEnchant(slot, item->GetEnchantmentId(EnchantmentSlot(slot))))
                        return "Iron Man: no enchants or gems on your gear.";

            if ((mask & CHALLENGE_SELF_CRAFTED) && !IsSelfCraftedExempt(proto) &&
                !(proto->HasSignature() && item->GetGuidValue(ITEM_FIELD_CREATOR) == player->GetGUID()))
                return "Self-Crafted: only gear you made yourself.";

            return nullptr;
        }

        std::string MoneyText(uint32 copper)
        {
            std::string text;
            if (copper >= GOLD)
                text += std::to_string(copper / GOLD) + "g ";
            if (copper % GOLD >= SILVER)
                text += std::to_string(copper % GOLD / SILVER) + "s ";
            if (copper % SILVER || text.empty())
                text += std::to_string(copper % SILVER) + "c ";
            text.pop_back();
            return text;
        }

        bool RewardEnabled(uint32 achievementId)
        {
            if (achievementId == ACHIEVEMENT_HAMBRINGER || achievementId == ACHIEVEMENT_NO_LIFE)
                return config.boaringRewards;
            return config.hardcoreRewards;
        }

        // Gives the player one of the account-wide achievements and records it for the whole account.
        // The account's other characters get it as they next load (HandleLoad). A game master in GM
        // mode can't earn achievements; they get it at their next load too.
        void Grant(Player* player, uint32 achievementId)
        {
            if (!RewardEnabled(achievementId))
                return;

            AchievementEntry const* achievement = sAchievementStore.LookupEntry(achievementId);
            if (!achievement)
            {
                LOG_ERROR("module", "mod-challenges: achievement {} is missing; run the module's world SQL", achievementId);
                return;
            }

            uint32 const account = player->GetSession()->GetAccountId();
            bool fresh;
            {
                std::unique_lock<std::shared_mutex> guard(lock);
                fresh = accountRewards[account].insert(achievementId).second;
            }
            if (fresh)
                CharacterDatabase.Execute("INSERT IGNORE INTO `mod_challenges_account_reward` (`account`, `achievement`, `guid`, `level`, "
                    "`earned_at`) VALUES ({}, {}, {}, {}, {})", account, achievementId, player->GetGUID().GetCounter(),
                    uint32(player->GetLevel()), uint32(GameTime::GetGameTime().count()));

            if (!player->HasAchieved(achievementId))
                player->CompletedAchievement(achievement);
        }

        // Every survival achievement up to the player's level, so a level set while offline, or a
        // reward that came after the level, still counts.
        void GrantSurvival(Player* player)
        {
            for (Reward const& reward : SURVIVAL_REWARDS)
                if (player->GetLevel() >= reward.minLevel)
                    Grant(player, reward.achievement);
        }

        void GrantDeath(Player* player)
        {
            uint8 const level = player->GetLevel();
            for (Reward const& reward : DEATH_REWARDS)
                if (level >= reward.minLevel && level <= reward.maxLevel)
                    Grant(player, reward.achievement);
        }

        // Into the bags, or by mail when they are full.
        void GiveItem(Player* player, uint32 itemId, std::string const& subject, std::string const& body)
        {
            if (!sObjectMgr->GetItemTemplate(itemId))
            {
                LOG_ERROR("module", "mod-challenges: item {} is missing; run the module's world SQL", itemId);
                return;
            }

            ItemPosCountVec dest;
            if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, 1) == EQUIP_ERR_OK)
                if (Item* item = player->StoreNewItem(dest, itemId, true))
                {
                    player->SendNewItem(item, 1, true, false);
                    return;
                }

            Item* item = Item::CreateItem(itemId, 1, player);
            if (!item)
                return;
            // From the Shrine of Challenge: a game object's mail can't be returned to a player.
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            item->SaveToDB(trans);
            MailDraft(subject, body).AddItem(item).SendMailTo(trans, MailReceiver(player), MailSender(MAIL_GAMEOBJECT, SHRINE_ENTRY));
            CharacterDatabase.CommitTransaction(trans);
            ChatHandler(player->GetSession()).SendSysMessage("Your bags are full: it waits in your mailbox.");
        }

        uint8 FirstMilestone()
        {
            return config.hardcoreMilestones.empty() ? 0 : uint8(config.hardcoreMilestones.front());
        }

        uint8 StartLevel(Player const* player)
        {
            if (player->getClass() == CLASS_DEATH_KNIGHT)
                return uint8(sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL));
            return uint8(sWorld->getIntConfig(CONFIG_START_PLAYER_LEVEL));
        }

        bool IsProfessionSpell(SpellInfo const* spell, uint8 depth)
        {
            switch (spell->Id)
            {
                case SPELL_RUNEFORGING:
                case SPELL_POISONS:
                case SPELL_BEAST_TRAINING:
                    return false;
                default:
                    break;
            }

            for (SpellEffectInfo const& effect : spell->Effects)
            {
                if (effect.Effect == SPELL_EFFECT_TRADE_SKILL)
                    return true;
                if ((effect.Effect == SPELL_EFFECT_SKILL || effect.Effect == SPELL_EFFECT_SKILL_STEP) &&
                    IsProfessionSkill(uint32(effect.MiscValue)))
                    return true;
                // Trainer entries that teach the real spell.
                if (effect.Effect == SPELL_EFFECT_LEARN_SPELL && depth)
                    if (SpellInfo const* learned = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                        if (learned != spell && IsProfessionSpell(learned, depth - 1))
                            return true;
            }
            return false;
        }
    }

    Config const& GetConfig()
    {
        return config;
    }

    void LoadConfig()
    {
        config.enabled = sConfigMgr->GetOption<bool>("Challenges.Enable", true);
        config.allowed = 0;
        for (Definition const& definition : DEFINITIONS)
            if (sConfigMgr->GetOption<bool>(definition.configKey, true))
                config.allowed |= definition.id;
        config.announceStart = sConfigMgr->GetOption<bool>("Challenges.Announce.Start", true);
        config.announceDeath = sConfigMgr->GetOption<bool>("Challenges.Announce.Death", true);
        config.announceMilestone = sConfigMgr->GetOption<bool>("Challenges.Announce.Milestone", true);
        config.loginReminder = sConfigMgr->GetOption<bool>("Challenges.LoginReminder", true);
        config.firstLoginWindow = sConfigMgr->GetOption<bool>("Challenges.FirstLoginWindow", true);
        config.milestoneLevel = sConfigMgr->GetOption<uint32>("Challenges.MilestoneLevel", 60);
        config.battlegroundDeaths = sConfigMgr->GetOption<bool>("Challenges.BattlegroundDeathsCount", false);
        config.ironManGroups = sConfigMgr->GetOption<bool>("Challenges.IronMan.AllowGroups", true);
        config.ironManDeathKnights = sConfigMgr->GetOption<bool>("Challenges.IronMan.AllowDeathKnights", false);
        config.selfFoundGroups = sConfigMgr->GetOption<bool>("Challenges.SelfFound.AllowGroups", false);
        config.selfFoundMoneyAllowance = sConfigMgr->GetOption<uint32>("Challenges.SelfFound.StartMoneyAllowance", 10000);
        config.slowXpRate = std::clamp(sConfigMgr->GetOption<float>("Challenges.SlowXP.Rate", 0.5f), 0.0f, 1.0f);
        config.boaringFinishLevel = sConfigMgr->GetOption<uint32>("Challenges.Boaring.FinishLevel", 60);
        config.boaringRewards = sConfigMgr->GetOption<bool>("Challenges.Boaring.Rewards", true);
        config.boaringMountSpell = sConfigMgr->GetOption<uint32>("Challenges.Boaring.MountSpell", 400624);
        config.boaringSwordItem = sConfigMgr->GetOption<uint32>("Challenges.Boaring.SwordItem", 9507000);

        config.boaringModels.clear();
        std::string const models = sConfigMgr->GetOption<std::string>("Challenges.Boaring.BoarModels", "66 1631");
        for (std::string_view token : Acore::Tokenize(models, ' ', false))
            if (Optional<uint32> model = Acore::StringTo<uint32>(token))
                config.boaringModels.push_back(*model);

        config.hardcoreLevelRange = sConfigMgr->GetOption<uint32>("Challenges.Hardcore.LevelRange", 5);
        config.hardcoreAuctionHouse = sConfigMgr->GetOption<bool>("Challenges.Hardcore.AllowAuctionHouse", false);
        config.hardcoreDungeonFinder = sConfigMgr->GetOption<bool>("Challenges.Hardcore.AllowDungeonFinder", false);
        config.hardcoreMaxXpRate = std::max(0.0f, sConfigMgr->GetOption<float>("Challenges.Hardcore.MaxXPRate", 1.0f));
        config.hardcoreRewards = sConfigMgr->GetOption<bool>("Challenges.Hardcore.Rewards", true);

        config.hardcoreMilestones.clear();
        std::string const milestones = sConfigMgr->GetOption<std::string>("Challenges.Hardcore.Milestones", "60 70 80");
        for (std::string_view token : Acore::Tokenize(milestones, ' ', false))
            if (Optional<uint32> level = Acore::StringTo<uint32>(token))
                if (*level > 1 && *level <= STRONG_MAX_LEVEL)
                    config.hardcoreMilestones.push_back(*level);
        std::sort(config.hardcoreMilestones.begin(), config.hardcoreMilestones.end());
        config.hardcoreMilestones.erase(std::unique(config.hardcoreMilestones.begin(), config.hardcoreMilestones.end()),
            config.hardcoreMilestones.end());

        config.ironManExtraSkills.clear();
        std::string const extraSkills = sConfigMgr->GetOption<std::string>("Challenges.IronMan.ExtraProfessionSkills", "");
        for (std::string_view token : Acore::Tokenize(extraSkills, ' ', false))
            if (Optional<uint32> skill = Acore::StringTo<uint32>(token))
                config.ironManExtraSkills.push_back(*skill);
    }

    void Load()
    {
        // Without mod_challenges_characters_hardcore.sql the table has no milestone_level, and the query
        // below would fail like an empty table: every challenge would read as not taken.
        bool const milestoneColumn = CharacterDatabase.Query("SHOW COLUMNS FROM `mod_challenges_character` LIKE 'milestone_level'") != nullptr;
        if (!milestoneColumn)
            LOG_ERROR("server.loading", "mod-challenges: mod_challenges_character has no milestone_level column; run "
                "data/sql/db-characters/mod_challenges_characters_hardcore.sql. Hardcore milestones are not loaded.");

        std::unordered_map<ObjectGuid::LowType, Entry> loaded;
        if (QueryResult result = CharacterDatabase.Query("SELECT `guid`, `challenge`, `state`, `milestone_at`, {} "
            "FROM `mod_challenges_character`", milestoneColumn ? "`milestone_level`" : "0"))
        {
            do
            {
                Field* fields = result->Fetch();
                Entry& entry = loaded[fields[0].Get<uint32>()];
                uint16 const challenge = fields[1].Get<uint16>();
                switch (fields[2].Get<uint8>())
                {
                    case 0:  entry.active |= challenge; break;
                    case 2:  entry.finished |= challenge; break;
                    default: entry.ended |= challenge; break;
                }
                if (fields[3].Get<uint32>())
                    entry.milestone |= challenge;
                if (challenge == CHALLENGE_HARDCORE)
                    entry.hardcoreMilestone = fields[4].Get<uint8>();
            } while (result->NextRow());
        }

        // Every character's characters.challenges from the table, which also fills it the first time
        // and puts right anything changed by hand (SaveSelectMarker keeps it after that).
        QueryResult const column = CharacterDatabase.Query("SHOW COLUMNS FROM `characters` LIKE 'challenges'");
        selectColumn = column != nullptr;
        selectWide = selectColumn && column->Fetch()[1].Get<std::string>().rfind("int", 0) == 0;
        if (selectColumn)
            CharacterDatabase.DirectExecute("UPDATE `characters` AS c LEFT JOIN (SELECT `guid`, "
                "BIT_OR(IF(`state` IN (0, 2), `challenge`, 0) & {0}) | (BIT_OR(IF(`state` <> 0, `challenge`, 0) & {0}) << {1}) AS `marker` "
                "FROM `mod_challenges_character` GROUP BY `guid`) AS m ON m.`guid` = c.`guid` "
                "SET c.`challenges` = COALESCE(m.`marker`, 0) WHERE c.`challenges` <> COALESCE(m.`marker`, 0)", SelectMask(), SelectShift());

        std::unordered_map<uint32, std::unordered_set<uint32>> rewards;
        if (QueryResult result = CharacterDatabase.Query("SELECT `account`, `achievement` FROM `mod_challenges_account_reward`"))
        {
            do
            {
                Field* fields = result->Fetch();
                rewards[fields[0].Get<uint32>()].insert(fields[1].Get<uint32>());
            } while (result->NextRow());
        }

        // A potion a quest hands out to be drunk (Potion of Water Breathing for Call of Water) is part
        // of the quest, not a consumable the rules mean.
        questStartItems.clear();
        for (auto const& [questId, quest] : sObjectMgr->GetQuestTemplates())
            if (uint32 item = quest->GetSrcItemId())
                questStartItems.insert(item);

        std::unique_lock<std::shared_mutex> guard(lock);
        entries = std::move(loaded);
        accountRewards = std::move(rewards);
        LOG_INFO("server.loading", ">> mod-challenges: {} characters with a challenge, {} accounts with challenge achievements",
            entries.size(), accountRewards.size());
    }

    char const* Name(Challenge challenge)
    {
        Definition const* definition = Find(challenge);
        return definition ? definition->name : "?";
    }

    char const* Command(Challenge challenge)
    {
        Definition const* definition = Find(challenge);
        return definition ? definition->command : "?";
    }

    std::string Rules(Challenge challenge)
    {
        switch (challenge)
        {
            case CHALLENGE_HARDCORE:
            {
                std::string text = "Hardcore: you have one life. The first death ends the challenge, and the realm hears of it; "
                    "the character then plays on as normal.";
                if (config.hardcoreLevelRange)
                    text += Acore::StringFormat(" You mail, trade and group only with characters within {} levels of you, bots "
                        "included.", config.hardcoreLevelRange);
                if (!config.hardcoreAuctionHouse)
                    text += " No auction house.";
                if (!config.hardcoreDungeonFinder)
                    text += " No Dungeon Finder.";
                if (config.hardcoreMaxXpRate > 0.0f)
                    text += Acore::StringFormat(" Experience at no more than {}x.", config.hardcoreMaxXpRate);
                if (!config.hardcoreMilestones.empty())
                {
                    std::string levels;
                    for (uint32 level : config.hardcoreMilestones)
                        levels += (levels.empty() ? "" : ", ") + std::to_string(level);
                    text += " Reaching level " + levels + " alive is a milestone; from the first one on you may finish the "
                        "challenge with .challenge finish, which lifts the rules.";
                }
                if (config.hardcoreRewards)
                    text += " Titles for your whole account: the Immortal, the Deathless and the Eternal for reaching 60, 70 "
                        "and 80 alive, and one for how far you climbed if you fall at level 10 or higher.";
                return text;
            }
            case CHALLENGE_IRONMAN:
                return "Iron Man: grey and white gear only, unenchanted. No talents or professions. No potions, elixirs, "
                    "flasks, scrolls or buff food. A death ends the challenge.";
            case CHALLENGE_SELF_FOUND:
                return std::string("Self-Found: everything you have, you find yourself. No trading, auction house, Black Market, "
                    "guild bank, player mail, or items, coins, mounts and pets shared across your account.") +
                    (config.selfFoundGroups ? "" : " No groups.") + " Mounts and companions you know now are given back. "
                    "A death doesn't end it.";
            case CHALLENGE_SELF_CRAFTED:
                return "Self-Crafted: you can only put on gear you made yourself. What you wear when you take it stays on; "
                    "bags, quivers, shirts, tabards and fishing poles don't count. A death doesn't end it.";
            case CHALLENGE_SLOW_XP:
                return Acore::StringFormat("Slow XP: you earn {}% of the normal experience. A death doesn't end it.",
                    uint32(config.slowXpRate * 100.0f + 0.5f));
            case CHALLENGE_QUEST_XP_ONLY:
                return "Quest XP Only: experience comes from quests alone. Kills, exploring and battlegrounds give none; "
                    "your pet still learns from your kills. A death doesn't end it.";
            case CHALLENGE_ITEM_QUALITY:
                return "Item Quality Level: grey and white gear only. A death doesn't end it.";
            case CHALLENGE_BOARING:
            {
                std::string text = "Boaring Adventure: you gain experience only from killing boars. Quests, exploring, "
                    "battlegrounds and every other kill give none; your pet still learns from your kills. Herds of boars "
                    "that only boar hunters can see roam the zones and levels the world has none for.";
                if (config.boaringFinishLevel)
                {
                    text += Acore::StringFormat(" At level {} the challenge is done and its rule lifts.", config.boaringFinishLevel);
                    if (config.boaringRewards)
                        text += " You earn the title the Hambringer for your whole account, and a Wild Goretusk to ride. Take "
                            "Exhaustion and Slow XP with it, and the Sword of a Thousand Truths awaits, with the title That "
                            "Which Has No Life.";
                }
                return text + " A death doesn't end it.";
            }
            case CHALLENGE_EXHAUSTION:
                return "Exhaustion: you never gain rested experience. A death doesn't end it.";
        }
        return "";
    }

    std::string Names(uint16 mask)
    {
        std::string text;
        for (Challenge challenge : ALL_CHALLENGES)
            if (mask & challenge)
                text += (text.empty() ? "" : ", ") + std::string(Name(challenge));
        return text;
    }

    bool IsAllowed(Challenge challenge)
    {
        return (Enforced() & challenge) != 0;
    }

    bool AtStartLevel(Player const* player)
    {
        return player->GetLevel() <= StartLevel(player);
    }

    // A challenge switched off in the config keeps its record but no rules.
    uint16 Active(Player const* player)
    {
        uint16 const enforced = Enforced();
        if (!enforced)
            return 0;
        return State(player).entry.active & enforced;
    }

    bool Has(Player const* player, Challenge challenge)
    {
        return (Active(player) & challenge) != 0;
    }

    bool MustPlaySolo(Player const* player)
    {
        return (Active(player) & SoloMask()) != 0;
    }

    bool MustPlaySolo(ObjectGuid::LowType guid)
    {
        return (ActiveByGuid(guid) & SoloMask()) != 0;
    }

    bool IsSelfFound(ObjectGuid::LowType guid)
    {
        return (ActiveByGuid(guid) & CHALLENGE_SELF_FOUND) != 0;
    }

    bool AuctionsClosed(Player const* player)
    {
        return (Active(player) & AuctionMask()) != 0;
    }

    bool IsAuctionClosed(ObjectGuid::LowType guid)
    {
        return (ActiveByGuid(guid) & AuctionMask()) != 0;
    }

    float XpRateCap(ObjectGuid::LowType guid)
    {
        return config.hardcoreMaxXpRate > 0.0f && HardcoreByGuid(guid) ? config.hardcoreMaxXpRate : 0.0f;
    }

    std::string LevelRule(ObjectGuid::LowType a, uint8 levelA, ObjectGuid::LowType b, uint8 levelB)
    {
        if (a == b || !LevelGap(levelA, levelB))
            return "";
        if (!HardcoreByGuid(a) && !HardcoreByGuid(b))
            return "";
        return RangeText();
    }

    bool InLevelRange(ObjectGuid::LowType a, uint8 levelA, ObjectGuid::LowType b, uint8 levelB)
    {
        return LevelRule(a, levelA, b, levelB).empty();
    }

    bool GroupHasHardcore(Group const* group)
    {
        if (!group)
            return false;
        for (Group::MemberSlot const& member : group->GetMemberSlots())
            if (HardcoreByGuid(member.guid.GetCounter()))
                return true;
        return false;
    }

    std::string GroupLevelRule(Group const* group, ObjectGuid::LowType guid, uint8 level)
    {
        if (!group || group->isBGGroup() || group->isBFGroup())
            return "";
        for (Group::MemberSlot const& member : group->GetMemberSlots())
        {
            ObjectGuid::LowType const other = member.guid.GetCounter();
            std::string rule = LevelRule(guid, level, other, CachedLevel(other));
            if (!rule.empty())
                return rule;
        }
        return "";
    }

    std::string CanStart(Player* player, Challenge challenge)
    {
        if (!IsAllowed(challenge))
            return Acore::StringFormat("The {} challenge is not available on this realm.", Name(challenge));

        Entry const& entry = State(player).entry;
        if (entry.active & challenge)
            return Acore::StringFormat("You have already taken the {} challenge.", Name(challenge));
        if (entry.ended & challenge)
            return Acore::StringFormat("Your {} challenge has ended. It can't be taken again on this character.", Name(challenge));
        if (entry.finished & challenge)
            return Acore::StringFormat("You have finished the {} challenge. It can't be taken again on this character.", Name(challenge));
        if (uint16 const clash = entry.active & Excludes(challenge))
            return Acore::StringFormat("{} can't be combined with {}.", Name(challenge), Names(clash));

        if (!AtStartLevel(player))
            return Acore::StringFormat("Challenges can only be taken at level {}.", StartLevel(player));
        if (!player->IsAlive())
            return "Challenges can't be taken while dead.";

        bool const solo = (SoloMask() & challenge) != 0;
        if (solo && player->GetGroup())
            return Acore::StringFormat("Leave your group first: {} allows no groups.", Name(challenge));
        if (solo && sLFGMgr->GetState(player->GetGUID()) != lfg::LFG_STATE_NONE)
            return Acore::StringFormat("Leave the Dungeon Finder first: {} allows no groups.", Name(challenge));

        switch (challenge)
        {
            case CHALLENGE_HARDCORE:
            {
                std::string const rule = GroupLevelRule(player->GetGroup(), player->GetGUID().GetCounter(), player->GetLevel());
                if (!rule.empty())
                    return "Leave your group first. " + rule;
                if (!config.hardcoreDungeonFinder && sLFGMgr->GetState(player->GetGUID()) != lfg::LFG_STATE_NONE)
                    return "Leave the Dungeon Finder first: Hardcore has none.";
                // What another character sent along would come in from outside the level range.
                for (Mail const* mail : player->GetMails())
                    if (mail->messageType == MAIL_NORMAL && mail->sender != player->GetGUID().GetCounter() &&
                        (mail->HasItems() || mail->money) && LevelGap(player->GetLevel(), CachedLevel(mail->sender)))
                        return Acore::StringFormat("Empty your mailbox of what characters more than {} levels away sent you first.",
                            config.hardcoreLevelRange);
                break;
            }
            case CHALLENGE_IRONMAN:
            {
                if (player->getClass() == CLASS_DEATH_KNIGHT && !config.ironManDeathKnights)
                    return "Death Knights can't take the Iron Man challenge.";

                // An item in the trade window's "will not be traded" slot could still be enchanted.
                if (player->GetTradeData())
                    return "Close the trade window first.";

                // resetTalents only clears the active spec; the other would come back on a swap.
                if (player->GetSpecsCount() > 1)
                    return "Iron Man can't be taken with a second talent specialization.";

                for (SkillLineEntry const* skill : sSkillLineStore)
                    if (skill && IsProfessionSkill(skill->id) && player->HasSkill(skill->id))
                        return Acore::StringFormat("Unlearn {} first: Iron Man allows no professions.", skill->name[Locale()]);

                if (uint32 ammo = player->GetUInt32Value(PLAYER_AMMO_ID))
                    if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(ammo))
                        if (IsBannedItemUse(proto))
                            return Acore::StringFormat("Unload {} first: Iron Man allows only grey and white ammo.", proto->Name1);
                break;
            }
            case CHALLENGE_SELF_FOUND:
            {
                if (player->GetTradeData())
                    return "Close the trade window first.";

                // Shared mounts and companions are given back when the challenge starts.
                if (player->IsMounted())
                    return "Dismount first.";
                if (player->GetCritterGUID())
                    return "Dismiss your companion first.";

                // Gold another character handed over can't be told apart once it is in the bags, so
                // a Self-Found character starts with no more than a new character could have.
                uint32 const startMoney = sWorld->getIntConfig(player->getClass() == CLASS_DEATH_KNIGHT ?
                    CONFIG_START_HEROIC_PLAYER_MONEY : CONFIG_START_PLAYER_MONEY);
                if (player->GetMoney() > startMoney + config.selfFoundMoneyAllowance)
                    return Acore::StringFormat("Self-Found can't tell earned gold from gold handed over, so it starts with at "
                        "most {}. Spend or send the rest first.", MoneyText(startMoney + config.selfFoundMoneyAllowance));

                // Nothing another character sent along may come in with the challenge.
                for (Mail const* mail : player->GetMails())
                    if (mail->messageType == MAIL_NORMAL && mail->sender != player->GetGUID().GetCounter() &&
                        (mail->HasItems() || mail->money))
                        return "Empty your mailbox of what other characters sent you first: Self-Found allows nothing shared.";

                if (Item* item = FindItem(player, [](Item* i) { return i->IsBoundAccountWide(); }))
                    return Acore::StringFormat("Get rid of {} first: Self-Found allows nothing shared across your account.",
                        item->GetTemplate()->Name1);
                break;
            }
            case CHALLENGE_SELF_CRAFTED:
                // Heirlooms would grow with the character, which makes the challenge moot.
                for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                    if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                        if (item->IsBoundAccountWide() || item->GetTemplate()->Quality == ITEM_QUALITY_HEIRLOOM)
                            return Acore::StringFormat("Take off {} first: Self-Crafted keeps only the gear you started with.",
                                item->GetTemplate()->Name1);
                break;
            default:
                break;
        }

        // Self-Crafted lets the starting gear stay on, so only the quality rules look at it.
        if (challenge == CHALLENGE_IRONMAN || challenge == CHALLENGE_ITEM_QUALITY)
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    if (char const* rule = GearRule(player, item, challenge))
                        return Acore::StringFormat("Take off {} first. {}", item->GetTemplate()->Name1, rule);

        return "";
    }

    void Start(Player* player, Challenge challenge)
    {
        ObjectGuid::LowType const guid = player->GetGUID().GetCounter();

        Update(player, [challenge](Entry& entry) { entry.active |= challenge; });

        // A talent point spent before the challenge goes back; Iron Man stops new ones.
        if (challenge == CHALLENGE_IRONMAN)
        {
            player->resetTalents(true);
            player->SendTalentsInfoData(false);
        }

        if (challenge == CHALLENGE_SELF_FOUND)
            if (uint32 taken = TakeBackSharedCollections(player))
            {
                // Saved now, so a crash before the next save can't bring them back.
                player->SaveToDB(false, false);
                ChatHandler(player->GetSession()).PSendSysMessage("Self-Found: {} mount(s) and companion(s) were given back. "
                    "Your other characters keep the shared ones.", taken);
            }

        if (challenge == CHALLENGE_BOARING)
            UpdateBoaringPhase(player);
        if (challenge == CHALLENGE_EXHAUSTION)
            player->SetRestBonus(0.0f);

        CharacterDatabase.Execute("REPLACE INTO `mod_challenges_character` (`guid`, `challenge`, `state`, `started_at`, `start_level`) "
            "VALUES ({}, {}, 0, {}, {})", guid, uint32(challenge), uint32(GameTime::GetGameTime().count()), uint32(player->GetLevel()));
        SaveSelectMarker(player);

        ChatHandler(player->GetSession()).PSendSysMessage("|cffff4040{}|r taken. {}", Name(challenge), Rules(challenge));

        if (config.announceStart)
            Announce(Tag(challenge) + player->GetName() + " has taken the " + Name(challenge) + " challenge. Good luck!");
    }

    std::string CanFinish(Player* player)
    {
        if (!Has(player, CHALLENGE_HARDCORE))
            return "You have no Hardcore challenge running.";
        uint8 const first = FirstMilestone();
        if (!first)
            return "Hardcore can't be finished on this realm.";
        if (player->GetLevel() < first)
            return Acore::StringFormat("Hardcore can be finished from level {} on.", first);
        if (!player->IsAlive())
            return "Hardcore can't be finished while dead.";
        return "";
    }

    void Finish(Player* player)
    {
        Update(player, [](Entry& entry)
        {
            entry.active &= ~CHALLENGE_HARDCORE;
            entry.finished |= CHALLENGE_HARDCORE;
        });

        CharacterDatabase.Execute("UPDATE `mod_challenges_character` SET `state` = 2, `ended_at` = {}, `end_level` = {}, `end_map` = {}, "
            "`end_zone` = {} WHERE `guid` = {} AND `challenge` = {} AND `state` = 0", uint32(GameTime::GetGameTime().count()),
            uint32(player->GetLevel()), player->GetMapId(), player->GetZoneId(), player->GetGUID().GetCounter(),
            uint32(CHALLENGE_HARDCORE));
        SaveSelectMarker(player);

        ChatHandler(player->GetSession()).SendSysMessage("|cffff4040Hardcore|r finished. Its rules are lifted, and your titles stay.");
        if (config.announceMilestone)
            Announce(Tag(CHALLENGE_HARDCORE) + player->GetName() + " has finished the Hardcore challenge at level " +
                std::to_string(player->GetLevel()) + ", never having died!");

        LOG_INFO("module", "mod-challenges: {} ({}) finished Hardcore at level {}", player->GetName(), player->GetGUID().GetCounter(),
            player->GetLevel());
    }

    void HandleDeath(Player* player, Unit* killer)
    {
        uint16 const mask = Active(player) & ENDS_ON_DEATH;
        if (!mask)
            return;

        if (!config.battlegroundDeaths && player->GetMap()->IsBattlegroundOrArena())
            return;

        std::string killerName;
        if (killer && killer != player)
        {
            if (Player* owner = killer->GetCharmerOrOwnerPlayerOrPlayerItself())
                killerName = owner == player ? "" : owner->GetName();
            else
                killerName = killer->GetName();
        }

        // While the challenge is still running, so the reward goes with this death.
        if (mask & CHALLENGE_HARDCORE)
            GrantDeath(player);

        Update(player, [mask](Entry& entry)
        {
            entry.active &= ~mask;
            entry.ended |= mask;
        });

        std::string killerSql = killerName;
        CharacterDatabase.EscapeString(killerSql);
        CharacterDatabase.Execute("UPDATE `mod_challenges_character` SET `state` = 1, `ended_at` = {}, `end_level` = {}, "
            "`end_map` = {}, `end_zone` = {}, `end_killer` = '{}' WHERE `guid` = {} AND (`challenge` & {}) <> 0 AND `state` = 0",
            uint32(GameTime::GetGameTime().count()), uint32(player->GetLevel()), player->GetMapId(), player->GetZoneId(),
            killerSql, player->GetGUID().GetCounter(), uint32(mask));
        SaveSelectMarker(player);

        ChatHandler(player->GetSession()).SendSysMessage(Acore::StringFormat("Your {} challenge has ended. "
            "This character plays on without it.", Names(mask)));

        if (config.announceDeath)
        {
            std::string text = Tag(mask) + Who(player);
            text += killerName.empty() ? " has died" : " was slain by " + killerName;
            std::string const zone = ZoneName(player);
            if (!zone.empty())
                text += " in " + zone;
            Announce(text + ".");
        }

        LOG_INFO("module", "mod-challenges: {} ({}) lost {} at level {} (map {}, zone {}, killer '{}')", player->GetName(),
            player->GetGUID().GetCounter(), Names(mask), player->GetLevel(), player->GetMapId(), player->GetZoneId(), killerName);
    }

    void HandleLevel(Player* player)
    {
        if (!config.milestoneLevel || player->GetLevel() < config.milestoneLevel)
            return;

        // Hardcore has milestones of its own (HandleHardcoreLevel), and a Boaring Adventure that finishes
        // is announced by HandleBoaringLevel.
        uint16 mask = Active(player) & ~CHALLENGE_HARDCORE;
        if (config.boaringFinishLevel)
            mask &= ~CHALLENGE_BOARING;
        uint16 const reached = mask & ~State(player).entry.milestone;
        if (!reached)
            return;

        Update(player, [reached](Entry& e) { e.milestone |= reached; });

        CharacterDatabase.Execute("UPDATE `mod_challenges_character` SET `milestone_at` = {} WHERE `guid` = {} AND (`challenge` & {}) <> 0",
            uint32(GameTime::GetGameTime().count()), player->GetGUID().GetCounter(), uint32(reached));

        if (config.announceMilestone)
            Announce(Tag(reached) + player->GetName() + " has reached level " + std::to_string(player->GetLevel()) +
                ((reached & ENDS_ON_DEATH) ? " without dying!" : "!"));
    }

    uint8 HandleHardcoreLevel(Player* player)
    {
        if (!Has(player, CHALLENGE_HARDCORE) || !player->IsAlive())
            return 0;

        GrantSurvival(player);

        uint8 const done = State(player).entry.hardcoreMilestone;
        uint8 reached = 0;
        for (uint32 level : config.hardcoreMilestones)
            if (level > done && level <= player->GetLevel())
                reached = uint8(level);
        if (!reached)
            return 0;

        Update(player, [reached](Entry& entry)
        {
            entry.hardcoreMilestone = reached;
            entry.milestone |= CHALLENGE_HARDCORE;
        });

        uint32 const now = uint32(GameTime::GetGameTime().count());
        CharacterDatabase.Execute("UPDATE `mod_challenges_character` SET `milestone_level` = {}, `milestone_at` = IF(`milestone_at` = 0, {}, "
            "`milestone_at`) WHERE `guid` = {} AND `challenge` = {} AND `state` = 0", uint32(reached), now,
            player->GetGUID().GetCounter(), uint32(CHALLENGE_HARDCORE));

        if (config.announceMilestone)
            Announce(Tag(CHALLENGE_HARDCORE) + player->GetName() + " has reached level " + std::to_string(reached) + " without dying!");

        return reached;
    }

    void HandleLogin(Player* player)
    {
        EnforceIronMan(player);

        // A group from before the rule (saved across a restart, or AllowGroups switched off since),
        // or with members who levelled while this character was away. In a battleground the
        // normal group is the original one.
        ObjectGuid const subject = player->GetGUID();
        if (Group* group = player->GetGroup())
            QueueGroupCheck(group, subject);
        if (Group* group = player->GetOriginalGroup())
            QueueGroupCheck(group, subject);

        UpdateBoaringPhase(player);

        uint16 const mask = Active(player);
        if (!mask || !config.loginReminder)
            return;
        ChatHandler(player->GetSession()).SendSysMessage(Acore::StringFormat("|cffff4040{}|r on this character. "
            "Type .challenge status for the rules.", Names(mask)));
        if ((mask & CHALLENGE_HARDCORE) && CanFinish(player).empty())
            ChatHandler(player->GetSession()).SendSysMessage("You have reached a Hardcore milestone. Type .challenge finish to "
                "finish the challenge, or play on for the next one.");
    }

    // The core sends no "achievement earned" while a player loads, so the account's other characters
    // get their copies without a word to the guild or anyone near.
    void HandleLoad(Player* player)
    {
        if (!config.hardcoreRewards && !config.boaringRewards)
            return;

        std::vector<uint32> owned;
        {
            std::shared_lock<std::shared_mutex> guard(lock);
            auto itr = accountRewards.find(player->GetSession()->GetAccountId());
            if (itr == accountRewards.end())
                return;
            owned.assign(itr->second.begin(), itr->second.end());
        }

        for (uint32 achievementId : owned)
            if (RewardEnabled(achievementId) && !player->HasAchieved(achievementId))
                if (AchievementEntry const* achievement = sAchievementStore.LookupEntry(achievementId))
                    player->CompletedAchievement(achievement);
    }

    // Every path that removes a character for good (client delete, .character erase, the purge of
    // soft-deleted characters) goes through Player::DeleteFromDB; a soft delete doesn't, so a
    // restored character keeps its record.
    void HandleDelete(CharacterDatabaseTransaction trans, ObjectGuid::LowType guid)
    {
        {
            std::unique_lock<std::shared_mutex> guard(lock);
            entries.erase(guid);
        }
        trans->Append("DELETE FROM `mod_challenges_character` WHERE `guid` = {}", guid);
    }

    // The amount is the player's share before rested bonus; the core gives the pet its kill share
    // from the same amount afterwards.
    void HandleXP(Player* player, uint32& amount, Unit* victim, uint8 source)
    {
        if (!amount)
            return;

        // Boaring Adventure's herds stand in a phase of their own. A group member outside it can't see
        // the boar, so takes nothing from it (the core shares group kills by distance alone).
        if (victim && (victim->GetPhaseMask() & PHASE_BOARING) && !player->InSamePhase(victim))
        {
            amount = 0;
            return;
        }

        // Hardcore's cap also covers the core's own experience auras (heirlooms), which are already
        // in the amount: kills carry SPELL_AURA_MOD_XP_PCT, quests SPELL_AURA_MOD_XP_QUEST_PCT.
        if (config.hardcoreMaxXpRate > 0.0f && Has(player, CHALLENGE_HARDCORE))
        {
            float multiplier = 1.0f;
            if (source == PlayerXPSource::XPSOURCE_KILL)
                multiplier = player->GetTotalAuraMultiplier(SPELL_AURA_MOD_XP_PCT);
            else if (source == PlayerXPSource::XPSOURCE_QUEST || source == PlayerXPSource::XPSOURCE_QUEST_DF)
                multiplier = player->GetTotalAuraMultiplier(SPELL_AURA_MOD_XP_QUEST_PCT);
            if (multiplier > config.hardcoreMaxXpRate)
                amount = uint32(std::lround(double(amount) * config.hardcoreMaxXpRate / multiplier));
        }

        uint16 const mask = Active(player) & (CHALLENGE_SLOW_XP | CHALLENGE_QUEST_XP_ONLY | CHALLENGE_BOARING);
        if (!mask || !amount)
            return;

        bool const kill = source == PlayerXPSource::XPSOURCE_KILL && victim;
        bool const quest = source == PlayerXPSource::XPSOURCE_QUEST || source == PlayerXPSource::XPSOURCE_QUEST_DF;
        if (((mask & CHALLENGE_QUEST_XP_ONLY) && !quest) || ((mask & CHALLENGE_BOARING) && !(kill && IsBoar(victim))))
        {
            if (kill)
                if (Pet* pet = player->GetPet())
                    pet->GivePetXP(player->GetGroup() ? amount / 2 : amount);
            amount = 0;
            return;
        }

        if (mask & CHALLENGE_SLOW_XP)
            amount = uint32(amount * config.slowXpRate);
    }

    bool IsBoar(Unit const* victim)
    {
        Creature const* creature = victim ? victim->ToCreature() : nullptr;
        if (!creature)
            return false;
        if (creature->GetCreatureTemplate()->family == CREATURE_FAMILY_BOAR)
            return true;
        CreatureDisplayInfoEntry const* display = sCreatureDisplayInfoStore.LookupEntry(creature->GetNativeDisplayId());
        return display && std::find(config.boaringModels.begin(), config.boaringModels.end(), display->ModelId) !=
            config.boaringModels.end();
    }

    // Also run at login, for a level set while offline.
    void HandleBoaringLevel(Player* player)
    {
        if (!config.boaringFinishLevel || player->GetLevel() < config.boaringFinishLevel || !Has(player, CHALLENGE_BOARING))
            return;

        uint16 const active = Active(player);
        bool const noLife = (active & CHALLENGE_EXHAUSTION) && (active & CHALLENGE_SLOW_XP);
        if (config.boaringRewards)
        {
            Grant(player, ACHIEVEMENT_HAMBRINGER);
            if (noLife)
                Grant(player, ACHIEVEMENT_NO_LIFE);

            if (uint32 const mount = config.boaringMountSpell)
            {
                if (!sSpellMgr->GetSpellInfo(mount))
                    LOG_ERROR("module", "mod-challenges: Boaring Adventure mount spell {} doesn't exist", mount);
                else if (!player->HasSpell(mount))
                    player->learnSpell(mount);
            }
            if (noLife && config.boaringSwordItem)
                GiveItem(player, config.boaringSwordItem, "Sword of a Thousand Truths",
                    "It was foretold, that one day, heroes who could wield the sword might reveal themselves.");
        }

        Update(player, [](Entry& entry)
        {
            entry.active &= ~CHALLENGE_BOARING;
            entry.finished |= CHALLENGE_BOARING;
            entry.milestone |= CHALLENGE_BOARING;
        });
        // The finish goes to the database now; the mount and the sword with it, so a crash before the
        // next save can't lose them.
        if (config.boaringRewards)
            player->SaveToDB(false, false);

        uint32 const now = uint32(GameTime::GetGameTime().count());
        CharacterDatabase.Execute("UPDATE `mod_challenges_character` SET `state` = 2, `milestone_at` = IF(`milestone_at` = 0, {0}, "
            "`milestone_at`), `ended_at` = {0}, `end_level` = {1}, `end_map` = {2}, `end_zone` = {3} WHERE `guid` = {4} AND "
            "`challenge` = {5} AND `state` = 0", now, uint32(player->GetLevel()), player->GetMapId(), player->GetZoneId(),
            player->GetGUID().GetCounter(), uint32(CHALLENGE_BOARING));
        SaveSelectMarker(player);
        UpdateBoaringPhase(player);

        ChatHandler(player->GetSession()).SendSysMessage(noLife ?
            "|cffff4040Boaring Adventure|r finished. You have spent your life killing boars, and the Sword of a Thousand "
            "Truths is yours." : "|cffff4040Boaring Adventure|r finished. Experience comes from anything again.");
        if (config.announceMilestone)
            Announce(Tag(CHALLENGE_BOARING) + player->GetName() + " has reached level " + std::to_string(player->GetLevel()) +
                (noLife ? " killing nothing but boars, and found the Sword of a Thousand Truths!" : " killing nothing but boars!"));

        LOG_INFO("module", "mod-challenges: {} ({}) finished Boaring Adventure at level {}{}", player->GetName(),
            player->GetGUID().GetCounter(), player->GetLevel(), noLife ? " (with Exhaustion and Slow XP)" : "");
    }

    void UpdateBoaringPhase(Player* player)
    {
        bool const boaring = Has(player, CHALLENGE_BOARING);
        if (boaring == player->HasAura(SPELL_BOARING_PHASE))
            return;
        if (!boaring)
        {
            player->RemoveAurasDueToSpell(SPELL_BOARING_PHASE);
            return;
        }
        if (!sSpellMgr->GetSpellInfo(SPELL_BOARING_PHASE))
        {
            LOG_ERROR("module", "mod-challenges: spell {} is missing; run the module's world SQL", SPELL_BOARING_PHASE);
            return;
        }
        player->AddAura(SPELL_BOARING_PHASE, player);
    }

    // Every change of the rest pool goes through Player::SetRestBonus, which holds it to this: an inn,
    // a night offline, a module's rest perk all leave it empty.
    void HandleRestBonusMax(Player* player, float& restBonusMax)
    {
        if (Has(player, CHALLENGE_EXHAUSTION))
            restBonusMax = 0.0f;
    }

    void SendStatus(ChatHandler* handler, Player* player)
    {
        Entry const& entry = State(player).entry;

        for (Challenge challenge : ALL_CHALLENGES)
        {
            if ((entry.active & challenge) && !IsAllowed(challenge))
                handler->PSendSysMessage("{}: taken, but switched off on this realm", Name(challenge));
            else if ((entry.active & challenge) && challenge == CHALLENGE_HARDCORE && entry.hardcoreMilestone)
                handler->PSendSysMessage("{}: |cff40ff40active|r (reached level {} alive; .challenge finish ends it)", Name(challenge),
                    entry.hardcoreMilestone);
            else if (entry.active & challenge)
                handler->PSendSysMessage("{}: |cff40ff40active|r{}", Name(challenge), (entry.milestone & challenge) ? " (milestone reached)" : "");
            else if (entry.ended & challenge)
                handler->PSendSysMessage("{}: |cffff4040ended|r", Name(challenge));
            else if (entry.finished & challenge)
                handler->PSendSysMessage("{}: |cff40ff40finished|r", Name(challenge));
            else if (IsAllowed(challenge))
                handler->PSendSysMessage("{}: not taken (.challenge {})", Name(challenge), Command(challenge));
        }

        for (Challenge challenge : ALL_CHALLENGES)
            if (Active(player) & challenge)
                handler->SendSysMessage(Rules(challenge));
    }

    void QueueGroupCheck(Group* group, ObjectGuid subject)
    {
        if (group->isBGGroup() || group->isBFGroup())
            return;

        bool solo = false;
        bool hardcore = false;
        for (Group::MemberSlot const& member : group->GetMemberSlots())
        {
            ObjectGuid::LowType const guid = member.guid.GetCounter();
            if (MustPlaySolo(guid))
                solo = true;
            if (HardcoreByGuid(guid))
                hardcore = true;
        }
        if (!hardcore || !config.hardcoreLevelRange)
            subject = ObjectGuid::Empty;
        if (!solo && !subject)
            return;

        std::lock_guard<std::mutex> guard(groupLock);
        pendingGroups.push_back({ group->GetGUID(), subject });
    }

    // World thread, while no map updates: removes everyone who must play solo from the groups queued,
    // then settles Hardcore's level rule around each subject.
    void CheckQueuedGroups()
    {
        std::vector<GroupCheck> checks;
        {
            std::lock_guard<std::mutex> guard(groupLock);
            if (pendingGroups.empty())
                return;
            checks.swap(pendingGroups);
        }

        auto takeOut = [](ObjectGuid groupGuid, ObjectGuid guid, std::string const& why)
        {
            // Removing the second-to-last member disbands the group.
            Group* group = sGroupMgr->GetGroupByGUID(groupGuid.GetCounter());
            if (!group || !group->IsMember(guid))
                return false;
            Player::RemoveFromGroup(group, guid);
            if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
                ChatHandler(player->GetSession()).PSendSysMessage("|cffff4040{}|r You were taken out of the group.", why);
            return true;
        };

        for (GroupCheck const& check : checks)
        {
            Group* group = sGroupMgr->GetGroupByGUID(check.group.GetCounter());
            if (!group)
                continue;

            std::vector<ObjectGuid> solo;
            for (Group::MemberSlot const& member : group->GetMemberSlots())
                if (MustPlaySolo(member.guid.GetCounter()))
                    solo.push_back(member.guid);

            for (ObjectGuid const& guid : solo)
            {
                Player* player = ObjectAccessor::FindConnectedPlayer(guid);
                takeOut(check.group, guid, player ? std::string(Names(Active(player) & SoloMask())) + ": you play alone." :
                    std::string("You play alone."));
            }

            group = sGroupMgr->GetGroupByGUID(check.group.GetCounter());
            if (!check.subject || !group || !group->IsMember(check.subject))
                continue;

            // An online member's own level; the cache for the rest.
            auto levelOf = [](ObjectGuid guid)
            {
                if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
                    return player->GetLevel();
                return CachedLevel(guid.GetCounter());
            };

            ObjectGuid::LowType const subject = check.subject.GetCounter();
            uint8 const subjectLevel = levelOf(check.subject);
            std::vector<ObjectGuid> outOfRange;
            for (Group::MemberSlot const& member : group->GetMemberSlots())
                if (!LevelRule(subject, subjectLevel, member.guid.GetCounter(), levelOf(member.guid)).empty())
                    outOfRange.push_back(member.guid);
            if (outOfRange.empty())
                continue;

            // The leader keeps the group; anyone else who opened the gap leaves it.
            if (!group->IsLeader(check.subject))
                outOfRange = { check.subject };
            for (ObjectGuid const& guid : outOfRange)
                takeOut(check.group, guid, RangeText());
        }
    }

    char const* GearRule(Player const* player, Item const* item)
    {
        return GearRule(player, item, Active(player));
    }

    bool IsProfessionSkill(uint32 skill)
    {
        switch (skill)
        {
            case SKILL_ALCHEMY:
            case SKILL_BLACKSMITHING:
            case SKILL_ENCHANTING:
            case SKILL_ENGINEERING:
            case SKILL_HERBALISM:
            case SKILL_INSCRIPTION:
            case SKILL_JEWELCRAFTING:
            case SKILL_LEATHERWORKING:
            case SKILL_MINING:
            case SKILL_SKINNING:
            case SKILL_TAILORING:
            case SKILL_FIRST_AID:
            case SKILL_COOKING:
            case SKILL_FISHING:
                return true;
            default:
                break;
        }
        for (uint32 extra : config.ironManExtraSkills)
            if (extra == skill)
                return true;
        return false;
    }

    bool IsProfessionSpell(SpellInfo const* spell)
    {
        return IsProfessionSpell(spell, LEARN_DEPTH);
    }

    bool IsBannedItemUse(ItemTemplate const* proto)
    {
        if (questStartItems.count(proto->ItemId))
            return false;

        if (proto->Class == ITEM_CLASS_GLYPH)
            return true;

        // Ammo isn't equipped through CanEquipItem; the core asks CanUseItem for it instead.
        if (proto->InventoryType == INVTYPE_AMMO && proto->Quality > ITEM_QUALITY_NORMAL)
            return true;

        if (proto->Class == ITEM_CLASS_CONSUMABLE)
        {
            switch (proto->SubClass)
            {
                case ITEM_SUBCLASS_POTION:
                case ITEM_SUBCLASS_ELIXIR:
                case ITEM_SUBCLASS_FLASK:
                case ITEM_SUBCLASS_SCROLL:
                case ITEM_SUBCLASS_ITEM_ENHANCEMENT:
                    return true;
                default:
                    break;
            }
        }

        // Class-only temporary enchants (rogue poisons) are class tools. Class-only permanent ones
        // (the Zul'Gurub head and leg enchants) are still enchants.
        bool const classItem = proto->AllowableClass > 0 && (uint32(proto->AllowableClass) & CLASSMASK_ALL_PLAYABLE) != CLASSMASK_ALL_PLAYABLE;

        for (auto const& itemSpell : proto->Spells)
        {
            if (itemSpell.SpellId <= 0 || itemSpell.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                continue;
            SpellInfo const* spell = sSpellMgr->GetSpellInfo(itemSpell.SpellId);
            if (!spell)
                continue;

            for (SpellEffectInfo const& effect : spell->Effects)
            {
                // Sharpening stones, weightstones, oils, armor kits.
                if (effect.Effect == SPELL_EFFECT_ENCHANT_ITEM || effect.Effect == SPELL_EFFECT_ENCHANT_ITEM_PRISMATIC ||
                    (effect.Effect == SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY && !classItem))
                    return true;
                // Buff food: the eating aura triggers Well Fed once the player has eaten long enough.
                if (proto->Class == ITEM_CLASS_CONSUMABLE && proto->SubClass == ITEM_SUBCLASS_FOOD &&
                    effect.Effect == SPELL_EFFECT_APPLY_AURA && effect.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL)
                    return true;
            }
        }
        return false;
    }

    void EnforceIronMan(Player* player)
    {
        if (!Has(player, CHALLENGE_IRONMAN))
            return;

        uint32 removed = 0;
        for (uint8 equipSlot = EQUIPMENT_SLOT_START; equipSlot < EQUIPMENT_SLOT_END; ++equipSlot)
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, equipSlot);
            if (!item)
                continue;
            // Every banned slot comes off before any is cleared: a meta gem's condition counts the
            // other gems, and the prismatic socket has to exist while its gem comes off.
            std::vector<EnchantmentSlot> banned;
            for (uint8 slot = PERM_ENCHANTMENT_SLOT; slot < MAX_INSPECTED_ENCHANTMENT_SLOT; ++slot)
                if (IsBannedEnchant(slot, item->GetEnchantmentId(EnchantmentSlot(slot))))
                    banned.push_back(EnchantmentSlot(slot));
            for (EnchantmentSlot slot : banned)
                player->ApplyEnchantment(item, slot, false);
            for (EnchantmentSlot slot : banned)
                item->ClearEnchantment(slot);
            removed += banned.size();
        }

        // The same steps as removing a glyph at the client's request.
        for (uint8 slot = 0; slot < MAX_GLYPH_SLOT_INDEX; ++slot)
        {
            GlyphPropertiesEntry const* glyph = sGlyphPropertiesStore.LookupEntry(player->GetGlyph(slot));
            if (!glyph)
                continue;
            player->RemoveAurasDueToSpell(glyph->SpellId);
            Unit::AuraMap& ownedAuras = player->GetOwnedAuras();
            for (Unit::AuraMap::iterator itr = ownedAuras.begin(); itr != ownedAuras.end();)
            {
                SpellInfo const* triggeredBy = itr->second->GetTriggeredByAuraSpellInfo();
                if (triggeredBy && triggeredBy->Id == glyph->SpellId)
                    player->RemoveOwnedAura(itr);
                else
                    ++itr;
            }
            player->SendLearnPacket(glyph->SpellId, false);
            player->SetGlyph(slot, 0, true);
            ++removed;
        }

        if (!removed)
            return;
        player->SendTalentsInfoData(false);
        ChatHandler(player->GetSession()).PSendSysMessage("|cffff4040Iron Man|r: {} enchant(s), gem(s) or glyph(s) taken off.", removed);
        LOG_INFO("module", "mod-challenges: stripped {} enchants/glyphs from Iron Man {} ({})", removed, player->GetName(),
            player->GetGUID().GetCounter());
    }

    void NotifyRule(Player* player, char const* text)
    {
        PlayerState& state = State(player);
        time_t const now = GameTime::GetGameTime().count();
        if (now - state.lastNotice < NOTICE_INTERVAL)
            return;
        state.lastNotice = now;
        ChatHandler(player->GetSession()).SendNotification(text);
    }
}
