-- mod-challenges: drops the challenge records from the characters database. This deletes the
-- record of every challenge taken and every death; run it only when removing the module for good.

DROP TABLE IF EXISTS `mod_challenges_character`;

-- So the updater applies the install file again if the module comes back.
DELETE FROM `updates` WHERE `name` LIKE 'mod\_challenges\_%';
