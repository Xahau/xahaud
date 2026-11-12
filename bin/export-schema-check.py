#!/usr/bin/env python3
"""Compare the export schema table (src/libxrpl/protocol/ExportSchema.cpp)
with the other network's `server_definitions`, and say what has drifted.

The table is hand-maintained and every change to it needs an amendment, so
this never edits anything: it prints the rows that would be added, changed
or dropped, for a developer to paste in behind `since(fixExportSchemaX)`.
Run it before a release, or whenever the other network ships new types.

A row also says what its type pays the other network on top of the ordinary
fee (ExtraFee), which xport_prepare() prices an export by. No definitions
file says that, so it is checked against SPECIAL_FEES below, a list kept by
hand from the other network's code: keep it current, or this check is not.

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

# What the other network charges on top of the ordinary fee (one reference fee
# and one more per signature). `server_definitions` says nothing of fees: they
# are in each transactor's calculateBaseFee there, so THIS LIST IS KEPT BY
# HAND and is only as good as its last reading of that code (rippled develop,
# f2856b4, 2026-11-11: `grep -rn "::calculateBaseFee" src/libxrpl/tx`). Read
# it again whenever the other network ships new types.
#
# {type: {field, or None for the type whatever it holds: (what, fee)}} where
# `fee` is how the table's ExtraFee says it, (flat, present, per) in reference
# fees, or None if it cannot: such a type, or field, must stay out of the
# table, because xport_prepare() could not fill in a Fee that covers it.
SPECIAL_FEES = {
    "EscrowFinish": {
        "Fulfillment": ("32 reference fees and one more per 16 bytes", (0, 32, 16)),
        "Gas": ("the gas allowance at that network's gas price", None),
    },
    "EscrowCreate": {"Bytecode": ("9 reference fees and 5 drops per byte", None)},
    "AccountDelete": {None: ("an owner reserve", None)},
    "AMMCreate": {None: ("an owner reserve", None)},
    "LedgerStateFix": {None: ("an owner reserve", None)},
    "Batch": {None: ("its inner transactions' fees and their signers'", None)},
    "LoanSet": {None: ("a reference fee per counterparty signature", None)},
    "LoanPay": {None: ("a fee that depends on the loan's state", None)},
    "SetRegularKey": {None: ("nothing, once, by the master key", None)},
    **{
        "ConfidentialMPT" + t: {None: ("9 reference fees", (9, 0, 0))}
        for t in ("Convert", "ConvertBack", "Send", "MergeInbox", "Clawback", "MirrorUpdate", "HolderKeyUpdate")
    },
}


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


def braces(text):
    """The contents of the brace group `text` opens with, and what follows it"""
    depth = 0
    for i, c in enumerate(text):
        depth += (c == "{") - (c == "}")
        if depth == 0:
            return text[1:i], text[i + 1 :]
    return text[1:], ""


def parse_table(src, local_flags):
    """{tx tag: (flag mask, {field: required}, fee)} from ExportSchema.cpp, the
    fee as (flat, field or None, present, per), or None if the row has none"""
    body = src[src.index("constexpr Type typeRows[] = {"):]
    body = body[: body.index("\n};")]
    # the other network's flags named locally in the file
    file_flags = {m[1]: int(m[2], 0) for m in re.finditer(r"constexpr std::uint32_t (tf\w+) = (0x[0-9A-Fa-f]+)", src)}
    rows = {}
    for chunk in re.split(r"\n    \{(?=tt)", body)[1:]:
        tag = re.match(r"(tt\w+)", chunk)[1]
        head, brace, rest = chunk.partition("{")
        # the fields are the row's first brace group; a second, if there is
        # one, is its ExtraFee
        fields, rest = braces(brace + rest)
        mask = 0
        for name in re.findall(r"\b(tf\w+)\b", head):
            mask |= local_flags.get(name) or file_flags.get(name) or 0
        req = set(re.findall(r"req\(sf(\w+)\)", fields))
        all_ = re.findall(r"\bsf(\w+)", fields)
        fee = None
        m = re.match(r"\s*,\s*\{\s*(\d+)\s*(?:,\s*(?:&sf(\w+)|nullptr)\s*(?:,\s*(\d+)\s*(?:,\s*(\d+)\s*)?)?)?\}", rest)
        if m:
            fee = (int(m[1]), m[2], int(m[3] or 0), int(m[4] or 0))
        rows[tag] = (mask, {f: f in req for f in all_}, fee)
    return rows


def expected_fee(tx, fields):
    """What a row for `tx` holding `fields` must say of its fee: (fee, problems),
    the fee as parse_table gives it"""
    fee, problems = None, []
    for field, (what, units) in SPECIAL_FEES.get(tx, {}).items():
        if field is not None and field not in fields:
            continue
        where = f"with {field} it" if field else "it"
        if units is None:
            problems.append(f"{where} pays {what} there, which the table's ExtraFee cannot express")
        elif fee is not None:
            problems.append(f"{where} pays {what} there, a second rule for one row, which ExtraFee cannot hold")
        elif field is None:
            fee = (units[0], None, 0, 0)
        else:
            fee = (units[0], field, units[1], units[2])
    return fee, problems


def fee_text(fee):
    if fee is None:
        return "none"
    flat, field, present, per = fee
    return f"{{{flat}, &sf{field}, {present}, {per}}}" if field else f"{{{flat}}}"


def expected(tx, xrpl, local):
    xrpl_fields, _, formats, xrpl_flags = xrpl
    local_fields, _, _ = local
    fields, problems = {}, []
    # a field (or a whole type) whose fee the table cannot express is not one
    # to export: nothing here could price it
    unpriced = {f for f, (_, units) in SPECIAL_FEES.get(tx, {}).items() if units is None}
    if None in unpriced:
        problems.append(f"pays {SPECIAL_FEES[tx][None][0]} there, which the table's ExtraFee cannot express")
    for name, opt in formats.get(tx, []):
        if name in EXCLUDED_FIELDS:
            continue
        if name in unpriced:
            if opt == 0:
                problems.append(f"requires {name}, whose fee the table cannot express")
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
    fee, _ = expected_fee(tx, fields)
    fee = f",\n     {fee_text(fee)}" if fee else ""
    return f"    {{{tag}, {flag_names(mask, tx, xrpl_flags, local_flags)},\n     {{{fs}}}{fee}}},"


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

    for tag, (mask, fields, fee) in table.items():
        name = by_tag.get(tag, tag)
        # the fee is checked against this script's own list, whatever the
        # other network's definitions say: by the fields the row holds
        efee, fee_problems = expected_fee(name, fields)
        for p in fee_problems:
            print(f"- {name}: {p}; the row (or that field) must go behind until(...)")
            drift += 1
        if fee != efee:
            print(f"~ {name}: fee {fee_text(fee)} -> {fee_text(efee)} (SPECIAL_FEES; if the row is right, this script is behind)")
            drift += 1
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
