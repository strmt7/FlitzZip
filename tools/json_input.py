"""Decode UTF-8 JSON without silently resolving ambiguous or nonfinite input.

Callers own byte budgets and domain/schema validation. This module performs no
I/O and never includes input keys or values in its validation errors.
"""

from __future__ import annotations

import json
import math
from typing import NoReturn


def unique_fields(pairs: list[tuple[str, object]]) -> dict[str, object]:
    """Purpose: Reject duplicate properties. Inputs: Decoded pairs. Outputs: Unique object or ValueError."""
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("JSON object contains duplicate properties")
        result[key] = value
    return result


def finite_number(text: str) -> float:
    """Purpose: Reject floating-point overflow. Inputs: JSON number. Outputs: Finite float or ValueError."""
    number = float(text)
    if not math.isfinite(number):
        raise ValueError("JSON numbers must be finite")
    return number


def reject_constant(_text: str) -> NoReturn:
    """Purpose: Reject non-JSON constants. Inputs: Constant token. Outputs: Always raises ValueError."""
    raise ValueError("JSON numbers must be finite")


def decode_json(payload: bytes | str) -> object:
    """Purpose: Decode unambiguous JSON. Inputs: Bounded UTF-8 bytes/text. Outputs: Value or decoding error."""
    text = payload.decode("utf-8") if isinstance(payload, bytes) else payload
    return json.loads(text, object_pairs_hook=unique_fields, parse_float=finite_number, parse_constant=reject_constant)
