-- mod-challenges: removes the Shrine of Challenge and Hardcore's achievements and titles from the
-- world database. The client patch keeps its rows; they do nothing without these.

DELETE FROM `gameobject` WHERE `id` = 9503800;
DELETE FROM `gameobject_template` WHERE `entry` = 9503800;
DELETE FROM `npc_text` WHERE `ID` BETWEEN 9503800 AND 9503803;
DELETE FROM `achievement_reward` WHERE `ID` BETWEEN 9260 AND 9270;
DELETE FROM `achievement_dbc` WHERE `ID` BETWEEN 9260 AND 9270;
DELETE FROM `chartitles_dbc` WHERE `ID` BETWEEN 178 AND 188;

-- So the updater applies the install file again if the module comes back.
DELETE FROM `updates` WHERE `name` LIKE 'mod\_challenges\_%';
