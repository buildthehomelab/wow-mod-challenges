-- mod-challenges: world database.
--
-- The Shrine of Challenge: gameobject entry, spawn guids and npc_text 9503800-9503899. It stands
-- by the graveyard of each starting area and Acherus, and only characters still at their starting
-- level can see it. It uses a model the client already has (display 6925), so no client patch is
-- needed. No stock rows are changed. Idempotent: safe to run again.
-- Undo with data/sql/uninstall/mod_challenges_uninstall_world.sql.

DELETE FROM `gameobject_template` WHERE `entry` = 9503800;
INSERT INTO `gameobject_template` (`entry`, `type`, `displayId`, `name`, `IconName`, `castBarCaption`, `unk1`, `size`,
    `Data0`, `Data1`, `Data2`, `Data3`, `Data4`, `Data5`, `Data6`, `Data7`, `Data8`, `Data9`, `Data10`, `Data11`,
    `Data12`, `Data13`, `Data14`, `Data15`, `Data16`, `Data17`, `Data18`, `Data19`, `Data20`, `Data21`, `Data22`,
    `Data23`, `AIName`, `ScriptName`, `VerifiedBuild`) VALUES
(9503800, 2, 6925, 'Shrine of Challenge', '', '', '', 1.2,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '', 'go_shrine_of_challenge', 0);

DELETE FROM `npc_text` WHERE `ID` BETWEEN 9503800 AND 9503802;
INSERT INTO `npc_text` (`ID`, `text0_0`, `text0_1`, `BroadcastTextID0`, `lang0`, `Probability0`) VALUES
(9503800, 'The shrine hums as you draw near. Old marks are scratched into the stone: names of those who swore to walk Azeroth the hard way, and many of them crossed out.$B$BA challenge, once sworn, can never be taken back.', 'The shrine hums as you draw near. Old marks are scratched into the stone: names of those who swore to walk Azeroth the hard way, and many of them crossed out.$B$BA challenge, once sworn, can never be taken back.', 0, 0, 1),
(9503801, 'HARDCORE$BOne life. The first death ends the challenge, and the realm hears of it.$B$BIRON MAN$BGrey and white gear only, unenchanted. No talents, no professions. No potions, elixirs, flasks, scrolls or buff food. A death ends it.$B$BSELF-FOUND$BEverything you have, you find yourself. No trading, auction house, Black Market, guild bank, player mail, or items shared across your account. No groups.$B$BSELF-CRAFTED$BYou can only put on gear you made yourself.$B$BSLOW XP$BYou earn a fraction of the normal experience.$B$BQUEST XP ONLY$BExperience comes from quests alone.$B$BITEM QUALITY LEVEL$BGrey and white gear only.$B$BOnly Hardcore and Iron Man end with a death. Most challenges can be sworn together; Iron Man can't be paired with Self-Crafted or Item Quality Level. Challenges can only be taken at your starting level.', 'HARDCORE$BOne life. The first death ends the challenge, and the realm hears of it.$B$BIRON MAN$BGrey and white gear only, unenchanted. No talents, no professions. No potions, elixirs, flasks, scrolls or buff food. A death ends it.$B$BSELF-FOUND$BEverything you have, you find yourself. No trading, auction house, Black Market, guild bank, player mail, or items shared across your account. No groups.$B$BSELF-CRAFTED$BYou can only put on gear you made yourself.$B$BSLOW XP$BYou earn a fraction of the normal experience.$B$BQUEST XP ONLY$BExperience comes from quests alone.$B$BITEM QUALITY LEVEL$BGrey and white gear only.$B$BOnly Hardcore and Iron Man end with a death. Most challenges can be sworn together; Iron Man can't be paired with Self-Crafted or Item Quality Level. Challenges can only be taken at your starting level.', 0, 0, 1),
(9503802, 'Before your journey begins, the old powers offer a choice. Some walk Azeroth the hard way: with one life, with nothing but what they find, with the plainest of gear.$B$BSwear to a challenge now, or later at the Shrine of Challenge by your starting graveyard. Once sworn, a challenge can never be taken back, and the offer lasts only while you are at your starting level.', 'Before your journey begins, the old powers offer a choice. Some walk Azeroth the hard way: with one life, with nothing but what they find, with the plainest of gear.$B$BSwear to a challenge now, or later at the Shrine of Challenge by your starting graveyard. Once sworn, a challenge can never be taken back, and the offer lasts only while you are at your starting level.', 0, 0, 1);

-- Beside the graveyard of each starting area. Only added when the spawn isn't there yet, so a spot
-- picked in game with .gobject move survives this file running again. The Acherus shrine is in phases
-- 1-16, so Death Knights still see it once the Ebon Hold chain moves them on.
INSERT IGNORE INTO `gameobject` (`guid`, `id`, `map`, `zoneId`, `areaId`, `spawnMask`, `phaseMask`, `position_x`, `position_y`, `position_z`, `orientation`, `rotation0`, `rotation1`, `rotation2`, `rotation3`, `spawntimesecs`, `animprogress`, `state`, `Comment`) VALUES
(9503800, 9503800,   0, 0, 0, 1, 1, -8920.64,   -178.191,  80.891,  4.3208,  0, 0, -0.83116,   0.556033,  300, 0, 1, 'mod-challenges: Shrine of Challenge, Northshire'),
(9503801, 9503800,   0, 0, 0, 1, 1, -6135.29,    336.119, 402.238,  5.55195, 0, 0, -0.357526,  0.933903,  300, 0, 1, 'mod-challenges: Shrine of Challenge, Coldridge Valley'),
(9503802, 9503800,   1, 0, 0, 1, 1, 10415.2,     809.575, 1318.19, 2.37082, 0, 0, -0.926654, -0.375916,  300, 0, 1, 'mod-challenges: Shrine of Challenge, Shadowglen'),
(9503803, 9503800, 530, 0, 0, 1, 1, -4147.11, -13667.7,    75.8166, 5.06421, 0, 0, -0.572447,  0.819942,  300, 0, 1, 'mod-challenges: Shrine of Challenge, Ammen Vale'),
(9503804, 9503800,   1, 0, 0, 1, 1, -658.88,   -4311.88,   45.666,  3.06225, 0, 0, -0.999213, -0.0396603, 300, 0, 1, 'mod-challenges: Shrine of Challenge, Valley of Trials'),
(9503805, 9503800,   0, 0, 0, 1, 1, 1842.91,    1651.33,   95.6206, 1.58336, 0, 0, -0.711535, -0.702651,  300, 0, 1, 'mod-challenges: Shrine of Challenge, Deathknell'),
(9503806, 9503800,   1, 0, 0, 1, 1, -2994.22,   -136.321,  77.9491, 1.05411, 0, 0, -0.502992, -0.864291,  300, 0, 1, 'mod-challenges: Shrine of Challenge, Camp Narache'),
(9503807, 9503800, 530, 0, 0, 1, 1, 10452,     -6389.91,   43.7962, 1.84851, 0, 0, -0.798173, -0.602429,  300, 0, 1, 'mod-challenges: Shrine of Challenge, Sunstrider Isle'),
(9503808, 9503800, 609, 0, 0, 1, 65535, 2415.84,   -5649.91,  376.819,  1.87356, 0, 0, -0.805655, -0.592385,  300, 0, 1, 'mod-challenges: Shrine of Challenge, Acherus');
