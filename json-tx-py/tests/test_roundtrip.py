import json

from xrpl.core.keypairs import derive_classic_address, derive_keypair, generate_seed, sign

from json_tx import compress_stream, decompress_stream, pack_wire, unpack_wire


def _signed(tx: dict) -> tuple[dict, str, bytes]:
    seed = generate_seed()
    pub, priv = derive_keypair(seed)
    tx = dict(tx)
    tx["Account"] = derive_classic_address(pub)
    tx["SigningPubKey"] = pub
    from json_tx import canonical_json
    tx_json_str = canonical_json(tx)
    sig = bytes.fromhex(sign(tx_json_str.encode().hex(), priv))
    return tx, tx_json_str, sig


def test_wire_roundtrip():
    tx, tx_json_str, sig = _signed({
        "TransactionType": "Payment",
        "Destination": "rf1BiGeXwwQoi8Z2ueFYTEXSwuJYfV2Jpn",
        "Amount": "1000000",
        "Fee": "12",
        "Sequence": 1,
        "Flags": 2147483648,
    })
    wire = pack_wire(tx_json_str, sig)
    recovered_tx, recovered_str, recovered_sig = unpack_wire(wire)
    assert recovered_str == tx_json_str
    assert recovered_sig == sig
    assert recovered_tx == tx


def test_stream_roundtrip_direct():
    tx, tx_json_str, _ = _signed({
        "TransactionType": "Payment",
        "Destination": "rf1BiGeXwwQoi8Z2ueFYTEXSwuJYfV2Jpn",
        "Amount": "2500000",
        "Fee": "15",
        "Sequence": 42,
        "Flags": 0,
    })
    stream = compress_stream(tx_json_str, tx_json=tx)
    rebuilt = decompress_stream(stream, tx)
    assert rebuilt == tx_json_str


def test_raw_fallback_preserved():
    # Unusual whitespace -> RAW opcodes. Dictionary still reconstructs losslessly.
    tx = {
        "TransactionType": "Payment",
        "Account": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
        "Destination": "rf1BiGeXwwQoi8Z2ueFYTEXSwuJYfV2Jpn",
        "Amount": "1",
        "Fee": "12",
        "Sequence": 1,
        "SigningPubKey": "",
    }
    odd = '{  "TransactionType" : "Payment"  }'
    stream = compress_stream(odd, tx_json=tx)
    assert decompress_stream(stream, tx) == odd
