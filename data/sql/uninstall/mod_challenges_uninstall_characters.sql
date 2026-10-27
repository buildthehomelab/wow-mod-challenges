-- mod-challenges: drops the challenge records from the characters database. This deletes the
-- record of every challenge taken and every death, and the Hardcore achievements accounts have
-- earned; run it only when removing the module for good. Titles already given stay; the
-- achievements go once the world uninstall removes their rows (the core drops unknown ones).

DROP TABLE IF EXISTS `mod_challenges_character`;
DROP TABLE IF EXISTS `mod_challenges_account_reward`;

-- So the updater applies the install file again if the module comes back.
DELETE FROM `updates` WHERE `name` LIKE 'mod\_challenges\_%';
