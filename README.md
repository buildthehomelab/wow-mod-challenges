# mod-challenges

An [AzerothCore](https://www.azerothcore.org/) module (WotLK 3.3.5a) that adds opt-in challenges a
character takes on at the start of its life:

| Challenge | Rule | Ends on death |
|-----------|------|---------------|
| **Hardcore** | One life. The first death is announced to the realm. Mail, trade and groups only within 5 levels, no auction house, no Dungeon Finder, no experience above 1x. Can be finished at 60, 70 or 80. Account-wide titles. | yes |
| **Iron Man** | The classic Iron Man ruleset: grey and white gear only, no talents, no professions, no enchants, no potions, elixirs, flasks, scrolls or buff food. | yes |
| **Self-Found** | Everything comes from the character itself: no trading, auction house, guild bank, player mail or groups. | no |
| **Self-Crafted** | Only gear the character made itself can be put on. | no |
| **Slow XP** | Half the normal experience (configurable). | no |
| **Quest XP Only** | Experience from quests only. | no |
| **Item Quality Level** | Grey and white gear only. | no |
| **Boaring Adventure** | Experience from killing boars only, until level 60. Herds of boars only its characters see fill the levels and zones the world has none for. A title and a mount at 60. | no |
| **Exhaustion** | No rested experience. | no |

Challenges can be combined, except that Iron Man can't be paired with Self-Crafted or Item Quality
Level, nor Boaring Adventure with Quest XP Only. A challenge can't be put down once taken. When a death ends Hardcore or Iron Man, the
character plays on without it, and that challenge can't be taken again on that character.

## Taking a challenge

Each starting area has a **Shrine of Challenge** by its graveyard, and so does Acherus. Only
characters still at their starting level (1, or 55 for Death Knights) can see it. Its menu asks
for confirmation before a challenge is taken.

A new character is also offered the challenges in a window the first time it enters the world,
once any intro cinematic is over (`Challenges.FirstLoginWindow`). It has the same choices as the
shrine and a "Not now" that leaves the shrine as the way back. The window comes from the server,
so no client patch or addon is needed. The character creation screen itself can't be used: the
3.3.5a client's glue screens have no way to send a choice to the server.

The same works by command:

```
.challenge status          what is running on this character, or on the selected player
.challenge finish          finishes Hardcore once a milestone is reached; add "confirm"
.challenge hardcore        shows the rules; add "confirm" to take it
.challenge ironman
.challenge selffound
.challenge selfcrafted
.challenge slowxp
.challenge questxp
.challenge itemquality
.challenge boaring
.challenge exhaustion
```

## What counts as a death

A death ends Hardcore and Iron Man, whatever the cause: a creature, another player, a fall or
drowning. It ends them even when a resurrection follows, so a soulstone or a group's battle rez
does not save the challenge. Deaths in battlegrounds and arenas don't count unless
`Challenges.BattlegroundDeathsCount = 1`. Wintergrasp is a world zone, not a battleground, so
deaths there count. Duels never kill.

The realm is told who fell, at what level, to what, and where. The record stays in the characters
table `mod_challenges_character`, along with when each challenge was taken and when the milestone
level was reached.

## Hardcore details

- **Level range**: a Hardcore character mails, trades and groups only with characters within 5
  levels (`Challenges.Hardcore.LevelRange`), bots included, and nobody further away can mail,
  trade or group with it. Invites and accepts are refused; a member playerbots adds directly is
  taken out again. A member who levels out of range leaves the group; if that member leads it,
  the members now out of range leave instead.
- **Closed**: the auction house (window and bids) and the Dungeon Finder: no queueing, and no
  joining a Dungeon Finder group by invite. A group with a Hardcore member can't queue either.
  Other modules' auctions (the Black Market) close through `Challenges::IsAuctionClosed(guid)`.
- **Experience**: never above 1x (`Challenges.Hardcore.MaxXPRate`). Modules that raise it ask
  `Challenges::XpRateCap(guid)`: mod-individual-xp's personal rate (the DragonUI slider and `.xp`),
  mod-forever-dungeon-xp's dungeon quest bonus and mod-guild-levels' Fast Track perk. The core's
  experience auras (heirlooms) are held to the cap by the module itself. The character's own rate
  setting comes back once Hardcore ends.
- **Milestones**: reaching level 60, 70 and 80 alive is announced, and a window asks whether to
  finish there or keep going (`Challenges.Hardcore.Milestones`). From the first milestone on,
  `.challenge finish` also finishes it. Finishing lifts every Hardcore rule, is announced, and
  can't be undone.
- **Death**: the challenge ends and the character plays on as a normal character.
- **Titles**: each comes with an achievement in Feats of Strength, and both are account-wide:
  once a character earns one, every character of the account has it from its next login (new
  characters and bot alts included), and it stays when the character that earned it is deleted.

| Achievement | Title | For |
|-------------|-------|-----|
| Immortal | *Name* the Immortal | reaching 60 alive |
| Deathless | *Name* the Deathless | reaching 70 alive |
| Eternal | *Name* the Eternal | reaching 80 alive |
| Ash | *Name* of the Ash | falling at level 10-19 |
| Ember | *Name* of the Ember | falling at 20-29 |
| Flame | *Name* of the Flame | falling at 30-39 |
| Blaze | *Name* of the Blaze | falling at 40-49 |
| Hellfire | *Name* of the Hellfire | falling at 50-59 |
| Inferno | *Name* of the Inferno | falling at 60-69 |
| Firestorm | *Name* of the Firestorm | falling at 70-79 |
| Phoenix | *Name* of the Phoenix | falling at 80 |

A milestone title is earned on reaching the level, so playing on risks nothing already earned.
The achievements and titles need rows in the client's spell patch; see **Client patch** below.

## Iron Man details

Iron Man can't be taken while the character knows a profession, wears gear that breaks its rules,
has a second talent specialization, or has a trade window open. A talent point spent before the
challenge is refunded.

- **Gear**: items above white quality can't be equipped, and neither can items with a permanent
  enchant or a gem on them. Ammo above white can't be loaded. Bags and quivers are exempt.
- **Talents**: none can be learned. Points still add up and become usable if the challenge ends.
- **Professions**: no primary or secondary profession can be learned, First Aid, Cooking and
  Fishing included. Trainers show them as unavailable. Class tools the core files as trade skills
  (Runeforging, Poisons) still work. Custom professions from other modules can be added with
  `Challenges.IronMan.ExtraProfessionSkills`. That blocks their spells; a module that sets its
  skill without a spell has to check for Iron Man itself.
- **Enchants**: gear with a permanent enchant or a gem can't be worn (see Gear). Sharpening
  stones, weightstones, oils, armor kits and the Zul'Gurub class enchants can't be used, and the
  trade window's "will not be traded" slot is refused, so nobody else can enchant the gear either.
  Weapon imbues, rogue poisons and Death Knight runes are class abilities and still work.
- **Consumables**: potions, elixirs, flasks, scrolls, glyphs and food that gives Well Fed can't be
  used. Plain food and drink are fine, and so is a potion a quest hands out to be used.
- **Playerbots**: an Iron Man logged in as an alt bot still can't learn talents or wear banned
  gear, but the bot "maintenance" command puts enchants, gems and glyphs on directly. Those come
  off again when the character logs in, levels up or changes map. Turning off
  `AiPlayerbot.AltMaintenanceGemsEnchants` and `AiPlayerbot.AltMaintenanceGlyphs` avoids it.
- **Groups**: allowed by default. `Challenges.IronMan.AllowGroups = 0` enforces the classic
  solo rule, the same way as for Self-Found below.

## Self-Found details

- **Closed**: the trade window (both ways), the auction house window, bidding on auctions, the
  guild bank, and player mail (sending, and receiving from players). Mail from quests and the game
  itself still arrives.
- **Groups**: none by default (`Challenges.SelfFound.AllowGroups = 0`): no invites, no Dungeon
  Finder, and Self-Found can't be taken while in a group. A Self-Found character that playerbots
  adds to a group directly is taken out of it within a moment.
- **Before it starts**: Self-Found can't be taken while the mailbox holds what another character
  sent, while the character owns an account-bound item, carries more than the start money plus
  `Challenges.SelfFound.StartMoneyAllowance` (1 gold), is mounted, or has a companion out. Mounts
  and companions it knows when it takes the challenge are given back, a vendor pet it bought
  included, since modules that share them across an account teach them at the first login. Class
  mounts stay. The account's other characters keep what they share.
- **Not covered**: a repair paid from the guild bank.
- **Other modules**: shared items that a module hands out itself (an account-wide bank, an
  account wallet, an auction of its own) need a check in that module. `src/ChallengesApi.h` gives
  them `Challenges::IsSelfFound(guid)`; see the comment at its top.

## Self-Crafted, XP and Item Quality details

- **Self-Crafted**: an item can be put on only if it carries this character's maker's mark. What
  the character wears when it takes the challenge stays on. Bags, quivers, shirts and tabards
  don't count.
- **Slow XP**: every source of experience is cut to `Challenges.SlowXP.Rate` (half by default).
  Rested experience follows from the cut amount.
- **Quest XP Only**: kills, exploring and battlegrounds give no experience. A hunter's pet still
  gets its share from kills, so it keeps up.
- **Item Quality Level**: items above white quality can't be equipped. Unlike Iron Man, enchants
  and everything else are allowed.

## Boaring Adventure details

Inspired by South Park's *Make Love, Not Warcraft*: level up by killing boars and nothing else.

- **What counts**: kills of creatures in the Boar beast family, and of anything wearing the boar or
  fel boar model (`Challenges.Boaring.BoarModels`): Helboars, Felboars, the undead Battle Boars,
  Swine. Quilboars are people, not boars. Group kills count; each member gets their share as usual.
- **No other experience**: quests, exploring, battlegrounds and every other kill give none. A
  hunter's pet still gets its share from every kill, so it keeps up.
- **Herds**: stock boars leave whole level ranges and start zones without any: none outside dungeons
  from level 18 to 47, none in Tirisfal, Teldrassil, Eversong or Azuremyst, none in the Barrens.
  The module adds 935 boars in 50 level bands, from piglets in every starting area to level 60
  in the Plaguelands, Winterspring and Silithus, in small herds where each zone's own wildlife
  roams. They stand in a phase of their own (bit 26), which only Boaring Adventure characters
  are in, through a hidden aura (spell 90700): nobody else sees them, bots included, so the
  challenge's characters have them to themselves. The aura is put back on at login, after a map
  change, and taken off when the challenge is done. A group member outside the phase gets no
  experience from a herd boar, but still counts in the group's split, and the group's loot rules
  can hand a corpse to someone who can't see it: group with other boar hunters.

| Levels | Herds (zone) |
|--------|--------------|
| 1-10 | Elwynn Forest, Tirisfal Glades, Teldrassil, Eversong Woods, Azuremyst Isle (two bands each) |
| 10-18 | The Barrens, Silverpine Forest, Darkshore, Bloodmyst Isle, Ghostlands (two bands each) |
| 17-29 | Redridge, Ashenvale, Wetlands, Stonetalon, Duskwood, Hillsbrad, Thousand Needles |
| 28-41 | Stranglethorn (two bands), Arathi, Desolace, Alterac, Badlands, Swamp of Sorrows, Dustwallow |
| 41-55 | Feralas, Hinterlands, Tanaris, Searing Gorge, Un'Goro, Felwood, Blasted Lands, Azshara, Burning Steppes, Western Plaguelands |
| 54-60 | Winterspring (two bands), Eastern Plaguelands (two bands), Silithus |

  Stock boars fill the rest: Dun Morogh, Durotar, Mulgore, Westfall and Loch Modan up to 17, the
  Ashmane Boars of the Blasted Lands, and Hellfire's Helboars past 60. `tools/build_herds.py` writes
  the herds' SQL from the bands at its top and the world database (see the file).
- **Finish**: at level 60 (`Challenges.Boaring.FinishLevel`) the challenge is done. It is
  announced, and the rule lifts, since Outland and Northrend have almost no boars.
- **Rewards** at the finish, account-wide titles like Hardcore's (`Challenges.Boaring.Rewards`):

| Achievement | Reward | For |
|-------------|--------|-----|
| Hambringer | *Name* the Hambringer, and a Wild Goretusk to ride (`Challenges.Boaring.MountSpell`) | reaching 60 |
| That Which Has No Life | *Name*, That Which Has No Life, and the Sword of a Thousand Truths (`Challenges.Boaring.SwordItem`) | reaching 60 with Exhaustion and Slow XP running too |

  The Sword of a Thousand Truths is a trophy: a level 1 one-hander with 1-2 damage and no stats,
  in The Hungering Cold's look, soulbound and unique. Miscellaneous, so every class can hold it.
  It goes into the bags, or by mail when they are full. The mount is the character's own; modules
  that share mounts across an account share it like any other.

## Exhaustion details

- The character never gets rested experience: the most rest it may hold is 0, so an inn, a night
  offline or another module's rest perk never fills the bar.

## Configuration

See `conf/mod_challenges.conf.dist`. Every option has a comment.

| Key | Default | Description |
|-----|---------|-------------|
| `Challenges.Enable` | `1` | Master switch |
| `Challenges.<Name>.Enable` | `1` | Each challenge on or off (`Hardcore`, `IronMan`, `SelfFound`, `SelfCrafted`, `SlowXP`, `QuestXPOnly`, `ItemQuality`, `Boaring`, `Exhaustion`) |
| `Challenges.Announce.Start` / `.Death` / `.Milestone` | `1` | Realm-wide announcements |
| `Challenges.MilestoneLevel` | `60` | Level that counts as beating a challenge other than Hardcore (announced; it keeps running) |
| `Challenges.Hardcore.Milestones` | `"60 70 80"` | Hardcore milestones; from the first one it can be finished |
| `Challenges.Hardcore.LevelRange` | `5` | Hardcore mails, trades and groups only this close in level |
| `Challenges.Hardcore.AllowAuctionHouse` / `.AllowDungeonFinder` | `0` | Whether Hardcore may use them |
| `Challenges.Hardcore.MaxXPRate` | `1` | Highest experience multiplier for Hardcore |
| `Challenges.Hardcore.Rewards` | `1` | Hardcore's account-wide achievements and titles |
| `Challenges.LoginReminder` | `1` | Remind challengers at login |
| `Challenges.FirstLoginWindow` | `1` | Offer the challenges in a window on a new character's first login |
| `Challenges.BattlegroundDeathsCount` | `0` | Whether battleground and arena deaths end Hardcore and Iron Man |
| `Challenges.IronMan.AllowGroups` | `1` | Whether Iron Men may group |
| `Challenges.IronMan.AllowDeathKnights` | `0` | Whether Death Knights may take Iron Man |
| `Challenges.IronMan.ExtraProfessionSkills` | `""` | Extra SkillLine ids treated as professions |
| `Challenges.SelfFound.AllowGroups` | `0` | Whether Self-Found characters may group |
| `Challenges.SelfFound.StartMoneyAllowance` | `10000` | Copper on top of the start money a character may carry when it takes Self-Found |
| `Challenges.SlowXP.Rate` | `0.5` | Share of normal experience for Slow XP |
| `Challenges.Boaring.FinishLevel` | `60` | Level at which Boaring Adventure is done and rewarded; 0 = never |
| `Challenges.Boaring.Rewards` | `1` | Boaring Adventure's titles, mount and sword |
| `Challenges.Boaring.MountSpell` | `400624` | Mount spell for finishing (Wild Goretusk) |
| `Challenges.Boaring.SwordItem` | `9507000` | Item for finishing with Exhaustion and Slow XP |
| `Challenges.Boaring.BoarModels` | `"66 1631"` | Creature models that count as boars, besides the Boar family |

## Requirements

- AzerothCore WotLK (3.3.5a), a recent master: the module uses the PlayerScript hooks for
  learning spells and talents, trainer spell states, trade items and the first login.
- No addon. The achievements, titles and the sword need rows in the client's spell patch (below);
  everything else works without a client patch.
- Boaring Adventure's default mount is the Wild Goretusk of the Evermore realm's retail mount port
  (spell 400624). On another realm, set `Challenges.Boaring.MountSpell` to a mount it has.
- Optional: modules that share items across an account can keep Self-Found characters out
  through `src/ChallengesApi.h` (see above), and modules with an auction or an experience bonus
  can apply Hardcore's rules through it.
- Optional: on a core with a `characters`.`challenges` column (the Evermore core's character select
  screen), the module keeps it set for every character: the challenges running in the low 16 bits,
  the ones a death ended in the high 16, a finished one in both (a finished Hardcore is
  `0x00010001`). It fills the column from its own table at every startup. Without the column
  nothing is written. An older core's SMALLINT column has a byte each and only the first seven
  challenges; the module writes that layout there. The character select screen shows an icon for
  each challenge: Boaring Adventure a boar, Exhaustion a sleeping moon.

## Installation

```
cd modules
git clone https://github.com/buildthehomelab/wow-mod-challenges.git mod-challenges
```

Then re-run CMake and rebuild the worldserver. Clone into the `mod-challenges` folder: the loader
symbol comes from the folder name. The SQL in `data/sql` is applied by the AzerothCore updater
(`db-world` and `db-characters`).

IDs used: gameobject entry, spawn guids, npc_text and the runtime gossip menu ids 9503800-9503899
(the module uses 9503800-9503808); achievements 9260-9272 (9260-9299 reserved); CharTitles
178-190 with title bits 143-155; creature entries 9507000-9507099 (uses 9507000-9507049) and spawn
guids 9507100-9508999 (uses 9507100-9508034) for the boar herds; spell 90700 (server-side phase
aura, 90700-90709 reserved); item 9507000; phase bit 26 (`0x04000000`).

## Client patch

The achievements and titles are rows in Achievement.dbc and CharTitles.dbc, and the sword is a row
in Item.dbc. Without them the client shows nothing for them (and the sword as a question mark). `tools/build_patch.py` adds them to the realm's spell patch
and writes `data/sql/db-world/mod_challenges_world_rewards.sql` from the same table:

```
python3 tools/build_patch.py --patch patch-z80-spells-and-items.MPQ --client /path/to/WoW \
    --out patch-z80-spells-and-items-new.MPQ --sql data/sql/db-world/mod_challenges_world_rewards.sql
```

It keeps every other file of the patch as it is and every row in its place; new rows go at the
end. `--client` is only needed the first time, when the patch has no CharTitles.dbc or Item.dbc yet. It needs
[StormLib](https://github.com/ladislav-zezula/StormLib) (`STORMLIB`, default
`/usr/local/lib/libstorm.dylib`). Running it again on its own output changes nothing.

To remove the module, run the files in `data/sql/uninstall`. The characters file drops the
challenge records.

## Credits

The shrine spots by each starting graveyard come from
[mod-challenge-modes](https://github.com/ZhengPeiRu21/mod-challenge-modes) (MIT), which also
inspired this module and its list of challenges.

## License

Released under the MIT License.
