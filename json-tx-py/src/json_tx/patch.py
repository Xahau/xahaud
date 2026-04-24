"""Runtime monkey-patch: register a `JsonTxCompressed` Blob field.

Import this module (or call `register_json_tx_field()`) before using the
binary codec so that a transaction dict containing `JsonTxCompressed`
will serialize it as a Blob and parse it back out.

The field is intentionally `isSigningField=False` — the ASCII JSON is
what TxnSignature signs, not the binary form, so this field must not
participate in any classical signing payload.
"""

from __future__ import annotations

from xrpl.core.binarycodec.definitions import definitions as _d
from xrpl.core.binarycodec.definitions.field_header import FieldHeader
from xrpl.core.binarycodec.definitions.field_info import FieldInfo

FIELD_NAME = "JsonTxCompressed"
_TYPE_NAME = "Blob"


def _pick_free_nth_for_type(type_name: str) -> int:
    """Find an unused `nth` code within the given type so there's no header clash."""
    type_code = _d._TYPE_ORDINAL_MAP[type_name]
    taken = {
        h.field_code for h in _d._FIELD_HEADER_NAME_MAP if h.type_code == type_code
    }
    for n in range(1, 255):
        if n not in taken:
            return n
    raise RuntimeError(f"no free nth code for type {type_name}")


def register_json_tx_field() -> None:
    if FIELD_NAME in _d._FIELD_INFO_MAP:
        return
    nth = _pick_free_nth_for_type(_TYPE_NAME)
    info = FieldInfo(
        nth=nth,
        is_variable_length_encoded=True,
        is_serialized=True,
        is_signing_field=False,
        type_name=_TYPE_NAME,
    )
    header = FieldHeader(_d._TYPE_ORDINAL_MAP[_TYPE_NAME], nth)
    _d._FIELD_INFO_MAP[FIELD_NAME] = info
    _d._FIELD_HEADER_NAME_MAP[header] = FIELD_NAME


register_json_tx_field()
