"""Execute the production rank/criteria policy against trainer-level fixtures."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).parents[1] / 'src/HardcoreProfessionAchievements.cpp').read_text()

def function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

harness = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <unordered_map>
#include <vector>
using uint8 = uint8_t; using uint16 = uint16_t; using uint32 = uint32_t;
enum AchievementCriteriaTypes { ACHIEVEMENT_CRITERIA_TYPE_REACH_SKILL_LEVEL = 7, ACHIEVEMENT_CRITERIA_TYPE_LEARN_SKILL_LEVEL = 40 };
namespace Challenges { constexpr int CHALLENGE_HARDCORE = 1; bool hardcore = true; template<class T> bool Has(T*, int) { return hardcore; } }
struct ProfessionDefinition { std::array<uint32, 6> RankSpells; } profession{{1,2,3,4,5,6}};
ProfessionDefinition const* GetProfessionDefinition(uint32 skill) { return skill == 171 ? &profession : nullptr; }

struct Manager;
struct Player {
    uint8 level = 1; bool awarded = false;
    uint8 GetLevel() { return level; }
    bool HasAchieved(uint32) { return awarded; }
    Manager* GetAchievementMgr();
    void UpdateAchievementCriteria(AchievementCriteriaTypes);
} player;
struct AchievementCriteriaEntry {
    int requiredType;
    struct { uint32 skillID, skillLevel; } reach_skill_level, learn_skill_level;
    uint32 referredAchievement = 100;
};
std::unordered_map<uint32, uint8> ProfessionTrainerLevels;
'''
harness += function('uint32 GetBestRankSpellForSkill(')
harness += function('bool MeetsProfessionAchievementLevel(')
harness += r'''
AchievementCriteriaEntry persisted{7, {171,450}, {0,0}};
struct Manager {
    uint32 counter = 450;
    std::vector<AchievementCriteriaEntry const*> entries{&persisted};
    auto GetAchievementCriteriaByType(AchievementCriteriaTypes type) {
        return type == ACHIEVEMENT_CRITERIA_TYPE_REACH_SKILL_LEVEL ? &entries : nullptr;
    }
    void RemoveCriteriaProgress(AchievementCriteriaEntry const*) { counter = 0; }
} manager;
Manager* sAchievementMgr = &manager;
Manager* Player::GetAchievementMgr() { return &manager; }
void Player::UpdateAchievementCriteria(AchievementCriteriaTypes type) {
    // Core skips complete stored counters before attempting new progress.
    if (type != ACHIEVEMENT_CRITERIA_TYPE_REACH_SKILL_LEVEL || manager.counter >= 450) return;
    if (MeetsProfessionAchievementLevel(this, &persisted)) { manager.counter = 450; awarded = true; }
}
'''
harness += function('void RefreshProfessionAchievements(')
harness += r'''
int main() {
    // Crafting requirements: apprentice through grand master. Fixture represents loaded world data.
    std::array<uint8, 6> levels{5,10,20,35,50,65};
    for (uint32 rank = 1; rank <= 6; ++rank) ProfessionTrainerLevels[rank] = levels[rank-1];
    for (uint32 rank = 1; rank <= 6; ++rank) {
        AchievementCriteriaEntry reach{7, {171, rank*75}, {0,0}};
        AchievementCriteriaEntry learn{40, {0,0}, {171,rank}};
        player.level = levels[rank-1]-1;
        assert(!MeetsProfessionAchievementLevel(&player, &reach));
        assert(!MeetsProfessionAchievementLevel(&player, &learn));
        player.level++;
        assert(MeetsProfessionAchievementLevel(&player, &reach));
        assert(MeetsProfessionAchievementLevel(&player, &learn));
        assert(GetBestRankSpellForSkill(171, rank*75) == rank);
        if (rank < 6) assert(GetBestRankSpellForSkill(171, rank*75+1) == rank+1);
    }
    player.level = 64;
    RefreshProfessionAchievements(&player);
    assert(manager.counter == 0 && !player.awarded);
    player.level = 65;
    RefreshProfessionAchievements(&player);
    assert(manager.counter == 450 && player.awarded);
    // Per-profession DB requirements override crafting assumptions (e.g. gathering).
    ProfessionTrainerLevels[6] = 55;
    AchievementCriteriaEntry grand{7, {171,450}, {0,0}};
    player.level = 54; assert(!MeetsProfessionAchievementLevel(&player, &grand));
    player.level = 55; assert(MeetsProfessionAchievementLevel(&player, &grand));
    ProfessionTrainerLevels.erase(6); assert(!MeetsProfessionAchievementLevel(&player, &grand));
    grand.reach_skill_level.skillID = 999;
    assert(MeetsProfessionAchievementLevel(&player, &grand));
    grand.requiredType = 5; assert(MeetsProfessionAchievementLevel(&player, &grand));
    grand.requiredType = 7; grand.reach_skill_level.skillID = 171;
    Challenges::hardcore = false;
    assert(MeetsProfessionAchievementLevel(&player, &grand));
    manager.counter = 123; player.awarded = false;
    RefreshProfessionAchievements(&player);
    assert(manager.counter == 123 && !player.awarded);
}
'''
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / 'policy.cpp'
    binary = Path(directory) / 'policy'
    cpp.write_text(harness)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Profession achievement level boundaries passed')
