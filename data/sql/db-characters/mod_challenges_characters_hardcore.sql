-- mod-challenges: Hardcore's milestones and account-wide achievements (characters database).
-- Runs after mod_challenges_characters.sql. Idempotent: safe to run again.
-- Undo with data/sql/uninstall/mod_challenges_uninstall_characters.sql.

-- The highest milestone a Hardcore character reached alive, for tables made before it existed
-- (mod_challenges_characters.sql makes new ones with it).
SET @has_table := (SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE()
    AND TABLE_NAME = 'mod_challenges_character');
SET @has_column := (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE()
    AND TABLE_NAME = 'mod_challenges_character' AND COLUMN_NAME = 'milestone_level');
SET @add_column := IF(@has_table = 1 AND @has_column = 0, 'ALTER TABLE `mod_challenges_character` ADD COLUMN `milestone_level` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT ''Hardcore: the highest milestone reached alive, 0 = none'' AFTER `milestone_at`', 'DO 0');
PREPARE add_column FROM @add_column;
EXECUTE add_column;
DEALLOCATE PREPARE add_column;

-- Hardcore characters that reached level 60 before milestones existed, so they aren't announced again.
UPDATE `mod_challenges_character` SET `milestone_level` = 60
WHERE `challenge` = 1 AND `state` = 0 AND `milestone_at` <> 0 AND `milestone_level` = 0;

-- Hardcore achievements an account has earned (9260-9270). Every character of the account is given
-- them as it loads; deleting the character that earned one keeps it for the account.
CREATE TABLE IF NOT EXISTS `mod_challenges_account_reward` (
  `account` INT UNSIGNED NOT NULL COMMENT 'account.id',
  `achievement` SMALLINT UNSIGNED NOT NULL,
  `guid` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'characters.guid of the character that earned it',
  `level` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'its level then',
  `earned_at` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time',
  PRIMARY KEY (`account`, `achievement`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-challenges: Hardcore achievements earned per account';
