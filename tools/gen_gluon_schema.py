#!/usr/bin/env python3
"""Builds server/generated/gluon_schema.json from the decompiled client (extracted/decompiled).

The Gluon wire format is reflection over C# auto-properties: a BSON key per
`public T name { get; set; }`, the value typed by T. The stand-in server has to
send exactly those types (an int64 into an int property throws in the client),
so it encodes every response against this schema.

Output: {"codes": {"aprq": "AuthenticatePlayerRequest", ...},
         "classes": {"AuthenticatePlayerResponse": {"base": "AbstractGluonResponseMessage",
                                                    "props": {"loginId": "string", ...}},
                     ...},
         "enums": {"GluonResponseStatus": ["SUCCESS", ...], ...}}
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "extracted", "decompiled", "Assembly-CSharp")
OUT = os.path.join(ROOT, "server", "generated", "gluon_schema.json")

CLASS_RE = re.compile(r"^(?:public |internal )?(?:abstract |sealed )?class (\w+)(?:<[^>]*>)?(?:\s*:\s*([\w.<>, ]+))?", re.M)
PROP_RE = re.compile(r"^\s*public (?:virtual |override )?([\w.<>\[\], ?]+?) (\w+)\s*\{\s*get;\s*set;\s*\}", re.M)
ENUM_RE = re.compile(r"^public enum (\w+)\s*\{([^}]*)\}", re.M | re.S)
CODE_RE = re.compile(r'AddType\(new EdgeMessageType\("(\w+)"\), typeof\((\w+)\)\)')


def main():
    if not os.path.isdir(SRC):
        sys.exit("no decompiled sources at %s (see PROJECT_STATE.md)" % SRC)
    classes, enums, codes = {}, {}, {}
    for d, _, files in os.walk(SRC):
        for f in files:
            if not f.endswith(".cs"):
                continue
            text = open(os.path.join(d, f), encoding="utf-8", errors="replace").read()
            for m in ENUM_RE.finditer(text):
                names = [re.split(r"\s*=", v.strip())[0] for v in m.group(2).split(",") if v.strip()]
                enums[m.group(1)] = names
            m = CLASS_RE.search(text)
            if m and m.group(1) not in classes:
                base = (m.group(2) or "").split(",")[0].strip()
                base = re.sub(r"<.*", "", base) or None
                props = {}
                for p in PROP_RE.finditer(text):
                    props[p.group(2)] = p.group(1).strip()
                classes[m.group(1)] = {"base": base, "props": props}
            for c in CODE_RE.finditer(text):
                codes[c.group(1)] = c.group(2)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    json.dump({"codes": codes, "classes": classes, "enums": enums}, open(OUT, "w"), indent=1, sort_keys=True)
    print("%d message codes, %d classes, %d enums -> %s" % (len(codes), len(classes), len(enums), OUT))


if __name__ == "__main__":
    main()
