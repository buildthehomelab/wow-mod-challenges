/*
 * mod-challenges loader.
 *
 * AzerothCore looks up a loader symbol derived from the module's folder name: for folder
 * "mod-challenges" that symbol is exactly "Addmod_challengesScripts". If you clone the repo under
 * a different folder name, rename this function to match.
 *
 * Released under the MIT License.
 */

void AddChallengesScripts();
void AddHardcoreProfessionAchievementScripts();

void Addmod_challengesScripts()
{
    AddChallengesScripts();
    AddHardcoreProfessionAchievementScripts();
}
