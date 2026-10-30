-- mod-challenges: the challenges' account-wide achievements and their titles, and Boaring
-- Adventure's sword (world database). Written by tools/build_patch.py, together with the matching
-- rows of the client patch (patch-z80-spells-and-items.MPQ): achievements 9260-9272 in Feats of
-- Strength, CharTitles 178-190 (title bits 143-155), item 9507000. The achievements have no criteria: the
-- module grants them. Idempotent. Undo with data/sql/uninstall/mod_challenges_uninstall_world.sql.

DELETE FROM `achievement_dbc` WHERE `ID` BETWEEN 9260 AND 9272;
INSERT INTO `achievement_dbc` (`ID`, `Faction`, `Instance_Id`, `Supercedes`, `Title_Lang_enUS`, `Title_Lang_Mask`, `Description_Lang_enUS`, `Description_Lang_Mask`, `Category`, `Points`, `Ui_Order`, `Flags`, `IconID`, `Reward_Lang_enUS`, `Reward_Lang_Mask`, `Minimum_Criteria`, `Shares_Criteria`) VALUES
(9260, -1, -1, 0, 'Immortal', 16712190, 'Reach level 60 in the Hardcore challenge without dying.', 16712190, 81, 0, 1016, 0, 3273, 'Title Reward: <Name> the Immortal', 16712190, 0, 0),
(9261, -1, -1, 0, 'Deathless', 16712190, 'Reach level 70 in the Hardcore challenge without dying.', 16712190, 81, 0, 1017, 0, 3274, 'Title Reward: <Name> the Deathless', 16712190, 0, 0),
(9262, -1, -1, 0, 'Eternal', 16712190, 'Reach level 80 in the Hardcore challenge without dying.', 16712190, 81, 0, 1018, 0, 3275, 'Title Reward: <Name> the Eternal', 16712190, 0, 0),
(9263, -1, -1, 0, 'Ash', 16712190, 'Fall in the Hardcore challenge at level 10 to 19.', 16712190, 81, 0, 1019, 0, 177, 'Title Reward: <Name> of the Ash', 16712190, 0, 0),
(9264, -1, -1, 0, 'Ember', 16712190, 'Fall in the Hardcore challenge at level 20 to 29.', 16712190, 81, 0, 1020, 0, 2128, 'Title Reward: <Name> of the Ember', 16712190, 0, 0),
(9265, -1, -1, 0, 'Flame', 16712190, 'Fall in the Hardcore challenge at level 30 to 39.', 16712190, 81, 0, 1021, 0, 185, 'Title Reward: <Name> of the Flame', 16712190, 0, 0),
(9266, -1, -1, 0, 'Blaze', 16712190, 'Fall in the Hardcore challenge at level 40 to 49.', 16712190, 81, 0, 1022, 0, 937, 'Title Reward: <Name> of the Blaze', 16712190, 0, 0),
(9267, -1, -1, 0, 'Hellfire', 16712190, 'Fall in the Hardcore challenge at level 50 to 59.', 16712190, 81, 0, 1023, 0, 2369, 'Title Reward: <Name> of the Hellfire', 16712190, 0, 0),
(9268, -1, -1, 0, 'Inferno', 16712190, 'Fall in the Hardcore challenge at level 60 to 69.', 16712190, 81, 0, 1024, 0, 1137, 'Title Reward: <Name> of the Inferno', 16712190, 0, 0),
(9269, -1, -1, 0, 'Firestorm', 16712190, 'Fall in the Hardcore challenge at level 70 to 79.', 16712190, 81, 0, 1025, 0, 1930, 'Title Reward: <Name> of the Firestorm', 16712190, 0, 0),
(9270, -1, -1, 0, 'Phoenix', 16712190, 'Fall in the Hardcore challenge at level 80.', 16712190, 81, 0, 1026, 0, 90221, 'Title Reward: <Name> of the Phoenix', 16712190, 0, 0),
(9271, -1, -1, 0, 'Hambringer', 16712190, 'Reach level 60 in Boaring Adventure, killing nothing but boars.', 16712190, 81, 0, 1027, 0, 1578, 'Title Reward: <Name> the Hambringer', 16712190, 0, 0),
(9272, -1, -1, 0, 'That Which Has No Life', 16712190, 'Reach level 60 in Boaring Adventure together with Exhaustion and Slow XP.', 16712190, 81, 0, 1028, 0, 2971, 'Title Reward: <Name>, That Which Has No Life', 16712190, 0, 0);

DELETE FROM `chartitles_dbc` WHERE `ID` BETWEEN 178 AND 190;
INSERT INTO `chartitles_dbc` (`ID`, `Condition_ID`, `Name_Lang_enUS`, `Name_Lang_Mask`, `Name1_Lang_enUS`, `Name1_Lang_Mask`, `Mask_ID`) VALUES
(178, 0, '%s the Immortal', 16712190, '%s the Immortal', 16712190, 143),
(179, 0, '%s the Deathless', 16712190, '%s the Deathless', 16712190, 144),
(180, 0, '%s the Eternal', 16712190, '%s the Eternal', 16712190, 145),
(181, 0, '%s of the Ash', 16712190, '%s of the Ash', 16712190, 146),
(182, 0, '%s of the Ember', 16712190, '%s of the Ember', 16712190, 147),
(183, 0, '%s of the Flame', 16712190, '%s of the Flame', 16712190, 148),
(184, 0, '%s of the Blaze', 16712190, '%s of the Blaze', 16712190, 149),
(185, 0, '%s of the Hellfire', 16712190, '%s of the Hellfire', 16712190, 150),
(186, 0, '%s of the Inferno', 16712190, '%s of the Inferno', 16712190, 151),
(187, 0, '%s of the Firestorm', 16712190, '%s of the Firestorm', 16712190, 152),
(188, 0, '%s of the Phoenix', 16712190, '%s of the Phoenix', 16712190, 153),
(189, 0, '%s the Hambringer', 16712190, '%s the Hambringer', 16712190, 154),
(190, 0, '%s, That Which Has No Life', 16712190, '%s, That Which Has No Life', 16712190, 155);

DELETE FROM `achievement_reward` WHERE `ID` BETWEEN 9260 AND 9272;
INSERT INTO `achievement_reward` (`ID`, `TitleA`, `TitleH`, `ItemID`, `Sender`, `Subject`, `Body`, `MailTemplateID`) VALUES
(9260, 178, 178, 0, 0, NULL, NULL, 0),
(9261, 179, 179, 0, 0, NULL, NULL, 0),
(9262, 180, 180, 0, 0, NULL, NULL, 0),
(9263, 181, 181, 0, 0, NULL, NULL, 0),
(9264, 182, 182, 0, 0, NULL, NULL, 0),
(9265, 183, 183, 0, 0, NULL, NULL, 0),
(9266, 184, 184, 0, 0, NULL, NULL, 0),
(9267, 185, 185, 0, 0, NULL, NULL, 0),
(9268, 186, 186, 0, 0, NULL, NULL, 0),
(9269, 187, 187, 0, 0, NULL, NULL, 0),
(9270, 188, 188, 0, 0, NULL, NULL, 0),
(9271, 189, 189, 0, 0, NULL, NULL, 0),
(9272, 190, 190, 0, 0, NULL, NULL, 0);

-- The Sword of a Thousand Truths: a trophy, not a weapon. Level 1, 1-2 damage, no stats, soulbound and
-- unique, in The Hungering Cold's look. Miscellaneous, so every class can hold it.
DELETE FROM `item_dbc` WHERE `ID` = 9507000;
INSERT INTO `item_dbc` (`ID`, `ClassID`, `SubclassID`, `Sound_Override_Subclassid`, `Material`, `DisplayInfoID`, `InventoryType`, `SheatheType`) VALUES (9507000, 2, 14, 7, 1, 35575, 13, 3);
DELETE FROM `item_template` WHERE `entry` = 9507000;
INSERT INTO `item_template` (`entry`, `class`, `subclass`, `SoundOverrideSubclass`, `name`, `displayid`, `Quality`, `BuyCount`, `BuyPrice`, `SellPrice`, `InventoryType`, `AllowableClass`, `AllowableRace`, `ItemLevel`, `RequiredLevel`, `maxcount`, `stackable`, `dmg_min1`, `dmg_max1`, `delay`, `bonding`, `description`, `Material`, `sheath`, `MaxDurability`, `VerifiedBuild`) VALUES
(9507000, 2, 14, 7, 'Sword of a Thousand Truths', 35575, 5, 1, 17, 3, 13, -1, -1, 1, 1, 1, 1, 1, 2, 2300, 1, 'It was foretold, that one day, heroes who could wield the sword might reveal themselves.', 1, 3, 20, 0);
