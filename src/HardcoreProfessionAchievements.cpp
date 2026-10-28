/* Hardcore profession achievement eligibility. Released under the MIT License. */
#include "Challenges.h"
#include "AchievementMgr.h"
#include "AchievementScript.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Player.h"
#include "PlayerScript.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldScript.h"
#include <algorithm>
#include <array>
#include <string_view>
#include <unordered_map>

namespace
{
struct ProfessionDefinition
{
    uint32 SkillId;
    std::string_view Name;
    std::array<uint32, 6> RankSpells;
};

constexpr std::array<ProfessionDefinition, 11> PrimaryProfessionDefinitions =
{{
    { SKILL_ALCHEMY,       "alchemy",       { 2259, 3101, 3464, 11611, 28596, 51304 } },
    { SKILL_BLACKSMITHING, "blacksmithing", { 2018, 3100, 3538, 9785, 29844, 51300 } },
    { SKILL_ENCHANTING,   "enchanting",    { 7411, 7412, 7413, 13920, 28029, 51313 } },
    { SKILL_ENGINEERING,  "engineering",   { 4036, 4037, 4038, 12656, 30350, 51306 } },
    { SKILL_HERBALISM,    "herbalism",     { 2366, 2368, 3570, 11993, 28695, 50300 } },
    { SKILL_INSCRIPTION,  "inscription",   { 45357, 45358, 45359, 45360, 45361, 45363 } },
    { SKILL_JEWELCRAFTING,"jewelcrafting", { 25229, 25230, 28894, 28895, 28897, 51311 } },
    { SKILL_LEATHERWORKING,"leatherworking",{ 2108, 3104, 3811, 10662, 32549, 51302 } },
    { SKILL_MINING,       "mining",        { 2575, 2576, 3564, 10248, 29354, 50310 } },
    { SKILL_SKINNING,     "skinning",      { 8613, 8617, 8618, 10768, 32678, 50305 } },
    { SKILL_TAILORING,    "tailoring",     { 3908, 3909, 3910, 12180, 26790, 51309 } },
}};

constexpr std::array<ProfessionDefinition, 3> SecondaryProfessionDefinitions =
{{
    { SKILL_COOKING,   "cooking",  { 2550, 3102, 3413, 18260, 33359, 51296 } },
    { SKILL_FIRST_AID, "firstaid", { 3273, 3274, 7924, 10846, 27028, 45542 } },
    { SKILL_FISHING,   "fishing",  { 7620, 7731, 7732, 18248, 33095, 51294 } },
}};

ProfessionDefinition const* GetProfessionDefinition(uint32 skillId)
{
    for (ProfessionDefinition const& profession : PrimaryProfessionDefinitions)
        if (profession.SkillId == skillId)
            return &profession;

    for (ProfessionDefinition const& profession : SecondaryProfessionDefinitions)
        if (profession.SkillId == skillId)
            return &profession;

    return nullptr;
}
uint32 GetBestRankSpellForSkill(uint32 skillId, uint16 maxValue)
{
    ProfessionDefinition const* profession = GetProfessionDefinition(skillId);
    if (!profession)
        return 0;

    uint32 rankIndex = 0;
    if (maxValue > 375)
        rankIndex = 5;
    else if (maxValue > 300)
        rankIndex = 4;
    else if (maxValue > 225)
        rankIndex = 3;
    else if (maxValue > 150)
        rankIndex = 2;
    else if (maxValue > 75)
        rankIndex = 1;

    return profession->RankSpells[rankIndex];
}
bool IsProfessionRankSpell(uint32 spellId)
{
    for (auto const& profession : PrimaryProfessionDefinitions)
        if (std::find(profession.RankSpells.begin(), profession.RankSpells.end(), spellId) != profession.RankSpells.end())
            return true;
    for (auto const& profession : SecondaryProfessionDefinitions)
        if (std::find(profession.RankSpells.begin(), profession.RankSpells.end(), spellId) != profession.RankSpells.end())
            return true;
    return false;
}

// Loaded once at startup; immutable while player threads evaluate achievements.
std::unordered_map<uint32, uint8> ProfessionTrainerLevels;

void LoadProfessionTrainerLevels()
{
    ProfessionTrainerLevels.clear();
    if (QueryResult result = WorldDatabase.Query("SELECT SpellId, MIN(ReqLevel) FROM trainer_spell GROUP BY SpellId"))
    {
        auto record = [](uint32 spellId, uint8 level)
        {
            if (!IsProfessionRankSpell(spellId))
                return;
            auto [itr, inserted] = ProfessionTrainerLevels.emplace(spellId, level);
            if (!inserted)
                itr->second = std::min(itr->second, level);
        };
        do
        {
            Field* fields = result->Fetch();
            uint32 spellId = fields[0].Get<uint32>();
            uint8 level = fields[1].Get<uint8>();
            record(spellId, level);
            // Trainers may cast a wrapper which teaches the actual rank spell.
            if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId))
                for (SpellEffectInfo const& effect : spell->Effects)
                    if (effect.Effect == SPELL_EFFECT_LEARN_SPELL)
                        record(effect.TriggerSpell, level);
        } while (result->NextRow());
    }
}

bool MeetsProfessionAchievementLevel(Player* player, AchievementCriteriaEntry const* criteria)
{
    if (!Challenges::Has(player, Challenges::CHALLENGE_HARDCORE))
        return true;

    uint32 skillId;
    uint32 requiredValue;
    if (criteria->requiredType == ACHIEVEMENT_CRITERIA_TYPE_REACH_SKILL_LEVEL)
    {
        skillId = criteria->reach_skill_level.skillID;
        requiredValue = criteria->reach_skill_level.skillLevel;
    }
    else if (criteria->requiredType == ACHIEVEMENT_CRITERIA_TYPE_LEARN_SKILL_LEVEL)
    {
        skillId = criteria->learn_skill_level.skillID;
        requiredValue = criteria->learn_skill_level.skillLevel * 75;
    }
    else
        return true;

    if (!GetProfessionDefinition(skillId))
        return true;

    // Select the rank which permits this achievement's threshold, not the account's maximum.
    uint32 spellId = GetBestRankSpellForSkill(skillId, uint16(std::min(requiredValue, uint32(450))));
    auto itr = ProfessionTrainerLevels.find(spellId);
    // Missing trainer data must not award an unreachable rank to a fresh character.
    return itr != ProfessionTrainerLevels.end() && player->GetLevel() >= itr->second;
}

void RefreshProfessionAchievements(Player* player)
{
    if (!Challenges::Has(player, Challenges::CHALLENGE_HARDCORE))
        return;

    for (AchievementCriteriaTypes type : { ACHIEVEMENT_CRITERIA_TYPE_REACH_SKILL_LEVEL,
        ACHIEVEMENT_CRITERIA_TYPE_LEARN_SKILL_LEVEL })
    {
        // Core skips already-complete criteria even when their achievement is still unawarded.
        // Rebuild only unawarded profession counters from the character's current synced skill.
        if (auto const* criteria = sAchievementMgr->GetAchievementCriteriaByType(type))
            for (auto const* entry : *criteria)
            {
                uint32 skillId = type == ACHIEVEMENT_CRITERIA_TYPE_REACH_SKILL_LEVEL
                    ? entry->reach_skill_level.skillID : entry->learn_skill_level.skillID;
                if (GetProfessionDefinition(skillId) && !player->HasAchieved(entry->referredAchievement))
                    player->GetAchievementMgr()->RemoveCriteriaProgress(entry);
            }
        player->UpdateAchievementCriteria(type);
    }
}

class HardcoreProfessionWorldScript : public WorldScript
{
public:
    HardcoreProfessionWorldScript() : WorldScript("HardcoreProfessionWorldScript") { }
    void OnStartup() override { LoadProfessionTrainerLevels(); }
};

class HardcoreProfessionPlayerScript : public PlayerScript
{
public:
    HardcoreProfessionPlayerScript() : PlayerScript("HardcoreProfessionPlayerScript", {
        PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_BEFORE_CRITERIA_PROGRESS
    }) { }
    void OnPlayerLogin(Player* player) override { RefreshProfessionAchievements(player); }
    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override { RefreshProfessionAchievements(player); }
    bool OnPlayerBeforeCriteriaProgress(Player* player, AchievementCriteriaEntry const* criteria) override
    {
        return MeetsProfessionAchievementLevel(player, criteria);
    }
};

class HardcoreProfessionAchievementScript : public AchievementScript
{
public:
    HardcoreProfessionAchievementScript() : AchievementScript("HardcoreProfessionAchievementScript", {
        ACHIEVEMENTHOOK_IS_COMPLETED_CRITERIA
    }) { }
    bool IsCompletedCriteria(AchievementMgr* mgr, AchievementCriteriaEntry const* criteria,
        AchievementEntry const* /*achievement*/, CriteriaProgress const* /*progress*/) override
    {
        return MeetsProfessionAchievementLevel(mgr->GetPlayer(), criteria);
    }
};
}

void AddHardcoreProfessionAchievementScripts()
{
    new HardcoreProfessionWorldScript();
    new HardcoreProfessionPlayerScript();
    new HardcoreProfessionAchievementScript();
}
