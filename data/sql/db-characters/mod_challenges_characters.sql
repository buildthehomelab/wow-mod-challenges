-- mod-challenges: characters database. One row per character and challenge taken; a row is never
-- removed when the challenge ends, so the table is also the record of who fell where.
-- Undo with data/sql/uninstall/mod_challenges_uninstall_characters.sql.

CREATE TABLE IF NOT EXISTS `mod_challenges_character` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
  `challenge` TINYINT UNSIGNED NOT NULL COMMENT 'bit: 1 Hardcore, 2 Iron Man, 4 Self-Found, 8 Self-Crafted, 16 Slow XP, 32 Quest XP Only, 64 Item Quality Level',
  `state` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 = running, 1 = ended by a death',
  `started_at` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time',
  `start_level` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `milestone_at` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time Challenges.MilestoneLevel was reached, 0 = not yet',
  `ended_at` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time of the death that ended it',
  `end_level` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `end_map` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `end_zone` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `end_killer` VARCHAR(100) NOT NULL DEFAULT '' COMMENT 'empty when nothing killed them (a fall, drowning)',
  PRIMARY KEY (`guid`, `challenge`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-challenges: challenges taken per character';
