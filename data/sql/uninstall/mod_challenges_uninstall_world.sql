-- mod-challenges: removes the Shrine of Challenge, Boaring Adventure's herds and the challenges'
-- achievements, titles and items from the world database. The client patch keeps its rows; they do
-- nothing without these.

DELETE FROM `gameobject` WHERE `id` = 9503800;
DELETE FROM `gameobject_template` WHERE `entry` = 9503800;
DELETE FROM `npc_text` WHERE `ID` BETWEEN 9503800 AND 9503803;
DELETE FROM `achievement_reward` WHERE `ID` BETWEEN 9260 AND 9272;
DELETE FROM `achievement_dbc` WHERE `ID` BETWEEN 9260 AND 9272;
DELETE FROM `chartitles_dbc` WHERE `ID` BETWEEN 178 AND 190;

DELETE FROM `creature` WHERE `guid` BETWEEN 9507100 AND 9508999;
DELETE FROM `smart_scripts` WHERE `entryorguid` BETWEEN 9507000 AND 9507099 AND `source_type` = 0;
DELETE FROM `creature_template_model` WHERE `CreatureID` BETWEEN 9507000 AND 9507099;
DELETE FROM `creature_template_movement` WHERE `CreatureId` BETWEEN 9507000 AND 9507099;
DELETE FROM `creature_template` WHERE `entry` BETWEEN 9507000 AND 9507099;
DELETE FROM `spell_dbc` WHERE `ID` = 90700;
DELETE FROM `item_template` WHERE `entry` = 9507000;
DELETE FROM `item_dbc` WHERE `ID` = 9507000;

-- So the updater applies the install file again if the module comes back.
DELETE FROM `updates` WHERE `name` LIKE 'mod\_challenges\_%';
