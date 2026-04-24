"""Demo: compare classical binary, raw JSON+sig, and JsonTxCompressed wire."""

from __future__ import annotations

import json

from xrpl.core.binarycodec import encode, encode_for_signing
from xrpl.core.keypairs import (
    derive_classic_address,
    derive_keypair,
    generate_seed,
    sign,
)

from json_tx import canonical_json, compress_stream, pack_wire, unpack_wire


SAMPLE_TX = {
    "TransactionType": "Payment",
    "Account": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
    "Destination": "rf1BiGeXwwQoi8Z2ueFYTEXSwuJYfV2Jpn",
    "Amount": "1000000",
    "Fee": "12",
    "Sequence": 1,
    "Flags": 2147483648,
    "SigningPubKey": "",
}


def _report(label: str, tx: dict, tx_json_str: str, priv: str) -> None:
    signature = bytes.fromhex(sign(tx_json_str.encode().hex(), priv))

    # 1. Classical signed wire: full binary with TxnSignature (what xrpl does today).
    classical_signing = bytes.fromhex(encode_for_signing(tx))
    classical_signed_dict = dict(tx)
    classical_signed_dict["TxnSignature"] = signature.hex().upper()
    classical_wire = bytes.fromhex(encode(classical_signed_dict))

    # 2. Naive JSON submission: tx_json_str + signature (what json-tx wants to replace).
    raw_json_plus_sig = len(tx_json_str) + len(signature)

    # 3. json-tx wire: classical binary (ex TxnSignature) + JsonTxCompressed + TxnSignature.
    stream = compress_stream(tx_json_str, tx_json=tx)
    jsontx_wire = pack_wire(tx_json_str, signature)

    print(f"\n== {label} ==")
    print(f"  tx_json_str                       : {len(tx_json_str):5d} bytes")
    print(f"  signature                         : {len(signature):5d} bytes")
    print(f"  classical binary (signing payload): {len(classical_signing):5d} bytes")
    print(f"  classical wire (binary + sig)     : {len(classical_wire):5d} bytes   <- today")
    print(f"  raw JSON + sig (bytes sent)       : {raw_json_plus_sig:5d} bytes   <- naive json submit")
    print(f"  JsonTxCompressed stream alone     : {len(stream):5d} bytes   [mode=0x{stream[0]:02x}]")
    print(f"  json-tx wire (classical + stream) : {len(jsontx_wire):5d} bytes   <- proposed")
    delta_vs_classical = len(jsontx_wire) - len(classical_wire)
    print(f"    overhead vs classical wire      : {delta_vs_classical:+5d} bytes")
    delta_vs_raw = len(jsontx_wire) - raw_json_plus_sig
    print(f"    overhead vs raw JSON+sig        : {delta_vs_raw:+5d} bytes")

    recovered_tx, recovered_str, recovered_sig = unpack_wire(jsontx_wire)
    assert recovered_str == tx_json_str
    assert recovered_sig == signature
    assert recovered_tx == tx


def main() -> None:
    seed = generate_seed()
    pub, priv = derive_keypair(seed)
    tx = dict(SAMPLE_TX)
    tx["Account"] = derive_classic_address(pub)
    tx["SigningPubKey"] = pub

    _report("canonical tx_json_str (ordinal order, no whitespace)",
            tx, canonical_json(tx), priv)
    _report("non-canonical tx_json_str (insertion order + spaces)",
            tx, json.dumps(tx, separators=(", ", ": ")), priv)


if __name__ == "__main__":
    main()
