-- mod-challenges: removes the Shrine of Challenge from the world database.

DELETE FROM `gameobject` WHERE `id` = 9503800;
DELETE FROM `gameobject_template` WHERE `entry` = 9503800;
DELETE FROM `npc_text` WHERE `ID` BETWEEN 9503800 AND 9503802;

-- So the updater applies the install file again if the module comes back.
DELETE FROM `updates` WHERE `name` LIKE 'mod\_challenges\_%';
