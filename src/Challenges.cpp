/*
 * mod-challenges: state, opt-in, deaths and the rules.
 *
 * Every row of mod_challenges_character is loaded at startup (one per character and challenge,
 * so the table stays small) and kept in a locked map. A player copies their own entry into
 * Player::CustomData the first time a hook asks, so the hooks that run for every player (bots
 * included) never take the lock. Writes go to the database asynchronously.
 *
 * Released under the MIT License.
 */

#include "Challenges.h"
#include "ChallengesApi.h"
#include "Chat.h"
#include "Config.h"
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
#include <algorithm>
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
        };

        Definition const* Find(Challenge challenge)
        {
            for (Definition const& definition : DEFINITIONS)
                if (definition.id == challenge)
                    return &definition;
            return nullptr;
        }

        // Iron Man already holds gear to white and has no professions to craft with.
        uint8 Excludes(Challenge challenge)
        {
            switch (challenge)
            {
                case CHALLENGE_IRONMAN:      return CHALLENGE_SELF_CRAFTED | CHALLENGE_ITEM_QUALITY;
                case CHALLENGE_SELF_CRAFTED: return CHALLENGE_IRONMAN;
                case CHALLENGE_ITEM_QUALITY: return CHALLENGE_IRONMAN;
                default:                     return 0;
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

        constexpr uint8 GEAR_CHALLENGES = CHALLENGE_IRONMAN | CHALLENGE_SELF_CRAFTED | CHALLENGE_ITEM_QUALITY;
        constexpr time_t NOTICE_INTERVAL = 5;
        constexpr uint8 LEARN_DEPTH = 3;

        struct Entry
        {
            uint8 active = 0;     // taken and still running
            uint8 ended = 0;      // taken and ended by a death
            uint8 milestone = 0;  // reached the milestone level while running
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

        std::mutex groupLock;
        std::vector<ObjectGuid> pendingGroups;

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

        uint8 Enforced()
        {
            return config.enabled ? config.allowed : 0;
        }

        uint8 SoloMask()
        {
            uint8 mask = 0;
            if (!config.selfFoundGroups)
                mask |= CHALLENGE_SELF_FOUND;
            if (!config.ironManGroups)
                mask |= CHALLENGE_IRONMAN;
            return mask & Enforced();
        }

        // The running challenges of a character, online or not, from the shared map.
        uint8 ActiveByGuid(ObjectGuid::LowType guid)
        {
            uint8 const enforced = Enforced();
            if (!enforced)
                return 0;
            std::shared_lock<std::shared_mutex> guard(lock);
            auto itr = entries.find(guid);
            return itr != entries.end() ? itr->second.active & enforced : 0;
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

        std::string Tag(uint8 mask)
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
        char const* GearRule(Player const* player, Item const* item, uint8 mask)
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

        config.ironManExtraSkills.clear();
        std::string const extraSkills = sConfigMgr->GetOption<std::string>("Challenges.IronMan.ExtraProfessionSkills", "");
        for (std::string_view token : Acore::Tokenize(extraSkills, ' ', false))
            if (Optional<uint32> skill = Acore::StringTo<uint32>(token))
                config.ironManExtraSkills.push_back(*skill);
    }

    void Load()
    {
        std::unordered_map<ObjectGuid::LowType, Entry> loaded;
        if (QueryResult result = CharacterDatabase.Query("SELECT `guid`, `challenge`, `state`, `milestone_at` FROM `mod_challenges_character`"))
        {
            do
            {
                Field* fields = result->Fetch();
                Entry& entry = loaded[fields[0].Get<uint32>()];
                uint8 const challenge = fields[1].Get<uint8>();
                if (fields[2].Get<uint8>() == 0)
                    entry.active |= challenge;
                else
                    entry.ended |= challenge;
                if (fields[3].Get<uint32>())
                    entry.milestone |= challenge;
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
        LOG_INFO("server.loading", ">> mod-challenges: {} characters with a challenge", entries.size());
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
                return "Hardcore: you have one life. The first death ends the challenge, and the realm hears of it. "
                    "The character then plays on as normal.";
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
        }
        return "";
    }

    std::string Names(uint8 mask)
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
    uint8 Active(Player const* player)
    {
        uint8 const enforced = Enforced();
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

    std::string CanStart(Player* player, Challenge challenge)
    {
        if (!IsAllowed(challenge))
            return Acore::StringFormat("The {} challenge is not available on this realm.", Name(challenge));

        Entry const& entry = State(player).entry;
        if (entry.active & challenge)
            return Acore::StringFormat("You have already taken the {} challenge.", Name(challenge));
        if (entry.ended & challenge)
            return Acore::StringFormat("Your {} challenge has ended. It can't be taken again on this character.", Name(challenge));
        if (uint8 const clash = entry.active & Excludes(challenge))
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

        CharacterDatabase.Execute("REPLACE INTO `mod_challenges_character` (`guid`, `challenge`, `state`, `started_at`, `start_level`) "
            "VALUES ({}, {}, 0, {}, {})", guid, uint32(challenge), uint32(GameTime::GetGameTime().count()), uint32(player->GetLevel()));

        ChatHandler(player->GetSession()).PSendSysMessage("|cffff4040{}|r taken. {}", Name(challenge), Rules(challenge));

        if (config.announceStart)
            Announce(Tag(challenge) + player->GetName() + " has taken the " + Name(challenge) + " challenge. Good luck!");
    }

    void HandleDeath(Player* player, Unit* killer)
    {
        uint8 const mask = Active(player) & ENDS_ON_DEATH;
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

        uint8 const reached = Active(player) & ~State(player).entry.milestone;
        if (!reached)
            return;

        Update(player, [reached](Entry& e) { e.milestone |= reached; });

        CharacterDatabase.Execute("UPDATE `mod_challenges_character` SET `milestone_at` = {} WHERE `guid` = {} AND (`challenge` & {}) <> 0",
            uint32(GameTime::GetGameTime().count()), player->GetGUID().GetCounter(), uint32(reached));

        if (config.announceMilestone)
            Announce(Tag(reached) + player->GetName() + " has reached level " + std::to_string(player->GetLevel()) +
                ((reached & ENDS_ON_DEATH) ? " without dying!" : "!"));
    }

    void HandleLogin(Player* player)
    {
        EnforceIronMan(player);

        // A group from before the rule (saved across a restart, or AllowGroups switched off since).
        // In a battleground the normal group is the original one.
        if (MustPlaySolo(player))
        {
            if (Group* group = player->GetGroup())
                QueueGroupCheck(group);
            if (Group* group = player->GetOriginalGroup())
                QueueGroupCheck(group);
        }

        uint8 const mask = Active(player);
        if (!mask || !config.loginReminder)
            return;
        ChatHandler(player->GetSession()).SendSysMessage(Acore::StringFormat("|cffff4040{}|r on this character. "
            "Type .challenge status for the rules.", Names(mask)));
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
        uint8 const mask = Active(player) & (CHALLENGE_SLOW_XP | CHALLENGE_QUEST_XP_ONLY);
        if (!mask || !amount)
            return;

        if ((mask & CHALLENGE_QUEST_XP_ONLY) && source != PlayerXPSource::XPSOURCE_QUEST && source != PlayerXPSource::XPSOURCE_QUEST_DF)
        {
            if (source == PlayerXPSource::XPSOURCE_KILL && victim)
                if (Pet* pet = player->GetPet())
                    pet->GivePetXP(player->GetGroup() ? amount / 2 : amount);
            amount = 0;
            return;
        }

        if (mask & CHALLENGE_SLOW_XP)
            amount = uint32(amount * config.slowXpRate);
    }

    void SendStatus(ChatHandler* handler, Player* player)
    {
        Entry const& entry = State(player).entry;

        for (Challenge challenge : ALL_CHALLENGES)
        {
            if ((entry.active & challenge) && !IsAllowed(challenge))
                handler->PSendSysMessage("{}: taken, but switched off on this realm", Name(challenge));
            else if (entry.active & challenge)
                handler->PSendSysMessage("{}: |cff40ff40active|r{}", Name(challenge), (entry.milestone & challenge) ? " (milestone reached)" : "");
            else if (entry.ended & challenge)
                handler->PSendSysMessage("{}: |cffff4040ended|r", Name(challenge));
            else if (IsAllowed(challenge))
                handler->PSendSysMessage("{}: not taken (.challenge {})", Name(challenge), Command(challenge));
        }

        for (Challenge challenge : ALL_CHALLENGES)
            if (Active(player) & challenge)
                handler->SendSysMessage(Rules(challenge));
    }

    void QueueGroupCheck(Group* group)
    {
        if (group->isBGGroup() || group->isBFGroup())
            return;

        bool solo = false;
        for (Group::MemberSlot const& member : group->GetMemberSlots())
            if (MustPlaySolo(member.guid.GetCounter()))
                solo = true;
        if (!solo)
            return;

        std::lock_guard<std::mutex> guard(groupLock);
        pendingGroups.push_back(group->GetGUID());
    }

    // World thread, while no map updates: removes everyone who must play solo from the groups queued.
    void CheckQueuedGroups()
    {
        std::vector<ObjectGuid> groups;
        {
            std::lock_guard<std::mutex> guard(groupLock);
            if (pendingGroups.empty())
                return;
            groups.swap(pendingGroups);
        }

        for (ObjectGuid const& groupGuid : groups)
        {
            Group* group = sGroupMgr->GetGroupByGUID(groupGuid.GetCounter());
            if (!group)
                continue;

            std::vector<ObjectGuid> solo;
            for (Group::MemberSlot const& member : group->GetMemberSlots())
                if (MustPlaySolo(member.guid.GetCounter()))
                    solo.push_back(member.guid);

            for (ObjectGuid const& guid : solo)
            {
                // Removing the second-to-last member disbands the group.
                group = sGroupMgr->GetGroupByGUID(groupGuid.GetCounter());
                if (!group || !group->IsMember(guid))
                    break;
                Player::RemoveFromGroup(group, guid);
                if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
                    ChatHandler(player->GetSession()).PSendSysMessage("|cffff4040{}|r: you play alone, so you were taken out of the group.",
                        Names(Active(player) & SoloMask()));
            }
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
