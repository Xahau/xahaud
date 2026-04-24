"""
json-tx: field-aware packer for (tx_json_str, signature).

The ripple binary codec already decomposes a transaction into ordered
(field_name, canonical_bytes) pairs. We reuse that as the dictionary.

Opcode stream:
    OP_FIELD  i          -> render field i exactly as it appears in tx_json_str
    OP_TAG    t          -> emit a glue snippet from TAGS (',', ':', '"', ...)
    OP_RAW    n  <bytes> -> n bytes of literal passthrough
    OP_END               -> terminator

The stream is what gets stored in the `JsonTxCompressed` Blob field on
the wire. The TxnSignature field still carries the ed25519/secp256k1
signature, but that signature is now over the ASCII `tx_json_str`, not
the classical signing payload.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any

from xrpl.core.binarycodec.definitions.field_instance import FieldInstance
from xrpl.core.binarycodec.types.st_object import STObject

# `patch` registers the JsonTxCompressed field. Imported for its side effect.
from json_tx import patch as _patch  # noqa: F401

OP_FIELD = 0x01   # emit `"<name>":<canonical-value>` (tight pair)
OP_TAG = 0x02     # emit a structural glue byte
OP_RAW = 0x03     # length-prefixed literal bytes
OP_NAME = 0x04    # emit `"<name>"` for field i
OP_VALUE = 0x05   # emit canonical rendering of field i's value
OP_END = 0x00

# Mode byte at the head of every stream.
MODE_CANONICAL = 0x00  # body is an OP_* stream; tx_json_str reconstructs via dictionary
MODE_VERBATIM = 0x01   # body is raw UTF-8 tx_json_str, length-prefixed

JSON_TX_FIELD = _patch.FIELD_NAME

# Structural glue. INVARIANT: no tag may end in `"` -- otherwise it would
# eat the leading `"` of a field NAME/FIELD rendering and prevent re-align.
# Order: longest first so the greedy matcher picks the most specific glue.
TAGS: list[bytes] = [
    # comma-based field separators
    b",\n    ",
    b",\n\t",
    b",\n  ",
    b",\n ",
    b",\n",
    b", ",
    # colon-based name:value separators
    b":  ",
    b": ",
    # leading indent after `{`
    b"\n    ",
    b"\n\t",
    b"\n  ",
    b"\n ",
    # single chars
    b"{",
    b"}",
    b"[",
    b"]",
    b",",
    b":",
    b'"',
    b" ",
    b"\n",
    b"\t",
]


# ---------- varint (unsigned LEB128) ----------

def _vw(n: int) -> bytes:
    if n < 0:
        raise ValueError("varint must be non-negative")
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def _vr(buf: bytes, i: int) -> tuple[int, int]:
    n = 0
    shift = 0
    while True:
        b = buf[i]
        i += 1
        n |= (b & 0x7F) << shift
        if not (b & 0x80):
            return n, i
        shift += 7


# ---------- canonical field extraction ----------

@dataclass
class CanonField:
    name: str
    instance: FieldInstance
    canonical_bytes: bytes
    value: Any


def _ordered_fields_from_dict(tx_json: dict, *, skip: set[str]) -> list[CanonField]:
    """Serialize tx_json once through STObject, then re-parse to slice each field."""
    from xrpl.core.binarycodec.binary_wrappers.binary_parser import BinaryParser
    from xrpl.core.binarycodec.definitions import definitions

    tx_for_enc = {k: v for k, v in tx_json.items() if k not in skip}
    st = STObject.from_value(tx_for_enc)
    blob = bytes(st)

    parser = BinaryParser(blob.hex())
    total = len(parser)
    fields: list[CanonField] = []
    while not parser.is_end():
        start = total - len(parser)
        fi = parser.read_field()
        parser.read_field_value(fi)
        end = total - len(parser)
        fields.append(
            CanonField(
                name=fi.name,
                instance=definitions.get_field_instance(fi.name),
                canonical_bytes=blob[start:end],
                value=tx_json[fi.name],
            )
        )
    return fields


def _render_name(name: str) -> bytes:
    """Render `"Name"` including the enclosing double-quotes."""
    return json.dumps(name, separators=(",", ":")).encode()


def _render_value(value: Any) -> bytes:
    """Render the canonical JSON form of a value."""
    return json.dumps(value, separators=(",", ":")).encode()


def _render_field_json(name: str, value: Any) -> bytes:
    """Render one tight `"Name":<value>` pair -- no whitespace."""
    return _render_name(name) + b":" + _render_value(value)


def canonical_json(tx_json: dict) -> str:
    """Serialize tx_json with fields in canonical (ordinal) order.

    The signed ASCII JSON must match this ordering so the opcode stream
    can walk fields in lock-step with the binary dictionary. Any field the
    codec does not recognize falls to the tail in insertion order.
    """
    from xrpl.core.binarycodec.definitions import definitions

    known, unknown = [], []
    for k, v in tx_json.items():
        try:
            fi = definitions.get_field_instance(k)
            known.append((fi.ordinal, k, v))
        except Exception:
            unknown.append((k, v))
    known.sort(key=lambda x: x[0])
    ordered = [(k, v) for _o, k, v in known] + unknown
    body = ",".join(
        f"{json.dumps(k, separators=(',', ':'))}:"
        f"{json.dumps(v, separators=(',', ':'))}"
        for k, v in ordered
    )
    return "{" + body + "}"


# ---------- stream codec (opcode layer only) ----------

def compress_stream(tx_json_str: str, *, tx_json: dict | None = None) -> bytes:
    """Encode tx_json_str using tx_json's fields as dictionary.

    Opcodes (after the mode byte):
      OP_FIELD i   -- `"Name":<canonical-value>` tight pair (no whitespace)
      OP_NAME  i   -- `"Name"` alone (enclosing quotes included)
      OP_VALUE i   -- canonical rendering of field i's value
      OP_TAG   t   -- structural glue from TAGS
      OP_RAW   n.. -- literal passthrough

    The matcher at each cursor position tries, longest-match first:
      1. OP_FIELD against any unused field pair
      2. OP_NAME against any unused field name
      3. OP_VALUE against any unused field value
      4. OP_TAG
      5. OP_RAW (one byte, coalesced)

    If the resulting OP stream is not shorter than a verbatim copy, we
    emit MODE_VERBATIM instead.
    """
    if tx_json is None:
        tx_json = json.loads(tx_json_str)
    src = tx_json_str.encode()

    fields = _ordered_fields_from_dict(
        tx_json, skip={"TxnSignature", JSON_TX_FIELD}
    )
    name_render = [_render_name(f.name) for f in fields]
    value_render = [_render_value(f.value) for f in fields]
    pair_render = [name_render[i] + b":" + value_render[i] for i in range(len(fields))]

    unused_pair = set(range(len(fields)))
    unused_name = set(range(len(fields)))
    unused_value = set(range(len(fields)))

    body = bytearray()
    raw_buf = bytearray()

    def flush_raw() -> None:
        if raw_buf:
            body.append(OP_RAW)
            body.extend(_vw(len(raw_buf)))
            body.extend(raw_buf)
            raw_buf.clear()

    def best_match(candidates: set[int], renders: list[bytes], at: int) -> tuple[int, int]:
        best_idx, best_len = -1, 0
        for idx in candidates:
            r = renders[idx]
            if len(r) > best_len and src.startswith(r, at):
                best_idx, best_len = idx, len(r)
        return best_idx, best_len

    i = 0
    while i < len(src):
        # Try the tight FIELD match first -- cheapest per byte of output.
        idx, hit = best_match(unused_pair, pair_render, i)
        if idx >= 0:
            flush_raw()
            body.append(OP_FIELD)
            body.extend(_vw(idx))
            i += hit
            unused_pair.discard(idx)
            unused_name.discard(idx)
            unused_value.discard(idx)
            continue

        # Then NAME (standalone), preferring longer names over shorter ones.
        idx, hit = best_match(unused_name, name_render, i)
        if idx >= 0:
            flush_raw()
            body.append(OP_NAME)
            body.extend(_vw(idx))
            i += hit
            unused_name.discard(idx)
            unused_pair.discard(idx)  # no longer a "pair" candidate
            continue

        # Then VALUE (standalone).
        idx, hit = best_match(unused_value, value_render, i)
        if idx >= 0:
            flush_raw()
            body.append(OP_VALUE)
            body.extend(_vw(idx))
            i += hit
            unused_value.discard(idx)
            unused_pair.discard(idx)
            continue

        # Structural glue.
        tag_hit = -1
        for t_idx, tag in enumerate(TAGS):
            if src.startswith(tag, i):
                tag_hit = t_idx
                break
        if tag_hit >= 0:
            flush_raw()
            body.append(OP_TAG)
            body.extend(_vw(tag_hit))
            i += len(TAGS[tag_hit])
            continue

        raw_buf.append(src[i])
        i += 1

    flush_raw()
    body.append(OP_END)

    op_form = bytes([MODE_CANONICAL]) + bytes(body)
    verbatim = bytes([MODE_VERBATIM]) + _vw(len(src)) + src
    return op_form if len(op_form) <= len(verbatim) else verbatim


def decompress_stream(stream: bytes, tx_json_for_dict: dict) -> str:
    """Rebuild tx_json_str from a json-tx stream + a parsed tx dict (dictionary source)."""
    mode = stream[0]
    i = 1
    if mode == MODE_VERBATIM:
        ln, i = _vr(stream, i)
        return stream[i : i + ln].decode()
    if mode != MODE_CANONICAL:
        raise ValueError(f"unknown json-tx stream mode 0x{mode:02x}")

    fields = _ordered_fields_from_dict(
        tx_json_for_dict,
        skip={"TxnSignature", JSON_TX_FIELD},
    )
    name_render = [_render_name(f.name) for f in fields]
    value_render = [_render_value(f.value) for f in fields]
    pair_render = [name_render[i] + b":" + value_render[i] for i in range(len(fields))]

    out = bytearray()
    while i < len(stream):
        op = stream[i]
        i += 1
        if op == OP_END:
            break
        if op == OP_FIELD:
            idx, i = _vr(stream, i)
            out += pair_render[idx]
        elif op == OP_NAME:
            idx, i = _vr(stream, i)
            out += name_render[idx]
        elif op == OP_VALUE:
            idx, i = _vr(stream, i)
            out += value_render[idx]
        elif op == OP_TAG:
            idx, i = _vr(stream, i)
            out += TAGS[idx]
        elif op == OP_RAW:
            ln, i = _vr(stream, i)
            out += stream[i : i + ln]
            i += ln
        else:
            raise ValueError(f"unknown opcode 0x{op:02x} at offset {i - 1}")
    return out.decode()


# ---------- wire-tx pack/unpack (full binary with JsonTxCompressed) ----------

def pack_wire(tx_json_str: str, signature: bytes) -> bytes:
    """Build the on-wire binary tx: canonical binary + JsonTxCompressed + TxnSignature.

    `tx_json_str` is the exact ASCII bytes the client signed -- any field
    order / whitespace. `signature` is the raw signature over those bytes.
    """
    from xrpl.core.binarycodec.main import encode

    tx_json = json.loads(tx_json_str)
    stream = compress_stream(tx_json_str, tx_json=tx_json)
    wire_dict = dict(tx_json)
    wire_dict[JSON_TX_FIELD] = stream.hex().upper()
    wire_dict["TxnSignature"] = signature.hex().upper()
    return bytes.fromhex(encode(wire_dict))


def unpack_wire(wire: bytes) -> tuple[dict, str, bytes]:
    """Decode the wire tx back into (tx_json_dict, tx_json_str, signature)."""
    from xrpl.core.binarycodec.main import decode

    decoded = decode(wire.hex().upper())
    stream_hex = decoded.pop(JSON_TX_FIELD)
    sig_hex = decoded.pop("TxnSignature")
    # The dictionary is the other fields of the tx, i.e. the decoded dict
    # minus the scaffolding keys (already removed above).
    tx_json_str = decompress_stream(bytes.fromhex(stream_hex), decoded)
    return json.loads(tx_json_str), tx_json_str, bytes.fromhex(sig_hex)


# ---------- convenience ----------

def pack(tx_json: dict, signature: bytes) -> bytes:
    """Alias for pack_wire for the common case."""
    return pack_wire(tx_json, signature)


def unpack(wire: bytes) -> tuple[dict, bytes]:
    tx_json, _, sig = unpack_wire(wire)
    return tx_json, sig
