"""Windows RAM corpus exchange using standard shared memory, without a network listener."""

import ctypes
import hashlib
import json
import os
import secrets
import struct
from contextlib import ExitStack, contextmanager
from multiprocessing.shared_memory import SharedMemory

MAX_CORPUS = 768 * 1024 * 1024
MAX_MANIFEST = 1024 * 1024
MAX_PROTOCOL = 65536
SLOT_BYTES = 4 + MAX_PROTOCOL
SESSION_ENV = "SUPERZIP_NEUTRON_CORPUS_MAPPING"


def mutex_api():
    """Purpose: Type the Windows mutex boundary. Inputs: None. Outputs: Configured kernel API or platform error."""
    if os.name != "nt":
        raise RuntimeError("The native Neutron corpus controller requires Windows")
    library = ctypes.WinDLL("kernel32", use_last_error=True)
    library.CreateMutexW.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_wchar_p]
    library.CreateMutexW.restype = ctypes.c_void_p
    library.OpenMutexW.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_wchar_p]
    library.OpenMutexW.restype = ctypes.c_void_p
    library.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    library.WaitForSingleObject.restype = ctypes.c_ulong
    library.ReleaseMutex.argtypes = [ctypes.c_void_p]
    library.ReleaseMutex.restype = ctypes.c_int
    library.CloseHandle.argtypes = [ctypes.c_void_p]
    library.CloseHandle.restype = ctypes.c_int
    return library


@contextmanager
def exclusive_writer(name: str):
    """Purpose: Serialize complete Hyperfine runs. Inputs: Existing session mutex. Outputs: Ownership or immediate
    failure.
    """
    library = mutex_api()
    handle = library.OpenMutexW(0x00100001, False, name)  # SYNCHRONIZE | MUTEX_MODIFY_STATE
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    owned = False
    try:
        status = library.WaitForSingleObject(handle, 0)
        owned = status in (0, 0x80)
        if status == 0x80:
            raise ValueError("A corpus worker abandoned its run; the entire study is invalid")
        if status == 0x102:
            raise ValueError("Concurrent corpus workers are unsupported")
        if status != 0:
            raise ctypes.WinError(ctypes.get_last_error())
        yield
    finally:
        if owned:
            library.ReleaseMutex(handle)
        library.CloseHandle(handle)


def prepare_session(stack: ExitStack, files: list[dict], runs: int, admit) -> dict:
    """Purpose: Transfer one corpus into owned RAM mappings. Inputs: Complete bytes, runs, admission callback.
    Outputs: Descriptor.
    """
    library = mutex_api()
    if not 1 <= len(files) <= 2000 or type(runs) is not int or not 1 <= runs <= 10:
        raise ValueError("Invalid complete corpus or repetition count")
    rows, offset = [], 0
    for member in files:
        data = member["data"]
        if not isinstance(data, bytes) or not 0 < len(data) <= MAX_CORPUS:
            raise ValueError("Invalid corpus member extent")
        if len(data) != member["bytes"] or hashlib.sha256(data).hexdigest() != member["sha256"]:
            raise ValueError("Resident corpus identity changed before publication")
        rows.append({"name": member["name"], "bytes": len(data), "sha256": member["sha256"], "offset": offset})
        offset += len(data)
    manifest = json.dumps(rows, ensure_ascii=True, separators=(",", ":")).encode("ascii")
    if offset > MAX_CORPUS or len(manifest) > MAX_MANIFEST:
        raise ValueError("Complete corpus exceeds the RAM exchange bounds")
    source_bytes = len(manifest) + offset
    result_bytes = len(files) * runs * SLOT_BYTES
    # Publication briefly overlaps original bytes; admission also reserves both child input copies.
    admit(source_bytes + result_bytes + 2 * max(row["bytes"] for row in rows))
    name = "superzip-neutron-" + secrets.token_hex(32)
    source = SharedMemory(name=name + "-source", create=True, size=source_bytes)
    stack.callback(source.close)
    results = SharedMemory(name=name + "-results", create=True, size=result_bytes)
    stack.callback(results.close)
    mutex_name = name + "-writer"
    mutex = library.CreateMutexW(None, False, mutex_name)
    if not mutex:
        raise ctypes.WinError(ctypes.get_last_error())
    stack.callback(library.CloseHandle, mutex)
    source.buf[: len(manifest)] = manifest
    for member, row in zip(files, rows, strict=True):
        start = len(manifest) + row["offset"]
        source.buf[start : start + row["bytes"]] = member.pop("data")
    # Windows creates zero-initialized mappings. A zero extent marks an unpublished observation.
    return {
        "version": 1,
        "source": source.name,
        "source_bytes": source_bytes,
        "manifest_bytes": len(manifest),
        "manifest_sha256": hashlib.sha256(manifest).hexdigest(),
        "results": results.name,
        "result_bytes": result_bytes,
        "file_count": len(files),
        "runs": runs,
        "mutex": mutex_name,
    }


def validate_descriptor(descriptor: dict) -> None:
    """Purpose: Bound mapping admission before opening. Inputs: Inherited descriptor. Outputs: Validation or
    ValueError.
    """
    if not isinstance(descriptor, dict) or type(descriptor.get("version")) is not int or descriptor["version"] != 1:
        raise ValueError("Unsupported RAM corpus exchange version")
    for key, minimum, maximum in (
        ("source_bytes", 2, MAX_CORPUS + MAX_MANIFEST),
        ("manifest_bytes", 1, MAX_MANIFEST),
        ("file_count", 1, 2000),
        ("runs", 1, 10),
        ("result_bytes", SLOT_BYTES, 20000 * SLOT_BYTES),
    ):
        if type(descriptor.get(key)) is not int or not minimum <= descriptor[key] <= maximum:
            raise ValueError("Invalid RAM corpus mapping extent")
    if descriptor["manifest_bytes"] >= descriptor["source_bytes"]:
        raise ValueError("RAM corpus has no payload")
    if descriptor["result_bytes"] != descriptor["file_count"] * descriptor["runs"] * SLOT_BYTES:
        raise ValueError("Observation capacity disagrees with the complete study")
    prefix = "superzip-neutron-"
    source = descriptor.get("source", "")
    token = source[len(prefix) : -len("-source")] if isinstance(source, str) else ""
    if len(token) != 64 or any(char not in "0123456789abcdef" for char in token):
        raise ValueError("Invalid private corpus mapping name")
    name = prefix + token
    if source != name + "-source" or descriptor.get("results") != name + "-results":
        raise ValueError("RAM corpus mapping names disagree")
    if descriptor.get("mutex") != name + "-writer":
        raise ValueError("RAM corpus writer identity disagrees")


def open_session(stack: ExitStack, descriptor: dict) -> tuple[memoryview, memoryview, list[dict]]:
    """Purpose: Open existing bounded mappings. Inputs: Descriptor and owner stack. Outputs: Read-only source,
    results, manifest.
    """
    validate_descriptor(descriptor)
    source = SharedMemory(name=descriptor["source"], create=False)
    stack.callback(source.close)
    results = SharedMemory(name=descriptor["results"], create=False)
    stack.callback(results.close)
    if source.size < descriptor["source_bytes"] or results.size < descriptor["result_bytes"]:
        raise ValueError("RAM mapping is smaller than its authenticated extent")
    view = source.buf[: descriptor["source_bytes"]].toreadonly()
    stack.callback(view.release)
    output = results.buf[: descriptor["result_bytes"]]
    stack.callback(output.release)
    manifest_bytes = bytes(view[: descriptor["manifest_bytes"]])
    if hashlib.sha256(manifest_bytes).hexdigest() != descriptor["manifest_sha256"]:
        raise ValueError("RAM corpus manifest identity changed")
    rows = json.loads(manifest_bytes)
    if not isinstance(rows, list) or len(rows) != descriptor["file_count"]:
        raise ValueError("RAM corpus inventory count changed")
    offset, names = 0, set()
    for row in rows:
        if not isinstance(row, dict) or type(row.get("bytes")) is not int or not 0 < row["bytes"] <= MAX_CORPUS:
            raise ValueError("Invalid RAM corpus member extent")
        if type(row.get("offset")) is not int or row["offset"] != offset:
            raise ValueError("RAM corpus member order or offset changed")
        name, digest = row.get("name"), row.get("sha256")
        if not isinstance(name, str) or not name or name in names:
            raise ValueError("Invalid or duplicate RAM corpus member name")
        if not isinstance(digest, str) or len(digest) != 64 or any(char not in "0123456789abcdef" for char in digest):
            raise ValueError("Invalid RAM corpus member digest")
        names.add(name)
        offset += row["bytes"]
        if offset > descriptor["source_bytes"] - descriptor["manifest_bytes"]:
            raise ValueError("RAM corpus member exceeds the remaining payload")
    if offset != descriptor["source_bytes"] - descriptor["manifest_bytes"]:
        raise ValueError("RAM corpus payload extent changed")
    return view, output, rows


def completed_slots(output: memoryview) -> int:
    """Purpose: Validate the serialized publication prefix. Inputs: Fixed observation slots. Outputs: Count or
    rejection.
    """
    count, empty = 0, False
    for offset in range(0, len(output), SLOT_BYTES):
        extent = struct.unpack_from("<I", output, offset)[0]
        if extent > MAX_PROTOCOL:
            raise ValueError("RAM observation exceeds its protocol bound")
        if not extent:
            empty = True
        elif empty:
            raise ValueError("RAM observations contain an unpublished gap")
        else:
            count += 1
    return count


def publish_observation(output: memoryview, slot: int, protocol: bytes) -> None:
    """Purpose: Commit one bounded observation. Inputs: Exclusively owned slot and protocol. Outputs: Data then
    extent.
    """
    if type(slot) is not int or not 0 <= slot < len(output) // SLOT_BYTES:
        raise ValueError("RAM observation slot exceeds the complete study")
    if not isinstance(protocol, bytes) or not 0 < len(protocol) <= MAX_PROTOCOL:
        raise ValueError("Invalid RAM observation protocol extent")
    offset = slot * SLOT_BYTES
    if struct.unpack_from("<I", output, offset)[0]:
        raise ValueError("RAM observation slot was already published")
    output[offset + 4 : offset + 4 + len(protocol)] = protocol
    struct.pack_into("<I", output, offset, len(protocol))


def observations(output: memoryview, descriptor: dict, files: list[dict], parse, *, require_complete: bool = True):
    """Purpose: Stream bounded reports after workers exit. Inputs: Slots, metadata, parser, completeness policy.
    Outputs: Each exact report; explicitly partial reports cannot qualify a complete study.
    """
    count = completed_slots(output)
    if require_complete and count != descriptor["file_count"] * descriptor["runs"]:
        raise ValueError("Incomplete public-corpus study")
    for slot in range(count):
        offset = slot * SLOT_BYTES
        extent = struct.unpack_from("<I", output, offset)[0]
        protocol = bytes(output[offset + 4 : offset + 4 + extent])
        index = slot % len(files)
        yield {
            "index": index,
            "name": files[index]["name"],
            "protocol": protocol.decode("ascii"),
            "stats": parse(protocol, files[index]),
        }
