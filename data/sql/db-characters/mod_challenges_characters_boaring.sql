-- mod-challenges: room for Boaring Adventure (128) and Exhaustion (256) (characters database).
-- Runs after mod_challenges_characters.sql. Idempotent: safe to run again.
-- Undo with data/sql/uninstall/mod_challenges_uninstall_characters.sql.

ALTER TABLE `mod_challenges_character` MODIFY COLUMN `challenge` SMALLINT UNSIGNED NOT NULL COMMENT 'bit: 1 Hardcore, 2 Iron Man, 4 Self-Found, 8 Self-Crafted, 16 Slow XP, 32 Quest XP Only, 64 Item Quality Level, 128 Boaring Adventure, 256 Exhaustion';
