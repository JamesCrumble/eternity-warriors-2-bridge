#!/usr/bin/env python3
"""Dumps the game's quest table (QuestDataList "QuestDB") to server/generated/quests.json.

The bundles were built without type trees, so the MonoBehaviour is read by hand
with the field layout of the decompiled C# (QuestData, QuestTarget,
QuestRewardItem) and Unity 4's serialisation rules: int32/enum = 4 bytes,
bool = 1 byte then align 4, string = int32 length + bytes + align 4,
List<T> = int32 count + elements, [Serializable] classes inline.

  tools/.venv/bin/python -I tools/dump_quests.py [obb]
"""
import json
import os
import struct
import sys
import zipfile

import UnityPy

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OBB = os.path.join(ROOT, "ew3data/Android/obb/com.glu.ewarriors3/main.4100.com.glu.ewarriors3.obb")
BUNDLE = "assets/AssetBundles/base_uniform_global_manager_"


class Reader:
    def __init__(self, data):
        self.d, self.p = data, 0

    def align(self):
        self.p = (self.p + 3) & ~3

    def i32(self):
        v = struct.unpack_from("<i", self.d, self.p)[0]
        self.p += 4
        return v

    def boolean(self):
        v = self.d[self.p] != 0
        self.p += 1
        self.align()
        return v

    def string(self):
        n = self.i32()
        s = self.d[self.p:self.p + n].decode("utf-8", "replace")
        self.p += n
        self.align()
        return s

    def array(self, read_one):
        return [read_one() for _ in range(self.i32())]


def quest_target(r):
    return {"target_type": r.i32(), "target_id": r.string(), "target_var1": r.string(), "target_count": r.i32()}


def quest_reward_item(r):
    return {"reward_item_id": r.string(), "reward_role": r.i32(), "reward_item_count": r.i32()}


def quest_data(r):
    q = {}
    q["quest_id"] = r.i32()
    for k in ("quest_name", "giver", "description"):
        q[k] = r.string()
    for k in ("status", "quest_type", "cycle_type", "start_offset", "end_offset"):
        q[k] = r.i32()
    q["guild_only"] = r.boolean()
    for k in ("visible_player_level_low", "visible_player_level_high", "visible_difficulty_low",
              "visible_difficulty_high", "prequest_id", "disable_prequest_id"):
        q[k] = r.i32()
    q["recommended_level_name"] = r.string()
    q["recommended_level_difficulty"] = r.i32()
    q["target_list"] = r.array(lambda: quest_target(r))
    for k in ("reward_exp", "reward_sc", "reward_hc"):
        q[k] = r.i32()
    q["reward_item_list"] = r.array(lambda: quest_reward_item(r))
    return q


def main():
    z = zipfile.ZipFile(sys.argv[1] if len(sys.argv) > 1 else OBB)
    name = [n for n in z.namelist() if n.startswith(BUNDLE)][0]
    env = UnityPy.load(z.read(name))
    raw = None
    for o in env.objects:
        if o.type.name == "MonoBehaviour" and o.read().name == "QuestDB":
            raw = bytes(o.get_raw_data())
    r = Reader(raw)
    r.p = 8              # m_GameObject (PPtr: file id, path id)
    r.boolean()          # m_Enabled
    r.p += 8             # m_Script
    r.string()           # m_Name
    quests = r.array(lambda: quest_data(r))
    if r.p != len(raw):
        sys.exit("layout mismatch: %d of %d bytes read" % (r.p, len(raw)))
    out = os.path.join(ROOT, "server/generated/quests.json")
    with open(out, "w") as f:
        json.dump(quests, f, indent=1)
    print("%d quests -> %s" % (len(quests), out))


if __name__ == "__main__":
    main()
