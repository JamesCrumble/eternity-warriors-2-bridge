#!/usr/bin/env python3
"""Dumps game tables from CAB-base_uniform_global_manager to server/generated/game_data.json.

Extracts:
  - items: item metadata & fixed attribute ranges for all 10 ItemDataList tables
  - attr_values: EW_HIT_PARAMS -> GS multiplier (_value) from AttriuteConfig
  - enemies: AcData stats (15 attributes + _lootXp + _lootVp) from AcDataList
  - loot1, loot2, loot_b1, loot_b2, loot_b3: drop tables from LootDatabase*
  - gacha_items, gacha_groups: Shrine (Gacha) configs and pools
"""
import json
import lzma
import os
import struct
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OBB = os.path.join(ROOT, "ew3data/Android/obb/com.glu.ewarriors3/main.4100.com.glu.ewarriors3.obb")
BUNDLE_PREFIX = "assets/AssetBundles/base_uniform_global_manager_"

EW_HIT_PARAMS = [
    "HITPOINT", "ANIMATION_SPEED", "CRITICAL_STRIKE_CHANCE", "CRITICAL_STRIKE_DAMAGE",
    "RUNSPEED", "PHYSICAL_RESIST", "PHYSICAL_DAMAGE", "FIRE_RESIST", "FIRE_DAMAGE",
    "POISON_RESIST", "POISON_DAMAGE", "THUNDER_RESIST", "THUNDER_DAMAGE", "ICE_RESIST",
    "ICE_DAMAGE", "PARALYSIS_RESIST", "FREEZE_RESIST", "DAMAGE_SCALE", "MANAPOINT",
    "CURE_POINT", "FREEZE_TIME", "FREEZE_PERCENT", "PARALYSIS_TIME", "PARALYSIS_PERCENT",
    "STUN_TIME", "STUN_PERCENT", "POISON_TIME", "POISON_TOTAL", "FIRE_TIME", "FIRE_TOTAL",
    "BLOOD_TIME", "BLOOD_TOTAL", "THRESHOLD_MAX", "EXP_ADD", "RAGE_RESTORE", "MANA_RESTORE",
    "HP_RESTORE", "RUN_SPEED_PERCENT", "SC_ADD", "ITEM_FIND", "REDUCE_ENERGY", "NONE",
    "UNVALID", "MAX",
]


class Reader:
    def __init__(self, data):
        self.d, self.p = data, 0

    def i32(self):
        v = struct.unpack_from("<i", self.d, self.p)[0]
        self.p += 4
        return v

    def f32(self):
        v = struct.unpack_from("<f", self.d, self.p)[0]
        self.p += 4
        return round(v, 6)

    def boolean(self):
        v = self.d[self.p] != 0
        self.p += 4
        return v

    def string(self):
        n = self.i32()
        s = self.d[self.p : self.p + n].decode("utf-8", "replace")
        self.p = (self.p + n + 3) & ~3
        return s

    def array(self, read_one):
        return [read_one() for _ in range(self.i32())]


def read_item(r):
    item_id = r.string()
    itype = r.i32()
    enable_lv = r.i32()
    iclass = r.i32()
    part = r.i32()
    level = r.i32()
    r.string()  # _nameIds
    r.string()  # _descriptionIds
    r.i32()     # _maskColor
    rare = r.i32()
    loot_score = r.i32()
    for _ in range(7):
        r.string()
    r.boolean()  # fixedAttr
    r.boolean()  # _isRandomAttr
    r.boolean()  # _isLoot
    attrs = []
    for _ in range(3):
        aid = r.string()
        amin = r.f32()
        amax = r.f32()
        if amin > 0 and aid and aid != "0":
            attrs.append([aid, amin, amax])
    r.i32()     # _equipmentScore
    r.i32()     # _mainType
    r.f32()     # _mainPercentMin
    r.f32()     # _mainPercentMax
    sell_factor = r.f32()
    default_xp = r.f32()
    for _ in range(3):
        r.f32()
    r.string()  # _kontagentId
    r.f32()     # _pvpPower
    r.string()  # _levelName
    r.i32()     # _levelDifficulty
    return item_id, {
        "t": itype,
        "c": iclass,
        "p": part,
        "lv": level,
        "r": rare,
        "ls": loot_score,
        "sf": sell_factor,
        "dxp": default_xp,
        "a": attrs,
    }


def read_ac(r):
    ac_id = r.string()
    r.string()  # _characterId
    r.string()  # _class
    r.string()  # _nameIds
    elite_type = r.i32()
    phys_res = r.f32()
    r.f32()     # _raduis
    r.i32()     # _faction
    is_player = r.boolean()
    speed = r.f32()
    r.f32()     # _angleSpeed
    hp = r.i32()
    r.i32()     # _energy
    r.i32()     # _vitality
    level = r.i32()
    threshold_max = r.i32()
    phys_dmg = r.f32()
    fire_dmg = r.f32()
    fire_res = r.f32()
    poison_dmg = r.f32()
    poison_res = r.f32()
    thunder_dmg = r.f32()
    thunder_res = r.f32()
    ice_dmg = r.f32()
    ice_res = r.f32()
    crit_rate = r.f32()
    crit_dmg = r.f32()
    r.array(lambda: (r.string(), r.i32()))  # _equipList
    loot_xp = r.i32()
    loot_vp = r.i32()
    r.array(lambda: (r.string(), r.i32(), r.i32(), r.i32()))  # _lootItems
    for _ in range(8):
        r.i32()
    r.string()  # _pathName
    for _ in range(10):
        r.i32()
    # Order expected by EndlessLevel.ReadEnemyDataFromWave (_attributes[0..14]):
    # 0: _speed, 1: _hitPoint, 2: _physicalDamage, 3: _physicalResist,
    # 4: _iceDamage, 5: _iceResist, 6: _fireDamage, 7: _fireResist,
    # 8: _thunderDamage, 9: _thunderResist, 10: _poisonDamage, 11: _poisonResist,
    # 12: _critRate, 13: _critDamage, 14: _thresholdMax
    attrs15 = [
        speed, hp, phys_dmg, phys_res,
        ice_dmg, ice_res, fire_dmg, fire_res,
        thunder_dmg, thunder_res, poison_dmg, poison_res,
        crit_rate, crit_dmg, threshold_max,
    ]
    return ac_id, {"xp": loot_xp, "vp": loot_vp, "et": elite_type, "lv": level, "p": is_player, "a": attrs15}


def read_loot_db(r):
    out = {}
    for _ in range(r.i32()):
        eid = r.string()
        lst = r.array(lambda: [r.i32(), r.string(), r.i32(), r.i32()])
        out[eid] = [w for w in lst if w[0] > 0 and w[1] != "0"]
    return out


def main():
    z = zipfile.ZipFile(sys.argv[1] if len(sys.argv) > 1 else OBB)
    bname = [n for n in z.namelist() if n.startswith(BUNDLE_PREFIX)][0]
    b = z.read(bname)
    hdr_sz = struct.unpack_from(">I", b, 31)[0]
    raw = lzma.LZMADecompressor(lzma.FORMAT_ALONE).decompress(b[hdr_sz:])
    cab_off = struct.unpack_from(">I", raw, 36)[0]
    cab = raw[cab_off:]
    meta_sz, file_sz, ver, data_off = struct.unpack_from(">IIII", cab, 0)

    # Locate object table in Unity 4.6 SerializedFile (offset 42)
    p = 42
    nobjs = struct.unpack_from("<i", cab, p)[0]
    p += 4

    by_name = {}
    for _ in range(nobjs):
        pid, off, sz, tid, cid, unk = struct.unpack_from("<iiiihh", cab, p)
        p += 20
        if cid == 114:
            o = data_off + off
            nlen = struct.unpack_from("<I", cab, o + 20)[0]
            name = cab[o + 24 : o + 24 + nlen].decode("utf-8", "replace")
            body = cab[(o + 24 + nlen + 3) & ~3 : o + sz]
            by_name[name] = body

    # 1. Attribute GS multipliers
    r = Reader(by_name["AttriuteConfig"])
    attr_values = {}
    for _ in range(r.i32()):
        t = r.i32()
        score = r.f32()
        r.string()
        val = r.f32()
        if 0 <= t < len(EW_HIT_PARAMS):
            attr_values[EW_HIT_PARAMS[t]] = val

    # 2. All ItemData tables
    items = {}
    for tname in (
        "WeaponItemDataList", "ArmorItemDataList", "OrnamentItemDataList",
        "VanityItemDataList", "PetItemDataList", "PotionItemDataList",
        "EvolutionItemDataList", "SocketItemDataList", "GemItemDataList", "Token",
    ):
        r = Reader(by_name[tname])
        for k, v in r.array(lambda: read_item(r)):
            items[k] = v

    # 3. AcDataList
    r = Reader(by_name["AcDataList"])
    enemies = {}
    for k, v in r.array(lambda: read_ac(r)):
        if not v["p"]:
            del v["p"]
            enemies[k] = v

    # 4. Loot databases
    loot1 = read_loot_db(Reader(by_name["LootDatabase1"]))
    loot2 = read_loot_db(Reader(by_name["LootDatabase2"]))
    loot_b1 = read_loot_db(Reader(by_name["LootDatabaseBonus1"]))
    loot_b2 = read_loot_db(Reader(by_name["LootDatabaseBonus2"]))
    loot_b3 = read_loot_db(Reader(by_name["LootDatabaseBonus3"]))

    # 5. GachaItemList & GachaGroupList
    r = Reader(by_name["GachaItemList"])
    gacha_items = {}
    for _ in range(r.i32()):
        gid = r.string()
        for _ in range(3):
            r.string()
        r.boolean()
        for _ in range(3):
            r.i32()
        c1_sc = r.i32()
        c1_hc = r.i32()
        c1_id = r.string()
        c1_cnt = r.i32()
        r.i32()
        c10_sc = r.i32()
        c10_hc = r.i32()
        c10_id = r.string()
        c10_cnt = r.i32()
        r.i32()
        chase = r.string()
        hit = r.string()
        hit2 = r.string()
        r.boolean()
        r.i32()
        r.i32()
        r.string()
        r.string()
        r.boolean()
        r.boolean()
        r.string()
        r.string()
        gacha_items[gid] = {
            "c1_sc": c1_sc, "c1_hc": c1_hc, "c1_id": c1_id, "c1_cnt": c1_cnt,
            "c10_sc": c10_sc, "c10_hc": c10_hc, "c10_id": c10_id, "c10_cnt": c10_cnt,
            "chase": chase, "hit": hit, "hit2": hit2,
        }

    r = Reader(by_name["GachaGroupList"])
    gacha_groups = {}
    for _ in range(r.i32()):
        grp_id = r.string()
        lst = r.array(lambda: [r.string(), r.i32(), r.i32()])
        gacha_groups[grp_id] = [x for x in lst if x[0] != "0" and x[1] > 0]

    # 6. FusionList, FusionConfig, and EvolutionList
    r = Reader(by_name["FusionList"])
    fusion_list = {}
    fusion_max = {}
    for _ in range(r.i32()):
        lv, rare, evo, fus = r.i32(), r.i32(), r.i32(), r.i32()
        nsc, nhc, nxp, oxp, eff, cd = r.f32(), r.f32(), r.i32(), r.i32(), r.f32(), r.f32()
        fusion_list[f"{lv}_{rare}_{evo}_{fus}"] = [nsc, nhc, nxp, oxp, eff, cd]
        if fus != 0:
            ekey = f"{lv}_{rare}_{evo}"
            fusion_max[ekey] = fusion_max.get(ekey, 0) + 1

    r = Reader(by_name["FusionConfig"])
    fusion_config = {}
    for _ in range(r.i32()):
        part, factor = r.i32(), r.f32()
        fusion_config[str(part)] = factor

    r = Reader(by_name["EvolutionList"])
    evolution_list = {}
    for _ in range(r.i32()):
        lv, rare, evo = r.i32(), r.i32(), r.i32()
        eff, cd, csc, chc = r.f32(), r.f32(), r.i32(), r.i32()
        needs = r.array(lambda: [r.string(), r.i32()])
        dhc, did, dcnt, dsc = r.i32(), r.string(), r.i32(), r.i32()
        ssc, rhc, rsc, srate = r.i32(), r.i32(), r.i32(), r.f32()
        evolution_list[f"{lv}_{rare}_{evo}"] = [eff, cd, csc, chc, needs]

    out_path = os.path.join(ROOT, "server", "generated", "game_data.json")
    data = {
        "attr_values": attr_values,
        "items": items,
        "enemies": enemies,
        "loot1": loot1,
        "loot2": loot2,
        "loot_b1": loot_b1,
        "loot_b2": loot_b2,
        "loot_b3": loot_b3,
        "gacha_items": gacha_items,
        "gacha_groups": gacha_groups,
        "fusion_list": fusion_list,
        "fusion_max": fusion_max,
        "fusion_config": fusion_config,
        "evolution_list": evolution_list,
    }
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(data, f, separators=(",", ":"))
    print(
        f"Wrote {out_path} ({os.path.getsize(out_path)} bytes): "
        f"{len(items)} items, {len(enemies)} enemies, {len(fusion_list)} fusions, {len(gacha_items)} gachas"
    )


if __name__ == "__main__":
    main()
