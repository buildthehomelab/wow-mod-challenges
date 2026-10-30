"""Adds the challenges' account-wide achievements and their titles (Hardcore, Boaring Adventure) and
Boaring Adventure's Sword of a Thousand Truths to the realm's spell patch, and writes the matching
world SQL.

    python3 build_patch.py --patch patch-z80-spells-and-items.MPQ --client /path/to/Evermore \\
        --out patch-z80-spells-and-items-new.MPQ --sql ../data/sql/db-world/mod_challenges_world_rewards.sql

Achievement.dbc gets 13 rows in Feats of Strength (it lists only what a character has earned),
CharTitles.dbc gets one title for each, and Item.dbc gets the sword (its look is The Hungering
Cold's, display 35575). The patch has no CharTitles.dbc of its own until the first
build, so that one starts from the client's newest locale copy (--client). A patch MPQ replaces
whole files, so every other file of the patch is carried over unchanged. The rows keep the files'
own order: new rows go at the end, and a rerun rewrites ours where they are (see the
achievement series note in dbcmpq.py). Run it again on its own output and nothing changes.

Ids: achievements 9260-9272, CharTitles 178-190 with title bits 143-155 (the 3.3.5a client stops
at 177 and bit 142; the core keeps 192 title bits), item 9507000.
"""

import argparse
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dbcmpq  # noqa: E402
from dbcmpq import add_string, read_dbc, string_at, write_dbc  # noqa: E402

LANG_MASK = 16712190                       # the enUS string plus the flags every stock row has
CATEGORY_FEATS_OF_STRENGTH = 81
ACHIEVEMENT_FIELDS, CHARTITLES_FIELDS = 62, 37
# Achievement.dbc: ID, Faction, Instance, Supercedes, Title[17], Description[17], Category, Points,
# Ui_Order, Flags, IconID, Reward[17], Minimum_Criteria, Shares_Criteria.
A_FACTION, A_INSTANCE, A_TITLE, A_DESCRIPTION = 1, 2, 4, 21
A_CATEGORY, A_POINTS, A_UI_ORDER, A_FLAGS, A_ICON, A_REWARD = 38, 39, 40, 41, 42, 43
# CharTitles.dbc: ID, Condition_ID, Name[17], Name1[17], Mask_ID.
T_NAME, T_NAME1, T_MASK_ID = 2, 19, 36
UNSET = 0xFFFFFFFF                         # -1: either faction, any map

# (achievement, title id, title bit, name, title, icon, description)
REWARDS = [
    (9260, 178, 143, "Immortal", "%s the Immortal", 3273, "Reach level 60 in the Hardcore challenge without dying."),
    (9261, 179, 144, "Deathless", "%s the Deathless", 3274, "Reach level 70 in the Hardcore challenge without dying."),
    (9262, 180, 145, "Eternal", "%s the Eternal", 3275, "Reach level 80 in the Hardcore challenge without dying."),
    (9263, 181, 146, "Ash", "%s of the Ash", 177, "Fall in the Hardcore challenge at level 10 to 19."),
    (9264, 182, 147, "Ember", "%s of the Ember", 2128, "Fall in the Hardcore challenge at level 20 to 29."),
    (9265, 183, 148, "Flame", "%s of the Flame", 185, "Fall in the Hardcore challenge at level 30 to 39."),
    (9266, 184, 149, "Blaze", "%s of the Blaze", 937, "Fall in the Hardcore challenge at level 40 to 49."),
    (9267, 185, 150, "Hellfire", "%s of the Hellfire", 2369, "Fall in the Hardcore challenge at level 50 to 59."),
    (9268, 186, 151, "Inferno", "%s of the Inferno", 1137, "Fall in the Hardcore challenge at level 60 to 69."),
    (9269, 187, 152, "Firestorm", "%s of the Firestorm", 1930, "Fall in the Hardcore challenge at level 70 to 79."),
    (9270, 188, 153, "Phoenix", "%s of the Phoenix", 90221, "Fall in the Hardcore challenge at level 80."),
    (9271, 189, 154, "Hambringer", "%s the Hambringer", 1578, "Reach level 60 in Boaring Adventure, killing nothing but boars."),
    (9272, 190, 155, "That Which Has No Life", "%s, That Which Has No Life", 2971,
     "Reach level 60 in Boaring Adventure together with Exhaustion and Slow XP."),
]
FIRST_ACHIEVEMENT, LAST_ACHIEVEMENT = REWARDS[0][0], REWARDS[-1][0]
FIRST_TITLE, LAST_TITLE = REWARDS[0][1], REWARDS[-1][1]

# Item.dbc: ID, Class, Subclass, Sound_Override_Subclass, Material, DisplayInfo, InventoryType, SheatheType.
SWORD_ITEM = 9507000
SWORD_DBC = [SWORD_ITEM, 2, 14, 7, 1, 35575, 13, 3]  # weapon, miscellaneous (anyone can wield it), sword sounds
SWORD_NAME = "Sword of a Thousand Truths"
SWORD_TEXT = "It was foretold, that one day, heroes who could wield the sword might reveal themselves."


def dbc_name(table):
    return "DBFilesClient\\%s.dbc" % table


def find_file(files, name):
    for archived, local in files.items():
        if archived.lower() == name.lower():
            return archived, local
    return None, None


def put(rows, row):
    """Replaces the row with the same id where it stands, or appends it."""
    for i, r in enumerate(rows):
        if r[0] == row[0]:
            rows[i] = row
            return
    rows.append(row)


def reward_text(title):
    return "Title Reward: " + title.replace("%s", "<Name>")


def patch_achievements(path):
    rows, strings, fields = read_dbc(path)
    if fields != ACHIEVEMENT_FIELDS:
        sys.exit("Achievement.dbc has %d fields, not %d" % (fields, ACHIEVEMENT_FIELDS))
    ours = {r[0] for r in REWARDS}
    taken = [r[0] for r in rows if 9260 <= r[0] <= 9299 and r[0] not in ours]
    if taken:
        sys.exit("Achievement.dbc already has rows in 9260-9299 that aren't ours: %s" % taken)

    feats = [r[A_UI_ORDER] for r in rows if r[A_CATEGORY] == CATEGORY_FEATS_OF_STRENGTH and r[0] not in ours]
    order = max(feats) + 1
    for achievement, _title_id, _bit, name, title, icon, description in REWARDS:
        row = [0] * fields
        row[0] = achievement
        row[A_FACTION] = UNSET
        row[A_INSTANCE] = UNSET
        row[A_TITLE] = add_string(strings, name)
        row[A_TITLE + 16] = LANG_MASK
        row[A_DESCRIPTION] = add_string(strings, description)
        row[A_DESCRIPTION + 16] = LANG_MASK
        row[A_CATEGORY] = CATEGORY_FEATS_OF_STRENGTH
        row[A_UI_ORDER] = order
        row[A_ICON] = icon
        row[A_REWARD] = add_string(strings, reward_text(title))
        row[A_REWARD + 16] = LANG_MASK
        put(rows, row)
        order += 1
    write_dbc(path, rows, strings, fields)
    return {r[0]: r[A_UI_ORDER] for r in rows if r[0] in ours}


def patch_items(path):
    rows, strings, fields = read_dbc(path)
    if fields != len(SWORD_DBC):
        sys.exit("Item.dbc has %d fields, not %d" % (fields, len(SWORD_DBC)))
    put(rows, list(SWORD_DBC))
    write_dbc(path, rows, strings, fields)


def patch_titles(path):
    rows, strings, fields = read_dbc(path)
    if fields != CHARTITLES_FIELDS:
        sys.exit("CharTitles.dbc has %d fields, not %d" % (fields, CHARTITLES_FIELDS))
    ours = {r[1] for r in REWARDS}
    for r in rows:
        if r[0] in ours:
            continue
        if r[0] >= 178 or r[T_MASK_ID] >= 143:
            sys.exit("CharTitles.dbc row %d (bit %d, %s) is in our range" % (r[0], r[T_MASK_ID], string_at(strings, r[T_NAME])))
    for _achievement, title_id, bit, _name, title, _icon, _description in REWARDS:
        row = [0] * fields
        row[0] = title_id
        row[T_NAME] = add_string(strings, title)
        row[T_NAME + 16] = LANG_MASK
        row[T_NAME1] = add_string(strings, title)
        row[T_NAME1 + 16] = LANG_MASK
        row[T_MASK_ID] = bit
        put(rows, row)
    write_dbc(path, rows, strings, fields)


def sql_str(s):
    return "'" + s.replace("\\", "\\\\").replace("'", "\\'") + "'"


def write_sql(path, order):
    def i32(v):
        return struct.unpack("<i", struct.pack("<I", v))[0]

    lines = [
        "-- mod-challenges: the challenges' account-wide achievements and their titles, and Boaring",
        "-- Adventure's sword (world database). Written by tools/build_patch.py, together with the matching",
        "-- rows of the client patch (patch-z80-spells-and-items.MPQ): achievements %d-%d in Feats of" % (FIRST_ACHIEVEMENT, LAST_ACHIEVEMENT),
        "-- Strength, CharTitles %d-%d (title bits %d-%d), item %d. The achievements have no criteria: the" % (
            FIRST_TITLE, LAST_TITLE, REWARDS[0][2], REWARDS[-1][2], SWORD_ITEM),
        "-- module grants them. Idempotent. Undo with data/sql/uninstall/mod_challenges_uninstall_world.sql.",
        "",
        "DELETE FROM `achievement_dbc` WHERE `ID` BETWEEN %d AND %d;" % (FIRST_ACHIEVEMENT, LAST_ACHIEVEMENT),
        "INSERT INTO `achievement_dbc` (`ID`, `Faction`, `Instance_Id`, `Supercedes`, `Title_Lang_enUS`, `Title_Lang_Mask`, "
        "`Description_Lang_enUS`, `Description_Lang_Mask`, `Category`, `Points`, `Ui_Order`, `Flags`, `IconID`, `Reward_Lang_enUS`, "
        "`Reward_Lang_Mask`, `Minimum_Criteria`, `Shares_Criteria`) VALUES",
    ]
    lines.append(",\n".join("(%d, %d, %d, 0, %s, %d, %s, %d, %d, 0, %d, 0, %d, %s, %d, 0, 0)" % (
        a, i32(UNSET), i32(UNSET), sql_str(name), LANG_MASK, sql_str(description), LANG_MASK, CATEGORY_FEATS_OF_STRENGTH,
        order[a], icon, sql_str(reward_text(title)), LANG_MASK)
        for a, _t, _b, name, title, icon, description in REWARDS) + ";")
    lines += [
        "",
        "DELETE FROM `chartitles_dbc` WHERE `ID` BETWEEN %d AND %d;" % (FIRST_TITLE, LAST_TITLE),
        "INSERT INTO `chartitles_dbc` (`ID`, `Condition_ID`, `Name_Lang_enUS`, `Name_Lang_Mask`, `Name1_Lang_enUS`, "
        "`Name1_Lang_Mask`, `Mask_ID`) VALUES",
    ]
    lines.append(",\n".join("(%d, 0, %s, %d, %s, %d, %d)" % (t, sql_str(title), LANG_MASK, sql_str(title), LANG_MASK, b)
                            for _a, t, b, _n, title, _i, _d in REWARDS) + ";")
    lines += [
        "",
        "DELETE FROM `achievement_reward` WHERE `ID` BETWEEN %d AND %d;" % (FIRST_ACHIEVEMENT, LAST_ACHIEVEMENT),
        "INSERT INTO `achievement_reward` (`ID`, `TitleA`, `TitleH`, `ItemID`, `Sender`, `Subject`, `Body`, `MailTemplateID`) VALUES",
    ]
    lines.append(",\n".join("(%d, %d, %d, 0, 0, NULL, NULL, 0)" % (a, t, t) for a, t, _b, _n, _ti, _i, _d in REWARDS) + ";")
    lines += [
        "",
        "-- The Sword of a Thousand Truths: a trophy, not a weapon. Level 1, 1-2 damage, no stats, soulbound and",
        "-- unique, in The Hungering Cold's look. Miscellaneous, so every class can hold it.",
        "DELETE FROM `item_dbc` WHERE `ID` = %d;" % SWORD_ITEM,
        "INSERT INTO `item_dbc` (`ID`, `ClassID`, `SubclassID`, `Sound_Override_Subclassid`, `Material`, `DisplayInfoID`, "
        "`InventoryType`, `SheatheType`) VALUES (%s);" % ", ".join(str(v) for v in SWORD_DBC),
        "DELETE FROM `item_template` WHERE `entry` = %d;" % SWORD_ITEM,
        "INSERT INTO `item_template` (`entry`, `class`, `subclass`, `SoundOverrideSubclass`, `name`, `displayid`, `Quality`, "
        "`BuyCount`, `BuyPrice`, `SellPrice`, `InventoryType`, `AllowableClass`, `AllowableRace`, `ItemLevel`, `RequiredLevel`, "
        "`maxcount`, `stackable`, `dmg_min1`, `dmg_max1`, `delay`, `bonding`, `description`, `Material`, `sheath`, "
        "`MaxDurability`, `VerifiedBuild`) VALUES",
        "(%d, 2, 14, 7, %s, 35575, 5, 1, 17, 3, 13, -1, -1, 1, 1, 1, 1, 1, 2, 2300, 1, %s, 1, 3, 20, 0);"
        % (SWORD_ITEM, sql_str(SWORD_NAME), sql_str(SWORD_TEXT)),
    ]
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote", path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--patch", required=True, help="the realm's spell patch (Achievement.dbc, ...)")
    ap.add_argument("--client", help="the client folder, for CharTitles.dbc when the patch has none yet")
    ap.add_argument("--out", required=True, help="the new spell patch")
    ap.add_argument("--sql", help="write the module's reward SQL here")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        files = dbcmpq.unpack(args.patch, tmp)
        name, achievements = find_file(files, dbc_name("Achievement"))
        if not achievements:
            sys.exit("%s has no Achievement.dbc" % args.patch)
        order = patch_achievements(achievements)

        name, titles = find_file(files, dbc_name("CharTitles"))
        if not titles:
            if not args.client:
                sys.exit("%s has no CharTitles.dbc yet: pass --client to start from the client's copy" % args.patch)
            titles = os.path.join(tmp, "CharTitles.dbc")
            print("CharTitles.dbc from", dbcmpq.client_file(args.client, "enUS", dbc_name("CharTitles"), titles))
            files[dbc_name("CharTitles")] = titles
        patch_titles(titles)

        name, items = find_file(files, dbc_name("Item"))
        if not items:
            if not args.client:
                sys.exit("%s has no Item.dbc yet: pass --client to start from the client's copy" % args.patch)
            items = os.path.join(tmp, "Item.dbc")
            print("Item.dbc from", dbcmpq.client_file(args.client, "enUS", dbc_name("Item"), items))
            files[dbc_name("Item")] = items
        patch_items(items)

        dbcmpq.pack(args.out, [(local, archived) for archived, local in sorted(files.items())])
        print("%s: %d files, %.1f MB" % (args.out, len(files), os.path.getsize(args.out) / 1e6))
    if args.sql:
        write_sql(args.sql, order)


if __name__ == "__main__":
    main()
