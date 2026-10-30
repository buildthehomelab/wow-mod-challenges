-- mod-challenges: drops the challenge records from the characters database. This deletes the
-- record of every challenge taken and every death, and the achievements accounts have
-- earned; run it only when removing the module for good. Titles already given stay; the
-- achievements go once the world uninstall removes their rows (the core drops unknown ones).

DROP TABLE IF EXISTS `mod_challenges_character`;
DROP TABLE IF EXISTS `mod_challenges_account_reward`;

-- The Evermore core's character select marker (characters.challenges), when the core has it.
SET @has_column := (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE()
    AND TABLE_NAME = 'characters' AND COLUMN_NAME = 'challenges');
SET @clear_marker := IF(@has_column = 1, 'UPDATE `characters` SET `challenges` = 0', 'DO 0');
PREPARE clear_marker FROM @clear_marker;
EXECUTE clear_marker;
DEALLOCATE PREPARE clear_marker;

-- So the updater applies the install file again if the module comes back.
DELETE FROM `updates` WHERE `name` LIKE 'mod\_challenges\_%';
