#!/usr/bin/env python3
"""Compare the export schema table (src/libxrpl/protocol/ExportSchema.cpp)
with the other network's `server_definitions`, and say what has drifted.

The table is hand-maintained and every change to it needs an amendment, so
this never edits anything: it prints the rows that would be added, changed
or dropped, for a developer to paste in behind `since(fixExportSchemaX)`.
Run it before a release, or whenever the other network ships new types.

    bin/export-schema-check.py                       # ask s1.ripple.com
    bin/export-schema-check.py --xrpl defs.json      # or a saved file
    bin/export-schema-check.py --candidates          # also list types not yet opened
    bin/export-schema-check.py --emit                # print a full table

Only the standard library is used.
"""

import argparse
import json
import os
import re
import sys
import urllib.request

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
TABLE = "src/libxrpl/protocol/ExportSchema.cpp"
SFIELDS = "include/xrpl/protocol/detail/sfields.macro"
TRANSACTIONS = "include/xrpl/protocol/detail/transactions.macro"
SFIELD_H = "include/xrpl/protocol/SField.h"
TXFLAGS_H = "include/xrpl/protocol/TxFlags.h"
DEFAULT_XRPL = "https://s1.ripple.com:51234/"

# the other network's common transaction fields an export may not carry
EXCLUDED_UNIVERSAL = {"TxnSignature", "PreviousTxnID", "Delegate", "BatchSigners"}
# left out of the table on purpose; see the comment above it
EXCLUDED_TYPES = {"AccountDelete", "SetRegularKey", "EnableAmendment", "SetFee", "UNLModify"}
# shared, but their elements (Credential) are not, so the table omits them
EXCLUDED_FIELDS = {"AuthorizeCredentials", "UnauthorizeCredentials"}


def read(path):
    with open(os.path.join(REPO, path)) as f:
        return f.read()


def xrpl_definitions(source):
    if os.path.exists(source):
        with open(source) as f:
            data = json.load(f)
    else:
        req = urllib.request.Request(
            source,
            data=json.dumps({"method": "server_definitions"}).encode(),
            headers={"Content-Type": "application/json"},
        )
        with urllib.request.urlopen(req, timeout=30) as r:
            data = json.load(r)
    data = data.get("result", data)
    types = data["TYPES"]
    fields = {
        name: (types[f["type"]], f["nth"], f["isSerialized"])
        for name, f in data["FIELDS"]
    }
    formats = {
        tx: [(e["name"], e["optionality"]) for e in entries]
        for tx, entries in data["TRANSACTION_FORMATS"].items()
    }
    flags = {tx: dict(v) for tx, v in data["TRANSACTION_FLAGS"].items()}
    return fields, dict(data["TRANSACTION_TYPES"]), formats, flags


def local_definitions():
    sti = {m[1]: int(m[2]) for m in re.finditer(r"STYPE\(STI_(\w+),\s*(-?\d+)\)", read(SFIELD_H))}
    fields = {}
    for line in read(SFIELDS).splitlines():
        if line.lstrip().startswith("//"):
            continue
        m = re.match(r"\s*(?:TYPED|UNTYPED)_SFIELD\(\s*sf(\w+),\s*(\w+),\s*(-?\d+)", line)
        if m:
            fields[m[1]] = (sti[m[2]], int(m[3]))
    tx = {}  # name -> (tag, code)
    for m in re.finditer(r"^TRANSACTION\(\s*(tt\w+),\s*(\d+),\s*(\w+)", read(TRANSACTIONS), re.M):
        tx[m[3]] = (m[1], int(m[2]))
    flags = {m[1]: int(m[2], 0) for m in re.finditer(r"\b(tf\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)", read(TXFLAGS_H))}
    return fields, tx, flags


def shared(name, xrpl_fields, local_fields):
    theirs, ours = xrpl_fields.get(name), local_fields.get(name)
    return bool(theirs and ours and theirs[2] and theirs[:2] == ours)


def parse_table(src, local_flags):
    """{tx name: (flag mask, {field: required}, raw text)} from ExportSchema.cpp"""
    body = src[src.index("constexpr Type typeRows[] = {"):]
    body = body[: body.index("\n};")]
    # the other network's flags named locally in the file
    file_flags = {m[1]: int(m[2], 0) for m in re.finditer(r"constexpr std::uint32_t (tf\w+) = (0x[0-9A-Fa-f]+)", src)}
    rows = {}
    for chunk in re.split(r"\n    \{(?=tt)", body)[1:]:
        tag = re.match(r"(tt\w+)", chunk)[1]
        head, _, fields = chunk.partition("{")
        mask = 0
        for name in re.findall(r"\b(tf\w+)\b", head):
            mask |= local_flags.get(name) or file_flags.get(name) or 0
        req = set(re.findall(r"req\(sf(\w+)\)", fields))
        all_ = re.findall(r"\bsf(\w+)", fields)
        rows[tag] = (mask, {f: f in req for f in all_})
    return rows


def expected(tx, xrpl, local):
    xrpl_fields, _, formats, xrpl_flags = xrpl
    local_fields, _, _ = local
    fields, problems = {}, []
    for name, opt in formats.get(tx, []):
        if name in EXCLUDED_FIELDS:
            continue
        if shared(name, xrpl_fields, local_fields):
            fields[name] = opt == 0
        elif opt == 0:
            problems.append(f"requires {name}, which this network cannot send")
    mask = 0
    for v in xrpl_flags.get(tx, {}).values():
        mask |= v
    return mask, fields, problems


def flag_names(mask, tx, xrpl_flags, local_flags):
    names = []
    for name, v in sorted(xrpl_flags.get(tx, {}).items(), key=lambda kv: kv[1]):
        if mask & v:
            names.append(name if local_flags.get(name) == v else f"{v:#010x} /* {name} there */")
    return " | ".join(names) or "0"


def row(tag, mask, fields, tx, xrpl_flags, local_flags, since=None):
    fs = ", ".join((f"req(sf{n})" if r else f"sf{n}") for n, r in fields.items())
    if since:
        fs = ", ".join(f"since({x}, {since})" for x in fs.split(", ")) if fs else fs
    return f"    {{{tag}, {flag_names(mask, tx, xrpl_flags, local_flags)},\n     {{{fs}}}}},"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--xrpl", default=DEFAULT_XRPL, help="rippled URL, or a saved server_definitions json")
    ap.add_argument("--emit", action="store_true", help="print a complete table from scratch instead of a diff")
    ap.add_argument("--candidates", action="store_true", help="also list types that could be opened")
    args = ap.parse_args()

    xrpl = xrpl_definitions(args.xrpl)
    local = local_definitions()
    xrpl_fields, xrpl_tx, formats, xrpl_flags = xrpl
    local_fields, local_tx, local_flags = local

    # every type both networks number the same, that the other network has
    # a format for
    candidates = {
        name: local_tx[name][0]
        for name, code in xrpl_tx.items()
        if name in local_tx and local_tx[name][1] == code and name in formats and name not in EXCLUDED_TYPES
    }

    if args.emit:
        for name in sorted(candidates, key=lambda n: local_tx[n][1]):
            mask, fields, problems = expected(name, xrpl, local)
            if problems:
                print(f"    // {name}: " + "; ".join(problems))
                continue
            print(row(candidates[name], mask, fields, name, xrpl_flags, local_flags))
        return 0

    table = parse_table(read(TABLE), local_flags)
    by_tag = {tag: name for name, (tag, _) in local_tx.items()}
    drift = 0

    for tag, (mask, fields) in table.items():
        name = by_tag.get(tag, tag)
        if name not in candidates:
            print(f"- {name}: no longer exportable there (gate the row with until(...))")
            drift += 1
            continue
        emask, efields, problems = expected(name, xrpl, local)
        for p in problems:
            print(f"- {name}: {p}; the type must go behind until(...)")
            drift += 1
        for f in sorted(set(efields) - set(fields)):
            print(f"+ {name}: {'req' if efields[f] else 'opt'} sf{f}  ->  since({'req(' if efields[f] else ''}sf{f}{')' if efields[f] else ''}, fixExportSchemaX)")
            drift += 1
        for f in sorted(set(fields) - set(efields)):
            print(f"- {name}: sf{f} is no longer shared  ->  until(..., fixExportSchemaX)")
            drift += 1
        for f in sorted(set(fields) & set(efields)):
            if fields[f] != efields[f]:
                print(f"~ {name}: sf{f} is now {'required' if efields[f] else 'optional'} there")
                drift += 1
        if mask != emask:
            print(f"~ {name}: flags {mask:#x} -> {emask:#x} ({flag_names(emask, name, xrpl_flags, local_flags)})")
            drift += 1

    if args.candidates:
        for name in sorted(set(candidates) - set(by_tag.get(t, t) for t in table), key=lambda n: local_tx[n][1]):
            mask, fields, problems = expected(name, xrpl, local)
            if problems:
                continue
            print(f"  type {name} could be opened:")
            print(row(candidates[name], mask, fields, name, xrpl_flags, local_flags, since="fixExportSchemaX"))

    print("in sync with the other network" if not drift else f"{drift} difference(s)")
    return 1 if drift else 0


if __name__ == "__main__":
    sys.exit(main())
