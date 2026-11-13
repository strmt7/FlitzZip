"""Independent fixed rANS framing for the private parser's sanitizer target."""

import argparse
import struct
import zipfile
from pathlib import Path

LOWER_STATE = 1 << 23


def singleton(decoded: int, precision: int, bits: int, shift: int, symbol: int) -> bytes:
    """Frame implicit full-mass rows; no symbol coding algorithm is involved."""
    contexts = sorted({0, symbol >> (8 - bits)}) if bits else [0]
    count = (decoded + (1 << shift) - 1) >> shift
    models = bytes((precision, bits, shift, len(contexts) - 1))
    models += b"".join(bytes((context, 0, symbol)) for context in contexts)
    offsets = struct.pack(f"<{count + 1}I", *(4 * i for i in range(count + 1)))
    return struct.pack("<I", decoded) + models + offsets + struct.pack("<I", LOWER_STATE) * count


def seed_cases() -> dict[str, bytes]:
    """Return valid independent identities and deep structural/stream corruptions."""
    full = singleton(513, 16, 8, 8, 255)
    unconditional = singleton(257, 8, 0, 8, 0)
    # At p=8, equal frequencies 128 halve the decoding state. Every eight
    # zero symbols consume one zero byte and restore L; 256 zeros need 32 bytes.
    binary = struct.pack("<I", 256) + bytes((8, 0, 8, 0, 0, 1, 0, 128, 0, 255, 128, 0))
    binary += struct.pack("<III", 0, 36, LOWER_STATE) + bytes(32)
    terminal = bytearray(unconditional)
    terminal[-4:] = struct.pack("<I", LOWER_STATE + 1)
    overlap = bytearray(unconditional)
    overlap[15:19] = struct.pack("<I", 3)
    missing = singleton(513, 16, 0, 8, 255)
    missing = missing[:5] + bytes((8,)) + missing[6:]
    bad_mass = bytearray(binary)
    bad_mass[14:16] = struct.pack("<H", 127)
    return {
        "singleton-unconditional": unconditional,
        "singleton-full-width-contexts": full,
        "singleton-segment-tail": singleton(4097, 12, 0, 12, 65),
        "binary-balanced": binary,
        "bad-terminal-state": bytes(terminal),
        "bad-dense-offset": bytes(overlap),
        "missing-used-context": missing,
        "bad-model-mass": bytes(bad_mass),
        "truncated-stream": full[:-1],
    }


def write_corpus(destination: Path) -> None:
    """Write deterministic generated seeds; never track binary corpus archives."""
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_STORED) as archive:
        for name, payload in seed_cases().items():
            entry = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            entry.create_system = 3
            entry.external_attr = 0o100600 << 16
            entry.compress_type = zipfile.ZIP_STORED
            archive.writestr(entry, payload)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    write_corpus(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
