/*
 * mod-challenges: hooks, the Shrine of Challenge and the .challenge command.
 *
 * Released under the MIT License.
 */

#include "Challenges.h"
#include "Chat.h"
#include "CommandScript.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "Group.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "SpellMgr.h"
#include "Trainer.h"
#include "WorldSession.h"

using namespace Acore::ChatCommands;

namespace Challenges
{
    namespace
    {
        enum GossipSender : uint32
        {
            SENDER_MAIN = 1,
            SENDER_TAKE,
            SENDER_RULES,
            SENDER_CLOSE,
        };

        // How long the first-login window waits for an intro cinematic to end before it opens anyway.
        constexpr Milliseconds FIRST_LOGIN_DELAY = 2s;
        constexpr uint32 FIRST_LOGIN_TRIES = 90;

        bool IronMan(Player const* player)
        {
            return Has(player, CHALLENGE_IRONMAN);
        }

        bool SelfFound(Player const* player)
        {
            return Has(player, CHALLENGE_SELF_FOUND);
        }

        std::string Popup(Challenge challenge)
        {
            return Rules(challenge) + " This can't be undone. Take the challenge?";
        }

        void NotifySolo(Player* player)
        {
            NotifyRule(player, "Your challenge allows no groups.");
        }

        bool CanStartAny(Player* player)
        {
            for (Challenge challenge : ALL_CHALLENGES)
                if (CanStart(player, challenge).empty())
                    return true;
            return false;
        }

        // The challenge menu, from the shrine or (the first-login window) from the player itself.
        // A menu the player sends has to carry its menu id, which the core checks on the reply.
        void ShowMenu(Player* player, ObjectGuid source, uint32 text)
        {
            ClearGossipMenuFor(player);
            uint8 const active = Active(player);
            for (Challenge challenge : ALL_CHALLENGES)
            {
                if (active & challenge)
                    AddGossipItemFor(player, GOSSIP_ICON_DOT, std::string(Name(challenge)) + ": taken", SENDER_MAIN, 0);
                else if (CanStart(player, challenge).empty())
                    AddGossipItemFor(player, GOSSIP_ICON_BATTLE, std::string("Take the ") + Name(challenge) + " challenge",
                        SENDER_TAKE, challenge, Popup(challenge), 0, false);
            }
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "What do the challenges ask of me?", SENDER_RULES, 0);
            if (source == player->GetGUID())
            {
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Not now. (The Shrine of Challenge by your graveyard keeps the offer "
                    "while you are at your starting level.)", SENDER_CLOSE, 0);
                player->PlayerTalkClass->GetGossipMenu().SetMenuId(MENU_FIRST_LOGIN);
            }
            SendGossipMenuFor(player, text, source);
        }

        void SelectMenu(Player* player, ObjectGuid source, uint32 text, uint32 sender, uint32 action)
        {
            switch (sender)
            {
                case SENDER_TAKE:
                {
                    Challenge const challenge = Challenge(action);
                    std::string const why = CanStart(player, challenge);
                    if (why.empty())
                        Start(player, challenge);
                    else
                        ChatHandler(player->GetSession()).SendSysMessage(why);
                    // Taking one challenge often leaves others to take.
                    if (CanStartAny(player))
                        ShowMenu(player, source, text);
                    else
                        CloseGossipMenuFor(player);
                    return;
                }
                case SENDER_RULES:
                    ClearGossipMenuFor(player);
                    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", SENDER_MAIN, 0);
                    if (source == player->GetGUID())
                        player->PlayerTalkClass->GetGossipMenu().SetMenuId(MENU_FIRST_LOGIN);
                    SendGossipMenuFor(player, TEXT_RULES, source);
                    return;
                case SENDER_CLOSE:
                    CloseGossipMenuFor(player);
                    return;
                default:
                    ShowMenu(player, source, text);
                    return;
            }
        }

        // Opens the first-login window once the intro cinematic is over: a window opened during it
        // would sit behind the cinematic and could be closed with it.
        void ScheduleFirstLoginWindow(Player* player, uint32 triesLeft)
        {
            player->m_Events.AddEventAtOffset([player, triesLeft]()
            {
                if (!player->IsInWorld() || !player->GetSession())
                    return;
                // The login handler sets the camera id as soon as it starts an intro cinematic, before
                // the loading screen is gone; it is cleared when the client reports the cinematic done.
                CinematicMgr& cinematic = player->GetCinematicMgr();
                if ((cinematic.GetActiveCinematicCamera() || cinematic.IsOnCinematic()) && triesLeft)
                {
                    ScheduleFirstLoginWindow(player, triesLeft - 1);
                    return;
                }
                if (AtStartLevel(player) && CanStartAny(player))
                    ShowMenu(player, player->GetGUID(), TEXT_FIRST_LOGIN);
            }, FIRST_LOGIN_DELAY);
        }
    }

    class ChallengesPlayerScript : public PlayerScript
    {
    public:
        ChallengesPlayerScript() : PlayerScript("ChallengesPlayerScript", {
            PLAYERHOOK_ON_PLAYER_JUST_DIED,
            PLAYERHOOK_ON_LOGIN,
            PLAYERHOOK_ON_FIRST_LOGIN,
            PLAYERHOOK_ON_GOSSIP_SELECT,
            PLAYERHOOK_ON_LEVEL_CHANGED,
            PLAYERHOOK_ON_DELETE_FROM_DB,
            PLAYERHOOK_ON_MAP_CHANGED,
            PLAYERHOOK_ON_GIVE_EXP,
            PLAYERHOOK_CAN_EQUIP_ITEM,
            PLAYERHOOK_CAN_USE_ITEM,
            PLAYERHOOK_CAN_LEARN_TALENT,
            PLAYERHOOK_CAN_LEARN_SPELL,
            PLAYERHOOK_ON_GET_TRAINER_SPELL_STATE,
            PLAYERHOOK_CAN_INIT_TRADE,
            PLAYERHOOK_CAN_SET_TRADE_ITEM,
            PLAYERHOOK_CAN_SEND_MAIL,
            PLAYERHOOK_CAN_PLACE_AUCTION_BID,
            PLAYERHOOK_CAN_GROUP_INVITE,
            PLAYERHOOK_CAN_GROUP_ACCEPT,
            PLAYERHOOK_CAN_JOIN_LFG
        }) { }

        // Deaths that don't go through Unit::Kill, such as Spirit of Redemption running out.
        void OnPlayerJustDied(Player* player) override
        {
            HandleDeath(player, nullptr);
        }

        void OnPlayerLogin(Player* player) override
        {
            HandleLogin(player);
        }

        // A new character gets the challenges offered as it first enters the world. Bots (sessions
        // without a socket) never see it.
        void OnPlayerFirstLogin(Player* player) override
        {
            // Bots have no socket (IsSocketClosed rather than IsHeadless, which stock AzerothCore lacks).
            if (!GetConfig().enabled || !GetConfig().firstLoginWindow || player->GetSession()->IsSocketClosed())
                return;
            ScheduleFirstLoginWindow(player, FIRST_LOGIN_TRIES);
        }

        void OnPlayerGossipSelect(Player* player, uint32 menuId, uint32 sender, uint32 action) override
        {
            if (menuId == MENU_FIRST_LOGIN)
                SelectMenu(player, player->GetGUID(), TEXT_FIRST_LOGIN, sender, action);
        }

        void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
        {
            HandleLevel(player);
            EnforceIronMan(player);
        }

        void OnPlayerMapChanged(Player* player) override
        {
            EnforceIronMan(player);
        }

        void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
        {
            HandleDelete(trans, guid);
        }

        void OnPlayerGiveXP(Player* player, uint32& amount, Unit* victim, uint8 xpSource) override
        {
            HandleXP(player, amount, victim, xpSource);
        }

        bool OnPlayerCanEquipItem(Player* player, uint8 /*slot*/, uint16& /*dest*/, Item* item, bool /*swap*/, bool notLoading) override
        {
            if (!notLoading || !item)
                return true;
            char const* rule = GearRule(player, item);
            if (!rule)
                return true;
            NotifyRule(player, rule);
            return false;
        }

        // Also asked whenever the core checks an item for the player, so it tells the player nothing;
        // the use fails with the core's error.
        bool OnPlayerCanUseItem(Player* player, ItemTemplate const* proto, InventoryResult& result) override
        {
            if (!proto || !IronMan(player) || !IsBannedItemUse(proto))
                return true;
            result = EQUIP_ERR_CANT_DO_RIGHT_NOW;
            return false;
        }

        bool OnPlayerCanLearnTalent(Player* player, TalentEntry const* /*talent*/, uint32 /*rank*/) override
        {
            if (!IronMan(player))
                return true;
            NotifyRule(player, "Iron Man: no talents.");
            return false;
        }

        bool OnPlayerCanLearnSpell(Player* player, uint32 spellId) override
        {
            if (!IronMan(player))
                return true;
            SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId);
            if (!spell || !IsProfessionSpell(spell))
                return true;
            NotifyRule(player, "Iron Man: no professions.");
            return false;
        }

        // The trainer charges before it teaches, so a profession must already show as unavailable here.
        void OnPlayerGetTrainerSpellState(Player const* player, uint32 /*trainerId*/, uint32 spellId, Trainer::SpellState& state) override
        {
            if (state == Trainer::SpellState::Known || !IronMan(player))
                return;
            if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId))
                if (IsProfessionSpell(spell))
                    state = Trainer::SpellState::Unavailable;
        }

        // Trading needs the two players next to each other, so both are on this map thread.
        bool OnPlayerCanInitTrade(Player* player, Player* target) override
        {
            if (SelfFound(player))
            {
                NotifyRule(player, "Self-Found: no trading.");
                return false;
            }
            if (target && SelfFound(target))
            {
                NotifyRule(player, "That player is Self-Found and can't trade.");
                return false;
            }
            return true;
        }

        // An item in the "will not be traded" slot is one the other player can enchant, worn gear included.
        bool OnPlayerCanSetTradeItem(Player* player, Item* /*item*/, uint8 tradeSlot) override
        {
            if (tradeSlot != TRADE_SLOT_NONTRADED || !IronMan(player))
                return true;
            NotifyRule(player, "Iron Man: no enchants from others.");
            return false;
        }

        // Only player mail comes through here; quest and system mail still reach Self-Found characters.
        bool OnPlayerCanSendMail(Player* player, ObjectGuid receiverGuid, ObjectGuid /*mailbox*/, std::string& /*subject*/,
            std::string& /*body*/, uint32 /*money*/, uint32 /*cod*/, Item* /*item*/) override
        {
            if (SelfFound(player))
            {
                NotifyRule(player, "Self-Found: no mail.");
                return false;
            }
            if (IsSelfFound(receiverGuid.GetCounter()))
            {
                NotifyRule(player, "That character is Self-Found and can't receive mail.");
                return false;
            }
            return true;
        }

        // Also asked for every listing an auction browser shows, so it tells the player nothing.
        bool OnPlayerCanPlaceAuctionBid(Player* player, AuctionEntry* /*auction*/) override
        {
            return !SelfFound(player);
        }

        bool OnPlayerCanGroupInvite(Player* player, std::string& memberName) override
        {
            if (MustPlaySolo(player))
            {
                NotifySolo(player);
                return false;
            }
            Player* target = ObjectAccessor::FindPlayerByName(memberName, false);
            if (target && MustPlaySolo(target->GetGUID().GetCounter()))
            {
                NotifyRule(player, "That player's challenge allows no groups.");
                return false;
            }
            return true;
        }

        bool OnPlayerCanGroupAccept(Player* player, Group* /*group*/) override
        {
            if (!MustPlaySolo(player))
                return true;
            NotifySolo(player);
            return false;
        }

        bool OnPlayerCanJoinLfg(Player* player, uint8 /*roles*/, std::set<uint32>& /*dungeons*/, std::string const& /*comment*/) override
        {
            if (!MustPlaySolo(player))
                return true;
            NotifySolo(player);
            return false;
        }
    };

    class ChallengesUnitScript : public UnitScript
    {
    public:
        ChallengesUnitScript() : UnitScript("ChallengesUnitScript", true, {
            UNITHOOK_ON_UNIT_DEATH
        }) { }

        void OnUnitDeath(Unit* unit, Unit* killer) override
        {
            if (Player* player = unit->ToPlayer())
                HandleDeath(player, killer);
        }
    };

    // Self-Found: the auction house window and the guild bank stay shut.
    class ChallengesMiscScript : public MiscScript
    {
    public:
        ChallengesMiscScript() : MiscScript("ChallengesMiscScript", {
            MISCHOOK_CAN_SEND_AUCTIONHELLO
        }) { }

        bool CanSendAuctionHello(WorldSession const* session, ObjectGuid /*guid*/, Creature* /*creature*/) override
        {
            Player* player = session->GetPlayer();
            if (!player || !SelfFound(player))
                return true;
            NotifyRule(player, "Self-Found: no auction house.");
            return false;
        }
    };

    class ChallengesGuildScript : public GuildScript
    {
    public:
        ChallengesGuildScript() : GuildScript("ChallengesGuildScript", {
            GUILDHOOK_CAN_GUILD_SEND_BANK_LIST,
            GUILDHOOK_ON_MEMBER_WITDRAW_MONEY
        }) { }

        // The session is null when the core broadcasts a bank change to every member who has the
        // tab open; that update can't be filtered per member.
        bool CanGuildSendBankList(Guild const* /*guild*/, WorldSession* session, uint8 /*tabId*/, bool /*sendAllSlots*/) override
        {
            Player* player = session ? session->GetPlayer() : nullptr;
            if (!player || !SelfFound(player))
                return true;
            NotifyRule(player, "Self-Found: no guild bank.");
            return false;
        }

        // A repair paid by the guild can't be refused from here: a zero amount would make it free.
        void OnMemberWitdrawMoney(Guild* /*guild*/, Player* player, uint32& amount, bool isRepair) override
        {
            if (isRepair || !player || !SelfFound(player))
                return;
            amount = 0;
            NotifyRule(player, "Self-Found: no guild bank.");
        }
    };

    // Playerbots adds bots to a group without an invite, so the invite checks never see it.
    class ChallengesGroupScript : public GroupScript
    {
    public:
        ChallengesGroupScript() : GroupScript("ChallengesGroupScript", {
            GROUPHOOK_ON_ADD_MEMBER
        }) { }

        void OnAddMember(Group* group, ObjectGuid /*guid*/) override
        {
            QueueGroupCheck(group);
        }
    };

    class ChallengesWorldScript : public WorldScript
    {
    public:
        ChallengesWorldScript() : WorldScript("ChallengesWorldScript", {
            WORLDHOOK_ON_AFTER_CONFIG_LOAD,
            WORLDHOOK_ON_STARTUP,
            WORLDHOOK_ON_UPDATE
        }) { }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            LoadConfig();
        }

        void OnStartup() override
        {
            Load();
        }

        void OnUpdate(uint32 /*diff*/) override
        {
            CheckQueuedGroups();
        }
    };

    class go_shrine_of_challenge : public GameObjectScript
    {
    public:
        go_shrine_of_challenge() : GameObjectScript("go_shrine_of_challenge") { }

        struct go_shrine_of_challengeAI : public GameObjectAI
        {
            explicit go_shrine_of_challengeAI(GameObject* go) : GameObjectAI(go) { }

            // Only new characters see it; there is nothing it can do for anyone else.
            bool CanBeSeen(Player const* player) override
            {
                return player->IsGameMaster() || (GetConfig().enabled && AtStartLevel(player));
            }
        };

        GameObjectAI* GetAI(GameObject* go) const override
        {
            return new go_shrine_of_challengeAI(go);
        }

        bool OnGossipHello(Player* player, GameObject* go) override
        {
            ShowMenu(player, go->GetGUID(), TEXT_SHRINE);
            return true;
        }

        bool OnGossipSelect(Player* player, GameObject* go, uint32 sender, uint32 action) override
        {
            SelectMenu(player, go->GetGUID(), TEXT_SHRINE, sender, action);
            return true;
        }
    };

    class ChallengesCommandScript : public CommandScript
    {
    public:
        ChallengesCommandScript() : CommandScript("ChallengesCommandScript") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable subCommands =
            {
                { "status",      HandleStatusCommand,                          SEC_PLAYER, Console::No },
                { "hardcore",    HandleTakeCommand<CHALLENGE_HARDCORE>,        SEC_PLAYER, Console::No },
                { "ironman",     HandleTakeCommand<CHALLENGE_IRONMAN>,         SEC_PLAYER, Console::No },
                { "selffound",   HandleTakeCommand<CHALLENGE_SELF_FOUND>,      SEC_PLAYER, Console::No },
                { "selfcrafted", HandleTakeCommand<CHALLENGE_SELF_CRAFTED>,    SEC_PLAYER, Console::No },
                { "slowxp",      HandleTakeCommand<CHALLENGE_SLOW_XP>,         SEC_PLAYER, Console::No },
                { "questxp",     HandleTakeCommand<CHALLENGE_QUEST_XP_ONLY>,   SEC_PLAYER, Console::No },
                { "itemquality", HandleTakeCommand<CHALLENGE_ITEM_QUALITY>,    SEC_PLAYER, Console::No },
            };

            static ChatCommandTable commandTable =
            {
                { "challenge", subCommands },
            };

            return commandTable;
        }

        // With a player selected, shows that player's challenges.
        static bool HandleStatusCommand(ChatHandler* handler)
        {
            Player* target = handler->getSelectedPlayerOrSelf();
            if (!target)
                return false;
            if (target != handler->GetPlayer())
                handler->PSendSysMessage("Challenges of {}:", target->GetName());
            SendStatus(handler, target);
            return true;
        }

        template<Challenge challenge>
        static bool HandleTakeCommand(ChatHandler* handler, Optional<std::string> confirm)
        {
            Player* player = handler->GetPlayer();
            std::string const why = CanStart(player, challenge);
            if (!why.empty())
            {
                handler->SendErrorMessage(why, false);
                return false;
            }

            if (!confirm || *confirm != "confirm")
            {
                handler->SendSysMessage(Popup(challenge));
                handler->PSendSysMessage("Type .challenge {} confirm to take it.", Command(challenge));
                return true;
            }

            Start(player, challenge);
            return true;
        }
    };
}

void AddChallengesScripts()
{
    new Challenges::ChallengesPlayerScript();
    new Challenges::ChallengesUnitScript();
    new Challenges::ChallengesMiscScript();
    new Challenges::ChallengesGuildScript();
    new Challenges::ChallengesGroupScript();
    new Challenges::ChallengesWorldScript();
    new Challenges::go_shrine_of_challenge();
    new Challenges::ChallengesCommandScript();
}
