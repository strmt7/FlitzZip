"""Offline public-corpus admission, byte-preservation and native-protocol contracts."""

import base64
import ctypes
import gzip
import hashlib
import io
import json
import os
import shlex
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from contextlib import ExitStack
from multiprocessing.shared_memory import SharedMemory
from pathlib import Path
from types import SimpleNamespace
from unittest import mock
from urllib.request import Request

from tools import neutron_corpus_benchmark as corpus
from tools import neutron_corpus_ipc as ipc


# Purpose: Construct a tiny owned ZIP for archive-admission tests without fetching or writing a public corpus.
# Inputs: Explicit name/payload pairs and optional Unix symlink metadata.
# Outputs: In-memory ZIP bytes and a matching extent pin; fixtures are never performance evidence.
def zip_fixture(rows: list[tuple[str, bytes]], *, link: bool = False) -> tuple[bytes, dict]:
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in rows:
            member = zipfile.ZipInfo(name)
            member.filename = name
            if link:
                member.create_system = 3
                member.external_attr = (stat.S_IFLNK | 0o777) << 16
            archive.writestr(member, data)
    return output.getvalue(), {"file_count": len(rows), "decoded_bytes": sum(len(data) for _, data in rows)}


# Purpose: Exercise reviewed natural-file ingestion and exact HIP observation admission without native work.
# Inputs: Tiny controlled archive/protocol fixtures and mocked fixed-source network responses.
# Outputs: Rejects malformed, changed, lossy, substituted or disk-writing observations; no benchmark is launched.
class CorpusContracts(unittest.TestCase):
    def test_published_corpus_studies(self) -> None:
        """Purpose: Verify the records consumed by current guides without acquiring or executing a corpus.
        Inputs: Published JSON and canonical corpus pins. Outputs: Complete repeated HIP evidence or a failure.
        """
        root = Path(__file__).resolve().parents[1]
        pins = json.loads((root / "docs/benchmarks/corpora/neutron-public-corpora.json").read_text("utf-8"))
        for name in ("Canterbury", "Pythia14M"):
            with self.subTest(corpus=name):
                path = root / f"docs/benchmarks/data/neutron-{name.lower()}.json"
                record = json.loads(path.read_text("utf-8"))
                self.assertEqual(record["corpus"], name)
                self.assertTrue(record["study_qualified"])
                self.assertFalse(record["timing_qualified"])
                self.assertTrue(record["memory_only"])
                self.assertEqual(record["disk_write_bytes"], 0)
                provenance, summary = record["provenance"], record["summary"]
                self.assertEqual(provenance["archive_sha256"], pins[name]["archive_sha256"])
                self.assertEqual(provenance["input_bytes"], pins[name]["decoded_bytes"])
                self.assertEqual(provenance["published_file_count"], pins[name]["file_count"])
                self.assertEqual(provenance["excluded"], [])
                count = provenance["admitted_file_count"]
                self.assertEqual(count, provenance["published_file_count"])
                self.assertEqual(len(record["runs"]), summary["observation_count"])
                self.assertEqual(len(record["runs"]), count * len(summary["totals"]))
                self.assertGreaterEqual(len(summary["totals"]), 3)
                previous = None
                for run, total in enumerate(summary["totals"], 1):
                    self.assertEqual(total["run"], run)
                    selected = [row for row in record["runs"] if row["run"] == run]
                    self.assertEqual(len({row["name"] for row in selected}), count)
                    self.assertEqual([row["index"] for row in selected], list(range(count)))
                    identities = []
                    for row in selected:
                        fields = row["stats"]
                        payload = " ".join(f"{key}={value}" for key, value in fields.items()).encode("ascii")
                        parsed = corpus.parse_stats(
                            payload, {"sha256": fields["source_sha256"], "bytes": int(fields["input_bytes"])}
                        )
                        self.assertEqual(parsed, fields)
                        self.assertEqual(int(fields["block_size_bytes"]), record["block_size_kib"] * 1024)
                        identities.append((row["name"], fields["source_sha256"], fields["archive_bytes"]))
                    for field in ("input_bytes", "archive_bytes"):
                        self.assertEqual(sum(int(row["stats"][field]) for row in selected), total[field])
                    self.assertEqual(total["input_bytes"], provenance["input_bytes"])
                    if previous is not None:
                        self.assertEqual(identities, previous)
                    previous = identities

    def test_pinned_publisher_redirect_boundary(self) -> None:
        """Purpose: Guard CDN transport. Inputs: Publisher URLs and boundary mutations. Outputs: Only bounded HTTPS."""
        handler = corpus.PinnedCorpusRedirects(["huggingface.co", "hf.co"])
        source = Request("https://huggingface.co/organization/model")
        for url in (source.full_url, "https://eu.cdn.hf.co/model?opaque=delivery", "https://hf.co:443/model"):
            redirected = handler.redirect_request(source, None, 302, "Found", {}, url)
            self.assertEqual(redirected.full_url, url)
        for scheme, authority, suffix in (
            ("http", "hf.co", "/model"),
            ("https", "hf.co.example.org", "/model"),
            ("https", "otherhf.co", "/model"),
            ("https", "user@hf.co", "/model"),
            ("https", "hf.co:8443", "/model"),
            ("https", "hf.co:invalid", "/model"),
            ("https", "hf.co.", "/model"),
            ("https", "hf.co", "/model#fragment"),
            ("https", "hf.co", "/model\n"),
            ("https", "hf.co", "/" + "x" * 16384),
        ):
            with self.subTest(scheme=scheme, authority=authority), self.assertRaisesRegex(ValueError, "boundary"):
                handler.redirect_request(source, None, 302, "Found", {}, f"{scheme}://{authority}{suffix}")
        for domains in ([], "hf.co", [None], ["HF.co"], ["hf.co/"], ["hf.co", "hf.co"], ["192.0.2.1"]):
            with self.subTest(domains=domains), self.assertRaisesRegex(ValueError, "domains"):
                corpus.PinnedCorpusRedirects(domains)
        self.assertEqual((handler.max_redirections, handler.max_repeats), (3, 1))

    def test_redirected_download_authenticates_complete_artifact(self) -> None:
        """Purpose: Bind delivered bytes. Inputs: RAM response and pin mutations. Outputs: Exact digest only."""
        payload = bytes(range(256))
        pin = {
            "url": "https://huggingface.co/organization/model",
            "redirect_domains": ["huggingface.co", "hf.co"],
            "archive_sha256": hashlib.sha256(payload).hexdigest(),
            "archive_bytes": len(payload),
        }
        response = io.BytesIO(payload)
        response.status, response.headers = 200, {}
        with mock.patch.object(corpus, "build_opener") as opener:
            opener.return_value.open.return_value = response
            self.assertEqual(corpus.download(pin, deadline=float("inf")), payload)
            self.assertIsInstance(opener.call_args.args[0], corpus.PinnedCorpusRedirects)
        for changed in (
            {**pin, "archive_sha256": ""},
            {**pin, "archive_sha256": "a" * 63},
            {**pin, "redirect_domains": []},
            {**pin, "url": "https://example.org/model"},
        ):
            with mock.patch.object(corpus, "build_opener") as opener, self.assertRaises(ValueError):
                corpus.download(changed, deadline=float("inf"))
            opener.assert_not_called()
        for changed in ({**pin, "archive_sha256": "a" * 64}, {**pin, "archive_bytes": 255}):
            response = io.BytesIO(payload)
            response.status, response.headers = 200, {}
            with mock.patch.object(corpus, "build_opener") as opener, self.assertRaises(ValueError):
                opener.return_value.open.return_value = response
                corpus.download(changed, deadline=float("inf"))

    def test_complete_single_artifact_preserves_bytes(self) -> None:
        """Purpose: Admit inert checkpoints. Inputs: Binary file and identity mutations. Outputs: No selection."""
        payload = bytes(range(256)) + b"\x00\r\n\x1a"
        pin = {
            "file_name": "model.safetensors",
            "file_count": 1,
            "decoded_bytes": len(payload),
            "max_file_bytes": len(payload),
            "archive_sha256": hashlib.sha256(payload).hexdigest(),
        }
        files, provenance = corpus.decode_single_file(payload, pin)
        self.assertIs(files[0]["data"], payload)
        self.assertEqual((len(files), provenance["input_bytes"], provenance["excluded"]), (1, len(payload), []))
        for key, value in (
            ("file_count", 2),
            ("decoded_bytes", len(payload) - 1),
            ("max_file_bytes", len(payload) - 1),
            ("archive_sha256", "a" * 64),
            ("file_name", "../model.safetensors"),
            ("file_name", "C:model.safetensors"),
            ("file_name", "model\0.safetensors"),
        ):
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                corpus.decode_single_file(payload, {**pin, key: value})
        with self.assertRaises(ValueError):
            corpus.decode_single_file(payload[:-1] + b"x", pin)

    def test_model_permission_is_bound_before_acquisition(self) -> None:
        """Purpose: Refuse unreviewed revisions. Inputs: Model preset and denial. Outputs: No native or network work."""
        revision = json.loads(corpus.PINS.read_text(encoding="utf-8"))["Pythia14M"]["revision"]
        with (
            mock.patch.object(
                corpus, "require_permission", side_effect=ValueError("test revision not reviewed")
            ) as permission,
            mock.patch.object(corpus, "validate_current") as native,
            mock.patch.object(corpus, "download") as download,
            self.assertRaisesRegex(ValueError, "revision not reviewed"),
        ):
            corpus.study(SimpleNamespace(corpus="Pythia14M"))
        permission.assert_called_once_with("Pythia14M", "execute", version=revision)
        native.assert_not_called()
        download.assert_not_called()

    def test_explicit_long_file_deadline_remains_bounded(self) -> None:
        """Purpose: Admit slow natural files. Inputs: Real CLI deadline arguments. Outputs: Explicit bounded
        limits; no worker or native launch.
        """
        for arguments, expected in (([], 300), (["--file-timeout", "3600"], 3600)):
            with mock.patch.object(sys, "argv", ["corpus", *arguments]), mock.patch.object(corpus, "study") as study:
                corpus.main()
                self.assertEqual(study.call_args.args[0].file_timeout, expected)
                self.assertEqual(study.call_args.args[0].suite_timeout, 3600)
        for value in ("0", "3601"):
            with (
                mock.patch.object(sys, "argv", ["corpus", "--file-timeout", value]),
                mock.patch.object(corpus, "study") as study,
                mock.patch.object(sys, "stderr", io.StringIO()),
                self.assertRaises(SystemExit) as rejected,
            ):
                corpus.main()
            self.assertEqual(rejected.exception.code, 2)
            study.assert_not_called()

    def test_hyperfine_parser_preserves_windows_arguments(self) -> None:
        """Purpose: Preserve parser-specific arguments. Inputs: Windows paths and quotes. Outputs: Exact tokens."""
        arguments = ["C:\\Program Files\\Python's\\python.exe", "-B", "--worker", "quotes ' \" $ ` ;", ""]
        self.assertEqual(shlex.split(corpus.hyperfine_command(arguments)), arguments)
        for invalid in ([], [None], ["embedded\0null"]):
            with self.assertRaises(ValueError):
                corpus.hyperfine_command(invalid)

    # Purpose: Preserve every member byte and published order, including binary and text boundary markers.
    # Inputs: Independent ZIP members with NUL, Ctrl-Z, CRLF and all byte values.
    # Outputs: Requires unmodified bytes, exact SHA-256, correct totals and no artificial exclusions.
    def test_original_bytes_order_and_hashes(self) -> None:
        rows = [("a/x.txt", b"\x00\r\n\x1a"), ("b/y.bin", bytes(range(256)))]
        data, pin = zip_fixture(rows)
        files, provenance = corpus.decode_corpus(data, pin)
        self.assertEqual([(row["name"], row["data"]) for row in files], rows)
        self.assertEqual(files[1]["sha256"], hashlib.sha256(rows[1][1]).hexdigest())
        self.assertEqual(provenance["admitted_file_count"], 2)
        self.assertEqual(provenance["input_bytes"], 260)
        self.assertEqual(provenance["excluded"], [])

    # Purpose: Reject unsafe names, special members, duplicates and changes to the complete published inventory.
    # Inputs: Independently malformed tiny ZIPs; limits are checked before public-file decoding.
    # Outputs: Every unsupported ZIP raises ValueError instead of selecting a convenient subset.
    def test_inventory_rejections(self) -> None:
        for name in ("../x", "/x", "C:x", "a\\x", "a//x", "a/./x"):
            data, pin = zip_fixture([(name, b"x")])
            with self.subTest(name=name), self.assertRaises(ValueError):
                corpus.decode_corpus(data, pin)
        data, pin = zip_fixture([("x", b"x")], link=True)
        with self.assertRaises(ValueError):
            corpus.decode_corpus(data, pin)
        data, pin = zip_fixture([("x", b"x")])
        for changed in ({**pin, "file_count": 2}, {**pin, "decoded_bytes": 2}, {**pin, "file_bytes": {"y": 1}}):
            with self.assertRaises(ValueError):
                corpus.decode_corpus(data, changed)
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            member = archive.infolist()[0]
            with self.assertRaises(ValueError):
                corpus.validate_members([member, member], {"file_count": 2, "decoded_bytes": 2})

    # Purpose: Reject corrupt member CRCs before any native compression consumes the fixture.
    # Inputs: A ZIP with one altered stored payload byte and intact original CRC metadata.
    # Outputs: The standard ZIP decoder rejects the corrupted source rather than issuing a new accepted identity.
    def test_corrupt_member_crc(self) -> None:
        data, pin = zip_fixture([("x", b"abcdef")])
        changed = bytearray(data)
        changed[data.index(b"abcdef")] ^= 1
        with self.assertRaises(zipfile.BadZipFile):
            corpus.decode_corpus(bytes(changed), pin)

    # Purpose: Require the original canonical TAR inventory and preserve text line endings exactly.
    # Inputs: A fixed tiny TAR/gzip with CRLF plus a second pin claiming normalized text extent.
    # Outputs: Original bytes pass; a changed member extent and excessive decoded container both fail.
    def test_original_tar_extent_and_decoded_bound(self) -> None:
        output = io.BytesIO()
        with tarfile.open(fileobj=output, mode="w") as archive:
            member = tarfile.TarInfo("alice29.txt")
            member.size = 4
            archive.addfile(member, io.BytesIO(b"a\r\nb"))
        data = gzip.compress(output.getvalue())
        pin = {"file_count": 1, "container_bytes_max": len(output.getvalue()), "file_bytes": {"alice29.txt": 4}}
        files, _ = corpus.decode_canterbury(data, pin)
        self.assertEqual(files[0]["data"], b"a\r\nb")
        for changed in ({**pin, "file_bytes": {"alice29.txt": 3}}, {**pin, "container_bytes_max": 10}):
            with self.assertRaises(ValueError):
                corpus.decode_canterbury(data, changed)

    # Purpose: Require complete published-corpus pins rather than a synthetic/repository replacement.
    # Inputs: Checked-in source metadata and its independent known canonical lengths and natural-file counts.
    # Outputs: Both presets retain the real full workload, including the 175 MB Govdocs member.
    def test_canonical_pins(self) -> None:
        pins = json.loads(corpus.PINS.read_text(encoding="utf-8"))
        self.assertEqual(sum(pins["Canterbury"]["file_bytes"].values()), 2810784)
        self.assertEqual(pins["Canterbury"]["file_bytes"]["alice29.txt"], 152089)
        self.assertEqual(pins["Govdocs1Thread0"]["file_count"], 991)
        self.assertLess(pins["Govdocs1Thread0"]["max_file_bytes"], corpus.MAX_FILE)

    # Purpose: Keep complete archive sizes and actual GPU work bound to each exact RAM source.
    # Inputs: Native protocol with original identity and independent invalid memory/mode/hash/extent/GPU mutations.
    # Outputs: Correct protocol passes; every substitution fails closed.
    def test_native_telemetry_contract(self) -> None:
        member = {"bytes": 7, "sha256": "a" * 64}
        fields = {
            "input_bytes": "7",
            "validated_bytes": "7",
            "source_sha256": "a" * 64,
            "compression_mode": "neutron_star",
            "memory_only": "true",
            "disk_write_bytes": "0",
            "measurement_protocol": "bytewise-corpus-v1",
            "data_source": "preloaded",
            "gpu_used": "true",
            "gpu_encode_chunks": "1",
            "gpu_decode_chunks": "1",
            "gpu_kernel_launches": "1",
            "archive_bytes": "116",
            "output_bytes": "3",
            "compress_seconds": "0.125",
        }

        # Purpose: Serialize controlled native fields for independent admission mutations.
        # Inputs: Fixture fields. Outputs: One ASCII key/value protocol line.
        def encode(values: dict) -> bytes:
            return " ".join(f"{key}={value}" for key, value in values.items()).encode()

        self.assertEqual(corpus.parse_stats(encode(fields), member), fields)
        expanded = {**fields, "output_bytes": "8"}
        self.assertEqual(corpus.parse_stats(encode(expanded), member), expanded)
        for key, value in (
            ("input_bytes", "8"),
            ("validated_bytes", "0"),
            ("source_sha256", "b" * 64),
            ("compression_mode", "standard"),
            ("disk_write_bytes", "1"),
            ("memory_only", "false"),
            ("gpu_used", "false"),
            ("gpu_decode_chunks", "0"),
            ("archive_bytes", "0"),
            ("output_bytes", "0"),
            ("output_bytes", "-1"),
            ("output_bytes", "117"),
            ("compress_seconds", "nan"),
            ("compress_seconds", "inf"),
            ("compress_seconds", "-1"),
        ):
            with self.subTest(key=key), self.assertRaises(ValueError):
                corpus.parse_stats(encode({**fields, key: value}), member)

    # Purpose: Validate binary process transport and enforce bounded output/deadline cleanup.
    # Inputs: Owned Python children echoing binary bytes, overflowing stdout or waiting beyond the caller deadline.
    # Outputs: Preserves every byte and rejects overflow/timeout without leaving the owned child alive.
    def test_owned_process_transport(self) -> None:
        data = bytes(range(256))
        command = [sys.executable, "-B", "-c", "import sys; sys.stdout.buffer.write(sys.stdin.buffer.read())"]
        self.assertEqual(corpus.run_process(command, data, 10), data)
        with self.assertRaises(ValueError):
            corpus.run_process(
                [sys.executable, "-B", "-c", "import sys; sys.stdout.buffer.write(b'x'*65537)"], None, 10
            )
        with self.assertRaises(subprocess.TimeoutExpired):
            corpus.run_process([sys.executable, "-B", "-c", "import time; time.sleep(30)"], None, 1)

    # Purpose: Preserve an orchestrator's worker diagnostic without admitting the failed measurement.
    # Inputs: Owned child with distinct bounded stdout/stderr and nonzero exit. Outputs: Both causes survive failure.
    def test_owned_failure_preserves_both_streams(self) -> None:
        code = "import sys; print('worker-root-cause'); print('orchestrator-failed', file=sys.stderr); sys.exit(7)"
        with self.assertRaises(ValueError) as failure:
            corpus.run_process([sys.executable, "-B", "-c", code], None, 10)
        self.assertIn("benchmark child failed (7)", str(failure.exception))
        self.assertIn("stdout=worker-root-cause", str(failure.exception))
        self.assertIn("stderr=orchestrator-failed", str(failure.exception))

    # Purpose: Distinguish successful tool diagnostics from the strict native telemetry protocol.
    # Inputs: Real owned children with arbitrary binary stderr, zero/nonzero exit and excessive diagnostics.
    # Outputs: Explicit byte retention permits success; strict callers, failures and output overflow still reject.
    def test_successful_diagnostics_require_explicit_retention(self) -> None:
        code = "import sys; sys.stdout.buffer.write(b'result'); sys.stderr.buffer.write(bytes(range(256)))"
        command = [sys.executable, "-B", "-c", code]
        with self.assertRaisesRegex(ValueError, "benchmark child failed \\(0\\)"):
            corpus.run_process(command, None, 10)
        diagnostics: list[bytes] = []
        self.assertEqual(corpus.run_process(command, None, 10, successful_stderr=diagnostics), b"result")
        self.assertEqual(diagnostics, [bytes(range(256))])
        for child in (code + "; sys.exit(7)", "import sys; sys.stderr.buffer.write(b'x'*65537)"):
            diagnostics = []
            with self.subTest(child=child), self.assertRaises(ValueError):
                corpus.run_process([sys.executable, "-B", "-c", child], None, 10, successful_stderr=diagnostics)
            self.assertEqual(diagnostics, [])


def resident_fixture() -> list[dict]:
    """Purpose: Own tiny binary IPC inputs. Inputs: None. Outputs: Natural boundary-marker fixtures, never
    benchmark evidence.
    """
    return [
        {"name": name, "data": data, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        for name, data in (("one", bytes(range(256))), ("two", b"\x00\r\n\x1a"))
    ]


def protocol_fixture(data: bytes) -> bytes:
    """Purpose: Produce a labelled parser fixture. Inputs: Exact test bytes. Outputs: Mock protocol, never actual
    HIP evidence.
    """
    return (
        f"input_bytes={len(data)} validated_bytes={len(data)} output_bytes={len(data)} "
        f"source_sha256={hashlib.sha256(data).hexdigest()} compression_mode=neutron_star "
        "memory_only=true disk_write_bytes=0 measurement_protocol=bytewise-corpus-v1 data_source=preloaded "
        f"gpu_used=true gpu_encode_chunks=1 gpu_decode_chunks=1 gpu_kernel_launches=1 archive_bytes={len(data) + 109} "
        "compress_seconds=0.125"
    ).encode("ascii")


@unittest.skipUnless(os.name == "nt", "Windows native controller")
class RamExchangeContracts(unittest.TestCase):
    """Purpose: Test real Windows IPC failure boundaries. Inputs: Owned tiny mappings. Outputs: Byte/lifetime
    rejection proofs.
    """

    def test_cross_process_worker_and_cleanup(self) -> None:
        """Purpose: Exercise production IPC and worker twice. Inputs: Separate CPU processes. Outputs: Ordered
        complete slots.
        """
        files = resident_fixture()
        with ExitStack() as stack:
            descriptor = ipc.prepare_session(stack, files, 2, corpus.admit_memory)
            self.assertTrue(all("data" not in row for row in files))
            source, output, rows = ipc.open_session(stack, descriptor)
            self.assertTrue(source.readonly)
            with self.assertRaises(TypeError):
                source[0] = 1
            code = (
                "import json, os, socket; from pathlib import Path; from unittest.mock import patch; "
                "from tools import neutron_corpus_benchmark as c, neutron_corpus_ipc as i; "
                "from tools.test_neutron_corpus_benchmark import protocol_fixture; "
                "descriptor=json.loads(os.environ[i.SESSION_ENV]); "
                "with_context=patch('socket.socket', side_effect=AssertionError('IPC must not open a socket')); "
                "with_context.start(); c.run_process=lambda args,data,timeout: protocol_fixture(data); "
                "c.worker(descriptor,Path('test-only-native-fixture'),256,10)"
            )
            environment = {**os.environ, ipc.SESSION_ENV: json.dumps(descriptor)}
            for _ in range(2):
                self.assertEqual(corpus.run_process([sys.executable, "-B", "-c", code], None, 10, env=environment), b"")
            reports = list(ipc.observations(output, descriptor, files, corpus.parse_stats))
            self.assertEqual([row["index"] for row in reports], [0, 1, 0, 1])
            self.assertEqual([row["stats"]["input_bytes"] for row in reports], ["256", "4", "256", "4"])
            with (
                mock.patch.object(corpus, "run_process", side_effect=AssertionError("excess run reached native")),
                self.assertRaises(ValueError),
            ):
                corpus.worker(descriptor, corpus.ROOT / "test-only-native-fixture", 256, 10)
            self.assertEqual(len(rows), 2)
        for name in (descriptor["source"], descriptor["results"]):
            with self.assertRaises(FileNotFoundError):
                SharedMemory(name=name, create=False)

    def test_admission_before_mapping_creation(self) -> None:
        """Purpose: Reject unsafe growth before allocation. Inputs: Refused RAM admission. Outputs: No mapping is
        created.
        """
        with ExitStack() as stack, mock.patch.object(ipc, "SharedMemory") as create:
            with self.assertRaises(MemoryError):
                ipc.prepare_session(stack, resident_fixture(), 1, mock.Mock(side_effect=MemoryError("fixture")))
            create.assert_not_called()

    def test_corrupt_descriptor_and_manifest(self) -> None:
        """Purpose: Reject altered extents and metadata. Inputs: Independent descriptor/manifest mutations.
        Outputs: Fail closed.
        """
        with ExitStack() as stack:
            descriptor = ipc.prepare_session(stack, resident_fixture(), 1, corpus.admit_memory)
            for key, value in (
                ("source_bytes", -1),
                ("result_bytes", 1),
                ("runs", True),
                ("file_count", 2001),
                ("mutex", "other"),
                ("source", "other"),
            ):
                with self.subTest(key=key), self.assertRaises(ValueError):
                    ipc.open_session(stack, {**descriptor, key: value})
            mapping = SharedMemory(name=descriptor["source"], create=False)
            stack.callback(mapping.close)
            mapping.buf[0] ^= 1
            with self.assertRaises(ValueError):
                ipc.open_session(stack, descriptor)

    def test_slots_reject_gaps_duplicates_overflow_and_partial_runs(self) -> None:
        """Purpose: Reject incomplete or substituted results. Inputs: Slot mutations. Outputs: No partial study
        admission.
        """
        files = resident_fixture()
        with ExitStack() as stack:
            descriptor = ipc.prepare_session(stack, files, 1, corpus.admit_memory)
            _, output, _ = ipc.open_session(stack, descriptor)
            with self.assertRaises(ValueError):
                list(ipc.observations(output, descriptor, files, corpus.parse_stats))
            for slot, payload in ((-1, b"x"), (2, b"x"), (0, b""), (0, b"x" * (ipc.MAX_PROTOCOL + 1))):
                with self.assertRaises(ValueError):
                    ipc.publish_observation(output, slot, payload)
            ipc.publish_observation(output, 1, b"x")
            with self.assertRaises(ValueError):
                ipc.completed_slots(output)
            struct.pack_into("<I", output, ipc.SLOT_BYTES, 0)
            ipc.publish_observation(output, 0, protocol_fixture(bytes(range(256))))
            with self.assertRaises(ValueError):
                ipc.publish_observation(output, 0, b"x")
            with (
                mock.patch.object(corpus, "run_process", side_effect=AssertionError("partial run reached native")),
                self.assertRaises(ValueError),
            ):
                corpus.worker(descriptor, corpus.ROOT / "test-only-native-fixture", 256, 10)
            struct.pack_into("<I", output, 0, ipc.MAX_PROTOCOL + 1)
            with self.assertRaises(ValueError):
                ipc.completed_slots(output)

    def test_concurrent_writer_rejected(self) -> None:
        """Purpose: Prevent overlapping measured runs. Inputs: A held named mutex. Outputs: A separate worker fails
        immediately.
        """
        with ExitStack() as stack:
            descriptor = ipc.prepare_session(stack, resident_fixture(), 1, corpus.admit_memory)
            code = (
                "import sys; from tools.neutron_corpus_ipc import exclusive_writer; "
                "exclusive_writer(sys.argv[1]).__enter__()"
            )
            with (
                ipc.exclusive_writer(descriptor["mutex"]),
                self.assertRaisesRegex(ValueError, "Concurrent corpus workers"),
            ):
                corpus.run_process([sys.executable, "-B", "-c", code, descriptor["mutex"]], None, 10)

    def test_abandoned_writer_invalidates_study(self) -> None:
        """Purpose: Reject interrupted ownership. Inputs: Worker exits while holding the mutex. Outputs: No
        reusable study.
        """
        with ExitStack() as stack:
            descriptor = ipc.prepare_session(stack, resident_fixture(), 1, corpus.admit_memory)
            code = (
                "import os,sys; from tools.neutron_corpus_ipc import exclusive_writer; "
                "owner=exclusive_writer(sys.argv[1]); owner.__enter__(); os._exit(7)"
            )
            with self.assertRaises(ValueError):
                corpus.run_process([sys.executable, "-B", "-c", code, descriptor["mutex"]], None, 10)
            with self.assertRaisesRegex(ValueError, "abandoned"), ipc.exclusive_writer(descriptor["mutex"]):
                self.fail("abandoned study admitted")

    def test_changed_source_rejected_before_native(self) -> None:
        """Purpose: Bind transported bytes. Inputs: Changed mapped source. Outputs: Rejection before the native
        consumer.
        """
        with ExitStack() as stack:
            descriptor = ipc.prepare_session(stack, resident_fixture(), 1, corpus.admit_memory)
            mapping = SharedMemory(name=descriptor["source"], create=False)
            stack.callback(mapping.close)
            mapping.buf[descriptor["manifest_bytes"]] ^= 1
            with (
                mock.patch.object(corpus, "run_process", side_effect=AssertionError("changed bytes reached native")),
                self.assertRaisesRegex(ValueError, "identity changed"),
            ):
                corpus.worker(descriptor, corpus.ROOT / "test-only-native-fixture", 256, 10)

    def test_stale_native_receipt_refuses_before_acquisition(self) -> None:
        """Purpose: Avoid wasted corpus acquisition. Inputs: Stale native identity. Outputs: No download or child
        launch.
        """
        args = SimpleNamespace(corpus="Canterbury", configuration="Release")
        with (
            mock.patch.object(corpus, "validate_current", side_effect=ValueError("stale native fixture")),
            mock.patch.object(corpus, "download") as download,
            mock.patch.object(corpus, "run_process") as launch,
            self.assertRaisesRegex(ValueError, "stale native fixture"),
        ):
            corpus.study(args)
        download.assert_not_called()
        launch.assert_not_called()

    def test_hyperfine_provisioning_evidence_and_mutation(self) -> None:
        """Purpose: Admit pins before probes. Inputs: Bootstrap receipts/mutation. Outputs: Evidence or refusal."""
        receipt = {"tool": "Hyperfine", "version": "1.20.0", "binary_sha256": "a" * 64, "archive_sha256": "b" * 64}
        with tempfile.TemporaryDirectory() as temporary:
            bootstrap = Path(temporary) / "bootstrap.ps1"
            bootstrap.write_bytes(b"reviewed bootstrap fixture")
            digest = hashlib.sha256(bootstrap.read_bytes()).hexdigest()
            with (
                mock.patch.object(corpus, "COMPARISON_BOOTSTRAP", bootstrap),
                mock.patch.object(corpus, "run_process", return_value=json.dumps(receipt).encode()) as launch,
            ):
                self.assertEqual(corpus.provision_hyperfine(), {**receipt, "bootstrap_sha256": digest})
                self.assertEqual(launch.call_args.args[0][-2:], ["-Tool", "Hyperfine"])
                self.assertNotIn("--version", launch.call_args.args[0])
            invalid = [
                [],
                {**receipt, "extra": True},
                {**receipt, "version": "different"},
                {**receipt, "binary_sha256": "a"},
            ]
            for candidate in invalid:
                with (
                    self.subTest(candidate=candidate),
                    mock.patch.object(corpus, "COMPARISON_BOOTSTRAP", bootstrap),
                    mock.patch.object(corpus, "run_process", return_value=json.dumps(candidate).encode()),
                    self.assertRaises(ValueError),
                ):
                    corpus.provision_hyperfine()

            # Purpose: Model a concurrent bootstrap edit. Inputs: Owned fixture. Outputs: Valid-looking stale receipt.
            def change(*args, **kwargs):
                bootstrap.write_bytes(b"changed bootstrap fixture")
                return json.dumps(receipt).encode()

            with (
                mock.patch.object(corpus, "COMPARISON_BOOTSTRAP", bootstrap),
                mock.patch.object(corpus, "run_process", side_effect=change),
                self.assertRaisesRegex(ValueError, "source changed"),
            ):
                corpus.provision_hyperfine()

    def test_changed_hyperfine_refuses_before_probe_or_acquisition(self) -> None:
        """Purpose: Enforce leased pins. Inputs: Changed tool/source. Outputs: Refusal before probe or acquisition."""
        args = SimpleNamespace(corpus="Canterbury", configuration="Release")
        with tempfile.TemporaryDirectory() as temporary:
            binary, bootstrap = Path(temporary) / "fixture.exe", Path(temporary) / "bootstrap.ps1"
            binary.write_bytes(b"reviewed executable fixture")
            bootstrap.write_bytes(b"reviewed bootstrap fixture")
            evidence = {
                "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "bootstrap_sha256": hashlib.sha256(bootstrap.read_bytes()).hexdigest(),
            }
            for key in evidence:
                with (
                    self.subTest(changed=key),
                    mock.patch.object(corpus, "HYPERFINE", binary),
                    mock.patch.object(corpus, "COMPARISON_BOOTSTRAP", bootstrap),
                    mock.patch.object(corpus, "provision_hyperfine", return_value={**evidence, key: "c" * 64}),
                    mock.patch.object(corpus, "validate_current", return_value={"fixture": "unchanged"}),
                    mock.patch.object(corpus, "hold_build"),
                    mock.patch.object(corpus, "download") as download,
                    mock.patch.object(corpus, "run_process") as launch,
                    self.assertRaisesRegex(ValueError, "identity changed"),
                ):
                    corpus.study(args)
                download.assert_not_called()
                launch.assert_not_called()

    def test_partial_reports_remain_visible_and_unqualified(self) -> None:
        """Purpose: Preserve failed-study evidence. Inputs: One completed slot. Outputs: Original protocol and
        partial totals.
        """
        files = resident_fixture()
        with ExitStack() as stack:
            descriptor = ipc.prepare_session(stack, files, 1, corpus.admit_memory)
            _, output, _ = ipc.open_session(stack, descriptor)
            protocol = protocol_fixture(bytes(range(256)))
            ipc.publish_observation(output, 0, protocol)
            printed = io.StringIO()
            with mock.patch.object(corpus.sys, "stdout", printed):
                count, totals = corpus.emit_observations(output, descriptor, files)
            row = json.loads(printed.getvalue())
            self.assertEqual(row["protocol"], protocol.decode("ascii"))
            self.assertFalse(row["study_qualified"])
            self.assertEqual((count, totals[0]["input_bytes"], totals[0]["archive_bytes"]), (1, 256, 365))

    # Purpose: Retain successful timing-tool diagnostics without converting them into a timing qualification.
    # Inputs: A complete mocked study over tiny RAM fixtures and arbitrary diagnostic bytes; no GPU/network work.
    # Outputs: All observations qualify for size, exact stderr survives, and timing remains explicitly unqualified.
    def test_complete_study_retains_diagnostics(self) -> None:
        files, diagnostic = resident_fixture(), bytes(range(256))
        source_bytes = [member["data"] for member in files]
        executable = mock.Mock()
        executable.open.return_value = io.BytesIO(b"reviewed executable fixture")
        identity = {"inputs_sha256": "a" * 64, "receipt_sha256": "b" * 64}
        provisioning = {
            "binary_sha256": hashlib.sha256(b"reviewed executable fixture").hexdigest(),
            "bootstrap_sha256": hashlib.sha256(corpus.COMPARISON_BOOTSTRAP.read_bytes()).hexdigest(),
        }
        args = SimpleNamespace(
            corpus="Canterbury",
            configuration="Release",
            runs=1,
            block_size_kib=256,
            file_timeout=10,
            suite_timeout=10,
        )

        # Purpose: Fill the real RAM observation transport. Inputs: Controller launch role. Outputs: Fixture telemetry.
        def launch(arguments, data, timeout, **kwargs):
            if "--version" in arguments:
                self.assertNotIn("successful_stderr", kwargs)
                return b"hyperfine 1.20.0\n"
            kwargs["successful_stderr"].append(diagnostic)
            descriptor = json.loads(kwargs["env"][ipc.SESSION_ENV])
            with ExitStack() as stack:
                _, output, _ = ipc.open_session(stack, descriptor)
                for index, data in enumerate(source_bytes):
                    ipc.publish_observation(output, index, protocol_fixture(data))
            return b"successful timing fixture"

        printed = io.StringIO()
        with (
            mock.patch.object(corpus, "require_permission"),
            mock.patch.object(corpus, "validate_current", return_value=identity),
            mock.patch.object(corpus, "provision_hyperfine", return_value=provisioning),
            mock.patch.object(corpus, "hold_build"),
            mock.patch.object(corpus, "HYPERFINE", executable),
            mock.patch.object(corpus, "admit_memory"),
            mock.patch.object(corpus, "download", return_value=b"archive fixture"),
            mock.patch.object(corpus, "decode_canterbury", return_value=(files, {})),
            mock.patch.object(corpus, "run_process", side_effect=launch),
            mock.patch.object(corpus.sys, "stdout", printed),
        ):
            corpus.study(args)
        summary = json.loads(printed.getvalue().splitlines()[-1])
        self.assertTrue(summary["study_qualified"])
        self.assertFalse(summary["timing_qualified"])
        self.assertEqual(summary["observation_count"], len(files))
        self.assertEqual(summary["hyperfine_provisioning"], provisioning)
        self.assertEqual(base64.b64decode(summary["hyperfine_stderr_base64"]), diagnostic)
        self.assertEqual(summary["hyperfine_stderr_sha256"], hashlib.sha256(diagnostic).hexdigest())

    def test_child_descendant_cannot_outlive_root(self) -> None:
        """Purpose: Guard cleanup after root exit. Inputs: A root spawning a sleeping child. Outputs: Terminated
        exit status.
        """
        code = (
            "import subprocess,sys; p=subprocess.Popen([sys.executable,'-B','-c','import time; time.sleep(30)']); "
            "print(p.pid,flush=True)"
        )
        child_id = int(corpus.run_process([sys.executable, "-B", "-c", code], None, 10))
        library = ctypes.WinDLL("kernel32", use_last_error=True)
        library.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
        library.OpenProcess.restype = ctypes.c_void_p
        handle = library.OpenProcess(0x00100000, False, child_id)
        if handle:
            try:
                # Job accounting reaches zero after termination; final kernel-handle signalling can follow shortly.
                self.assertEqual(ipc.mutex_api().WaitForSingleObject(handle, 5000), 0)
            finally:
                ipc.mutex_api().CloseHandle(handle)


if __name__ == "__main__":
    unittest.main()
