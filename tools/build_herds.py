"""Writes Boaring Adventure's boar herds: data/sql/db-world/mod_challenges_world_boaring.sql.

    EVERMORE_DB_USER=... EVERMORE_DB_PASS=... python3 build_herds.py --client /path/to/WoW \\
        --sql ../data/sql/db-world/mod_challenges_world_boaring.sql

Stock boars leave whole level ranges and start zones without any (none from 18 to 47 outside
dungeons, none in Tirisfal, Teldrassil, Eversong or Azuremyst). Each band below gets a creature of
its own and a few small herds in its zone, in a phase only Boaring Adventure characters see (phase
bit 26, through the aura spell 90700 the module gives them).

The herds stand where the zone's own wildlife does: the spawns of ground beasts and critters of
about the band's level, inside the zone's map rectangle (the client's WorldMapArea.dbc), away from
any NPC that offers a service (towns, camps, flight masters). The script reads the world database
(read-only, EVERMORE_DB_HOST/PORT/USER/PASS) and picks the same spots on every run as long as the
world data doesn't change. Needs pymysql.

Ids: creature entries 9507000-9507099, spawn guids 9507100-9508999.
"""

import argparse
import math
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dbcmpq  # noqa: E402

ENTRY_BASE = 9507000
GUID_BASE, GUID_END = 9507100, 9508999
PHASE_BOARING = 0x04000000
AURA_SPELL = 90700
HERDS, PER_HERD = 8, 4
HERD_RADIUS = 70.0        # yards from a herd's centre a spot may be
MIN_SPACING = 8.0         # yards between two boars of a herd
TOWN_RADIUS = 60.0        # yards to keep from service NPCs
RESPAWN, WANDER = 90, 8.0

# Stock boars the herds take their AI (charges), faction and loot from, by level.
BASES = [(12, 3100), (99, 547)]                                   # Elder Mottled Boar, Great Goretusk
LOOT = [(12, 3100), (22, 547), (35, 4511), (50, 5992), (99, 5993)]  # + Agam'ar, Ashmane Boar, Helboar

# (zone area id, map, min level, max level, name, displays)
BANDS = [
    (12, 0, 1, 4, "Thicket Piglet", [503, 377]),
    (12, 0, 7, 10, "Elwynn Tusker", [704, 1208]),
    (85, 0, 1, 5, "Blighted Piglet", [6121]),
    (85, 0, 5, 10, "Rotting Hog", [6121, 6122]),
    (141, 1, 1, 5, "Moonwood Piglet", [607, 381]),
    (141, 1, 5, 10, "Shadowglen Tusker", [744, 381]),
    (3430, 530, 1, 5, "Sunstrider Piglet", [8869, 8871]),
    (3430, 530, 5, 10, "Goldenmane Boar", [8871, 6807]),
    (3524, 530, 1, 5, "Azure Piglet", [607, 381]),
    (3524, 530, 5, 10, "Ammen Boar", [744, 381]),
    (17, 1, 10, 14, "Barrens Razorback", [3027, 704]),
    (17, 1, 14, 18, "Thornback Boar", [3035, 3027]),
    (130, 0, 10, 14, "Silverpine Mudboar", [704, 1208]),
    (130, 0, 14, 18, "Pyrewood Hog", [6121]),
    (148, 1, 10, 14, "Darkshore Tusker", [744, 381]),
    (148, 1, 14, 18, "Moonlit Boar", [744]),
    (3525, 530, 10, 14, "Bloodmyst Boar", [3027, 6807]),
    (3525, 530, 14, 18, "Crimsonhide Boar", [3035]),
    (3433, 530, 10, 14, "Ghostlands Boar", [8871, 8869]),
    (3433, 530, 14, 18, "Deadscar Hog", [6121]),
    (44, 0, 17, 21, "Redridge Bristleback", [3027, 1208]),
    (331, 1, 18, 22, "Ashenvale Tusker", [744, 381]),
    (11, 0, 20, 24, "Wetlands Mudwallow", [704, 1208]),
    (406, 1, 20, 24, "Stonetalon Ridgeback", [703, 704]),
    (10, 0, 22, 26, "Duskwood Gloomhog", [6121, 6122]),
    (267, 0, 23, 27, "Hillsbrad Tusker", [1208, 704]),
    (400, 1, 25, 29, "Needlerock Boar", [3035, 3027]),
    (33, 0, 28, 32, "Jungle Razorback", [703, 3035]),
    (45, 0, 31, 35, "Highland Boar", [1167, 499]),
    (405, 1, 31, 35, "Desolace Dunehog", [8870]),
    (36, 0, 33, 37, "Alterac Snowtusk", [193, 1167]),
    (33, 0, 34, 38, "Grimtusk Razorback", [3026, 703]),
    (3, 0, 36, 40, "Badlands Ridgeback", [3026]),
    (8, 0, 36, 40, "Sorrowmire Hog", [703, 704]),
    (15, 1, 37, 41, "Mire Wallower", [703, 1208]),
    (357, 1, 41, 45, "Feralas Thornboar", [4713, 4716]),
    (47, 0, 42, 46, "Hinterland Tusker", [4714, 1167]),
    (440, 1, 43, 47, "Dune Boar", [8870, 8871]),
    (51, 0, 45, 49, "Cinderhide Boar", [3026, 2453]),
    (490, 1, 48, 52, "Crater Razorback", [4716, 20814]),
    (361, 1, 49, 53, "Felwood Felboar", [11410, 16571]),
    (4, 0, 50, 54, "Dreadmaul Hellboar", [16571, 18701]),
    (16, 1, 50, 54, "Azshara Tusker", [20829, 4714]),
    (46, 0, 51, 55, "Ember Felboar", [16572, 18701]),
    (28, 0, 51, 55, "Plaguewood Hog", [6122, 6121]),
    (618, 1, 54, 58, "Frostmane Boar", [22320, 1167]),
    (139, 0, 55, 59, "Plaguelands Rotboar", [6122]),
    (1377, 1, 56, 60, "Silithid-Scarred Boar", [4714, 20814]),
    (618, 1, 57, 60, "Everfrost Tusker", [22320, 193]),
    (139, 0, 57, 60, "Darrowshire Hog", [6122, 6121]),
]

TYPE_BEAST, TYPE_CRITTER = 1, 8

SPELL_COLUMNS = 'INSERT INTO `spell_dbc` (`ID`,`Category`,`DispelType`,`Mechanic`,`Attributes`,`AttributesEx`,`AttributesEx2`,`AttributesEx3`,`AttributesEx4`,`AttributesEx5`,`AttributesEx6`,`AttributesEx7`,`ShapeshiftMask`,`unk_320_2`,`ShapeshiftExclude`,`unk_320_3`,`Targets`,`TargetCreatureType`,`RequiresSpellFocus`,`FacingCasterFlags`,`CasterAuraState`,`TargetAuraState`,`ExcludeCasterAuraState`,`ExcludeTargetAuraState`,`CasterAuraSpell`,`TargetAuraSpell`,`ExcludeCasterAuraSpell`,`ExcludeTargetAuraSpell`,`CastingTimeIndex`,`RecoveryTime`,`CategoryRecoveryTime`,`InterruptFlags`,`AuraInterruptFlags`,`ChannelInterruptFlags`,`ProcTypeMask`,`ProcChance`,`ProcCharges`,`MaxLevel`,`BaseLevel`,`SpellLevel`,`DurationIndex`,`PowerType`,`ManaCost`,`ManaCostPerLevel`,`ManaPerSecond`,`ManaPerSecondPerLevel`,`RangeIndex`,`Speed`,`ModalNextSpell`,`CumulativeAura`,`Totem_1`,`Totem_2`,`Reagent_1`,`Reagent_2`,`Reagent_3`,`Reagent_4`,`Reagent_5`,`Reagent_6`,`Reagent_7`,`Reagent_8`,`ReagentCount_1`,`ReagentCount_2`,`ReagentCount_3`,`ReagentCount_4`,`ReagentCount_5`,`ReagentCount_6`,`ReagentCount_7`,`ReagentCount_8`,`EquippedItemClass`,`EquippedItemSubclass`,`EquippedItemInvTypes`,`Effect_1`,`Effect_2`,`Effect_3`,`EffectDieSides_1`,`EffectDieSides_2`,`EffectDieSides_3`,`EffectRealPointsPerLevel_1`,`EffectRealPointsPerLevel_2`,`EffectRealPointsPerLevel_3`,`EffectBasePoints_1`,`EffectBasePoints_2`,`EffectBasePoints_3`,`EffectMechanic_1`,`EffectMechanic_2`,`EffectMechanic_3`,`ImplicitTargetA_1`,`ImplicitTargetA_2`,`ImplicitTargetA_3`,`ImplicitTargetB_1`,`ImplicitTargetB_2`,`ImplicitTargetB_3`,`EffectRadiusIndex_1`,`EffectRadiusIndex_2`,`EffectRadiusIndex_3`,`EffectAura_1`,`EffectAura_2`,`EffectAura_3`,`EffectAuraPeriod_1`,`EffectAuraPeriod_2`,`EffectAuraPeriod_3`,`EffectMultipleValue_1`,`EffectMultipleValue_2`,`EffectMultipleValue_3`,`EffectChainTargets_1`,`EffectChainTargets_2`,`EffectChainTargets_3`,`EffectItemType_1`,`EffectItemType_2`,`EffectItemType_3`,`EffectMiscValue_1`,`EffectMiscValue_2`,`EffectMiscValue_3`,`EffectMiscValueB_1`,`EffectMiscValueB_2`,`EffectMiscValueB_3`,`EffectTriggerSpell_1`,`EffectTriggerSpell_2`,`EffectTriggerSpell_3`,`EffectPointsPerCombo_1`,`EffectPointsPerCombo_2`,`EffectPointsPerCombo_3`,`EffectSpellClassMaskA_1`,`EffectSpellClassMaskA_2`,`EffectSpellClassMaskA_3`,`EffectSpellClassMaskB_1`,`EffectSpellClassMaskB_2`,`EffectSpellClassMaskB_3`,`EffectSpellClassMaskC_1`,`EffectSpellClassMaskC_2`,`EffectSpellClassMaskC_3`,`SpellVisualID_1`,`SpellVisualID_2`,`SpellIconID`,`ActiveIconID`,`SpellPriority`,`Name_Lang_enUS`,`Name_Lang_enGB`,`Name_Lang_koKR`,`Name_Lang_frFR`,`Name_Lang_deDE`,`Name_Lang_enCN`,`Name_Lang_zhCN`,`Name_Lang_enTW`,`Name_Lang_zhTW`,`Name_Lang_esES`,`Name_Lang_esMX`,`Name_Lang_ruRU`,`Name_Lang_ptPT`,`Name_Lang_ptBR`,`Name_Lang_itIT`,`Name_Lang_Unk`,`Name_Lang_Mask`,`NameSubtext_Lang_enUS`,`NameSubtext_Lang_enGB`,`NameSubtext_Lang_koKR`,`NameSubtext_Lang_frFR`,`NameSubtext_Lang_deDE`,`NameSubtext_Lang_enCN`,`NameSubtext_Lang_zhCN`,`NameSubtext_Lang_enTW`,`NameSubtext_Lang_zhTW`,`NameSubtext_Lang_esES`,`NameSubtext_Lang_esMX`,`NameSubtext_Lang_ruRU`,`NameSubtext_Lang_ptPT`,`NameSubtext_Lang_ptBR`,`NameSubtext_Lang_itIT`,`NameSubtext_Lang_Unk`,`NameSubtext_Lang_Mask`,`Description_Lang_enUS`,`Description_Lang_enGB`,`Description_Lang_koKR`,`Description_Lang_frFR`,`Description_Lang_deDE`,`Description_Lang_enCN`,`Description_Lang_zhCN`,`Description_Lang_enTW`,`Description_Lang_zhTW`,`Description_Lang_esES`,`Description_Lang_esMX`,`Description_Lang_ruRU`,`Description_Lang_ptPT`,`Description_Lang_ptBR`,`Description_Lang_itIT`,`Description_Lang_Unk`,`Description_Lang_Mask`,`AuraDescription_Lang_enUS`,`AuraDescription_Lang_enGB`,`AuraDescription_Lang_koKR`,`AuraDescription_Lang_frFR`,`AuraDescription_Lang_deDE`,`AuraDescription_Lang_enCN`,`AuraDescription_Lang_zhCN`,`AuraDescription_Lang_enTW`,`AuraDescription_Lang_zhTW`,`AuraDescription_Lang_esES`,`AuraDescription_Lang_esMX`,`AuraDescription_Lang_ruRU`,`AuraDescription_Lang_ptPT`,`AuraDescription_Lang_ptBR`,`AuraDescription_Lang_itIT`,`AuraDescription_Lang_Unk`,`AuraDescription_Lang_Mask`,`ManaCostPct`,`StartRecoveryCategory`,`StartRecoveryTime`,`MaxTargetLevel`,`SpellClassSet`,`SpellClassMask_1`,`SpellClassMask_2`,`SpellClassMask_3`,`MaxTargets`,`DefenseType`,`PreventionType`,`StanceBarOrder`,`EffectChainAmplitude_1`,`EffectChainAmplitude_2`,`EffectChainAmplitude_3`,`MinFactionID`,`MinReputation`,`RequiredAuraVision`,`RequiredTotemCategoryID_1`,`RequiredTotemCategoryID_2`,`RequiredAreasID`,`SchoolMask`,`RuneCostID`,`SpellMissileID`,`PowerDisplayID`,`EffectBonusMultiplier_1`,`EffectBonusMultiplier_2`,`EffectBonusMultiplier_3`,`SpellDescriptionVariableID`,`SpellDifficultyID`) VALUES'
SPELL_ROW = "(%d,0,0,0,2843738496,3072,16385,1048576,128,393224,4096,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,101,0,0,0,0,21,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,-1,0,0,6,0,0,1,0,0,0,0,0,-1,0,0,0,0,0,1,0,0,0,0,0,0,0,0,261,0,0,0,0,0,0,0,0,0,0,0,0,0,0,%d,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,253,0,0,'Phase Shift: Boaring Adventure','','','','','','','','','','','','','','','',16712190,'','','','','','','','','','','','','','','','',16712172,'','','','','','','','','','','','','','','','',16712188,'','','','','','','','','','','','','','','','',16712188,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0)"


def pick(table, level):
    for top, entry in table:
        if level <= top:
            return entry
    return table[-1][1]


def zone_rects(client):
    """area id -> (map, min x, max x, min y, max y) from WorldMapArea.dbc."""
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "WorldMapArea.dbc")
        dbcmpq.client_file(client, "enUS", "DBFilesClient\\WorldMapArea.dbc", path)
        rows, _strings, _fields = dbcmpq.read_dbc(path)

    def f(v):
        return struct.unpack("<f", struct.pack("<I", v))[0]

    rects = {}
    for r in rows:
        if r[2]:
            left, right, top, bottom = (f(v) for v in r[4:8])
            rects[r[2]] = (r[1], bottom, top, right, left)
    return rects


def query(cur, sql, *args):
    cur.execute(sql, args)
    return cur.fetchall()


def farthest(points, count):
    """Up to count points spread out over the set, starting from the one nearest its middle."""
    if not points:
        return []
    cx = sum(p[0] for p in points) / len(points)
    cy = sum(p[1] for p in points) / len(points)
    chosen = [min(points, key=lambda p: (p[0] - cx) ** 2 + (p[1] - cy) ** 2)]
    while len(chosen) < count:
        best = max(points, key=lambda p: min(math.dist(p[:2], c[:2]) for c in chosen))
        if min(math.dist(best[:2], c[:2]) for c in chosen) < HERD_RADIUS:
            break
        chosen.append(best)
    return chosen


def herd_spots(candidates):
    spots = []
    for centre in farthest(candidates, HERDS):
        near = sorted((p for p in candidates if math.dist(p[:2], centre[:2]) <= HERD_RADIUS),
                      key=lambda p: math.dist(p[:2], centre[:2]))
        herd = []
        for p in near:
            if all(math.dist(p[:2], q[:2]) >= MIN_SPACING for q in herd + spots):
                herd.append(p)
            if len(herd) == PER_HERD:
                break
        spots += herd
    return spots


def sql_str(s):
    return "'" + s.replace("\\", "\\\\").replace("'", "''") + "'"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--client", required=True, help="the client folder, for WorldMapArea.dbc")
    ap.add_argument("--sql", required=True, help="where to write the herd SQL")
    args = ap.parse_args()

    import pymysql
    db = pymysql.connect(host=os.environ.get("EVERMORE_DB_HOST", "127.0.0.1"), port=int(os.environ.get("EVERMORE_DB_PORT", "3306")),
                         user=os.environ["EVERMORE_DB_USER"], password=os.environ["EVERMORE_DB_PASS"], database="acore_world")
    cur = db.cursor()
    rects = zone_rects(args.client)

    spawns = query(cur, """
        SELECT c.guid, c.map, c.position_x, c.position_y, c.position_z, ct.minlevel, ct.maxlevel, ct.type,
               ct.skinloot, ct.mingold, ct.maxgold
        FROM creature c JOIN creature_template ct ON ct.entry = c.id
        LEFT JOIN creature_template_movement m ON m.CreatureId = c.id
        WHERE c.map IN (0, 1, 530) AND (c.phaseMask & 1) <> 0 AND c.MovementType = 1 AND c.wander_distance > 0
          AND ct.type IN (%s, %s) AND ct.`rank` = 0 AND ct.npcflag = 0 AND ct.entry < 9500000
          AND COALESCE(m.Flight, 0) = 0 AND COALESCE(m.Swim, 0) = 0 AND COALESCE(m.Ground, 1) <> 0""",
                   TYPE_BEAST, TYPE_CRITTER)
    towns = query(cur, """
        SELECT c.map, c.position_x, c.position_y FROM creature c JOIN creature_template ct ON ct.entry = c.id
        WHERE c.map IN (0, 1, 530) AND ct.npcflag <> 0""")
    town_cells = {}
    for m, x, y in towns:
        town_cells.setdefault((m, int(x // TOWN_RADIUS), int(y // TOWN_RADIUS)), []).append((x, y))

    def in_town(m, x, y):
        cx, cy = int(x // TOWN_RADIUS), int(y // TOWN_RADIUS)
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for tx, ty in town_cells.get((m, cx + dx, cy + dy), ()):
                    if math.dist((x, y), (tx, ty)) < TOWN_RADIUS:
                        return True
        return False

    templates, models, spawn_rows, report = [], [], [], []
    guid = GUID_BASE
    for index, (zone, mapid, lo, hi, name, displays) in enumerate(BANDS):
        entry = ENTRY_BASE + index
        zmap, x0, x1, y0, y1 = rects[zone]
        if zmap != mapid:
            sys.exit("%s: zone %d is on map %d, not %d" % (name, zone, zmap, mapid))
        inside = [s for s in spawns if s[1] == mapid and x0 <= s[2] <= x1 and y0 <= s[3] <= y1
                  and not in_town(mapid, s[2], s[3])]
        beasts = [s for s in inside if s[7] == TYPE_BEAST and s[6] >= lo - 3 and s[5] <= hi + 3]
        pool = beasts if len(beasts) >= HERDS * PER_HERD else beasts + [s for s in inside if s[7] == TYPE_CRITTER]
        points = sorted({(s[2], s[3], s[4], s[0]) for s in pool}, key=lambda p: p[3])
        spots = herd_spots(points)
        if len(spots) < PER_HERD * 2:
            sys.exit("%s: only %d spots in zone %d" % (name, len(spots), zone))

        def median(values, default):
            values = sorted(v for v in values if v)
            return values[len(values) // 2] if values else default

        skinloot = median([s[8] for s in beasts], 0)
        mingold = median([s[9] for s in beasts], 0)
        maxgold = median([s[10] for s in beasts], 0)
        templates.append((entry, pick(BASES, hi), name, lo, hi, pick(LOOT, hi), skinloot, mingold, maxgold))
        for i, display in enumerate(displays):
            models.append((entry, i, display))
        for x, y, z, _src in spots:
            orientation = round(((x * 7.0 + y * 13.0) % 628) / 100.0, 4)
            spawn_rows.append((guid, entry, mapid, zone, x, y, z, orientation, name))
            guid += 1
        report.append("%-24s %2d-%2d zone %4d: %2d boars (%d beast spots)" % (name, lo, hi, zone, len(spots), len(beasts)))

    if guid - 1 > GUID_END:
        sys.exit("too many spawns: %d" % (guid - GUID_BASE))
    write_sql(args.sql, templates, models, spawn_rows)
    print("\n".join(report))
    print("%d bands, %d boars, guids %d-%d" % (len(BANDS), len(spawn_rows), GUID_BASE, guid - 1))


def write_sql(path, templates, models, spawns):
    last = ENTRY_BASE + len(templates) - 1
    lines = [
        "-- mod-challenges: Boaring Adventure's boar herds (world database).",
        "-- Written by tools/build_herds.py; edit the bands there and run it again rather than editing this file.",
        "-- One creature per level band (%d-%d), copied from a stock boar of about that level (its AI," % (ENTRY_BASE, last),
        "-- faction and loot), and small herds in the band's zone (spawn guids %d-%d). They stand in phase" % (GUID_BASE, GUID_END),
        "-- %d (bit 26), which only Boaring Adventure characters are in, through the server-side aura" % PHASE_BOARING,
        "-- spell %d. Nobody else sees them. Idempotent. Undo with data/sql/uninstall/mod_challenges_uninstall_world.sql." % AURA_SPELL,
        "",
        "DELETE FROM `spell_dbc` WHERE `ID` = %d;" % AURA_SPELL,
        # The same row as mod-era-events' phase auras: passive, hidden, kept through death, self only.
        SPELL_COLUMNS,
        SPELL_ROW % (AURA_SPELL, PHASE_BOARING | 1) + ";",
        "",
        "DELETE FROM `creature` WHERE `guid` BETWEEN %d AND %d;" % (GUID_BASE, GUID_END),
        "DELETE FROM `smart_scripts` WHERE `entryorguid` BETWEEN %d AND %d AND `source_type` = 0;" % (ENTRY_BASE, ENTRY_BASE + 99),
        "DELETE FROM `creature_template_model` WHERE `CreatureID` BETWEEN %d AND %d;" % (ENTRY_BASE, ENTRY_BASE + 99),
        "DELETE FROM `creature_template_movement` WHERE `CreatureId` BETWEEN %d AND %d;" % (ENTRY_BASE, ENTRY_BASE + 99),
        "DELETE FROM `creature_template` WHERE `entry` BETWEEN %d AND %d;" % (ENTRY_BASE, ENTRY_BASE + 99),
        "",
        "-- Copies of the stock boars, without naming their columns.",
        "DROP TEMPORARY TABLE IF EXISTS `mod_challenges_boar`;",
        "CREATE TEMPORARY TABLE `mod_challenges_boar` LIKE `creature_template`;",
    ]
    for entry, base, name, lo, hi, loot, skinloot, mingold, maxgold in templates:
        lines += [
            "INSERT INTO `mod_challenges_boar` SELECT * FROM `creature_template` WHERE `entry` = %d;" % base,
            "UPDATE `mod_challenges_boar` SET `entry` = %d, `name` = %s, `subname` = NULL, `minlevel` = %d, `maxlevel` = %d, "
            "`lootid` = %d, `skinloot` = %d, `mingold` = %d, `maxgold` = %d, `VerifiedBuild` = 0 WHERE `entry` = %d;"
            % (entry, sql_str(name), lo, hi, loot, skinloot, mingold, maxgold, base),
        ]
    lines += [
        "INSERT INTO `creature_template` SELECT * FROM `mod_challenges_boar`;",
        "DROP TEMPORARY TABLE `mod_challenges_boar`;",
        "",
        "DROP TEMPORARY TABLE IF EXISTS `mod_challenges_boar_movement`;",
        "CREATE TEMPORARY TABLE `mod_challenges_boar_movement` LIKE `creature_template_movement`;",
    ]
    for entry, base, *_rest in templates:
        lines.append("INSERT INTO `mod_challenges_boar_movement` SELECT %d, `Ground`, `Swim`, `Flight`, `Rooted`, `Chase`, `Random`, "
                     "`InteractionPauseTimer` FROM `creature_template_movement` WHERE `CreatureId` = %d;" % (entry, base))
    lines += [
        "INSERT INTO `creature_template_movement` SELECT * FROM `mod_challenges_boar_movement`;",
        "DROP TEMPORARY TABLE `mod_challenges_boar_movement`;",
        "",
        "-- The stock boars' charges.",
        "DROP TEMPORARY TABLE IF EXISTS `mod_challenges_boar_ai`;",
        "CREATE TEMPORARY TABLE `mod_challenges_boar_ai` LIKE `smart_scripts`;",
    ]
    for base in sorted({t[1] for t in templates}):
        lines.append("INSERT INTO `mod_challenges_boar_ai` SELECT * FROM `smart_scripts` WHERE `entryorguid` = %d AND `source_type` = 0;" % base)
    for entry, base, *_rest in templates:
        lines.append("INSERT INTO `smart_scripts` SELECT %d, `source_type`, `id`, `link`, `event_type`, `event_phase_mask`, "
                     "`event_chance`, `event_flags`, `event_param1`, `event_param2`, `event_param3`, `event_param4`, `event_param5`, "
                     "`event_param6`, `action_type`, `action_param1`, `action_param2`, `action_param3`, `action_param4`, "
                     "`action_param5`, `action_param6`, `target_type`, `target_param1`, `target_param2`, `target_param3`, "
                     "`target_param4`, `target_x`, `target_y`, `target_z`, `target_o`, `comment` FROM `mod_challenges_boar_ai` "
                     "WHERE `entryorguid` = %d;" % (entry, base))
    lines += [
        "DROP TEMPORARY TABLE `mod_challenges_boar_ai`;",
        "",
        "INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`) VALUES",
        ",\n".join("(%d, %d, %d, 1, 1, 0)" % m for m in models) + ";",
        "",
        "INSERT INTO `creature` (`guid`, `id`, `map`, `zoneId`, `areaId`, `spawnMask`, `phaseMask`, `equipment_id`, `position_x`, "
        "`position_y`, `position_z`, `orientation`, `spawntimesecs`, `wander_distance`, `currentwaypoint`, `curhealth`, `curmana`, "
        "`MovementType`, `npcflag`, `unit_flags`, `dynamicflags`, `ScriptName`, `VerifiedBuild`, `CreateObject`, `Comment`) VALUES",
        ",\n".join("(%d, %d, %d, %d, 0, 1, %d, 0, %.3f, %.3f, %.3f, %.4f, %d, %g, 0, 1, 0, 1, 0, 0, 0, '', 0, 0, %s)"
                   % (g, e, m, z, PHASE_BOARING, x, y, zz, o, RESPAWN, WANDER, sql_str("mod-challenges: Boaring Adventure, " + n))
                   for g, e, m, z, x, y, zz, o, n in spawns) + ";",
    ]
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote", path)


if __name__ == "__main__":
    main()
