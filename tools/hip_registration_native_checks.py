"""Exercise real native registration rejection against the helper-inclusive qualification probe."""

from __future__ import annotations

import argparse
import json
import re
import struct
from pathlib import Path

from tools import hip_coverage as coverage
from tools.hip_coverage_registration import (
    SOURCE_DIRECTORY,
    admit_tool,
    native_metadata,
    report_object,
    validate_completion,
)
from tools.hip_coverage_toolchain import LOCK
from tools.native_coverage import owned_path


# Purpose: Bind completion and idempotence claims to every independently read artifact inventory.
# Inputs: Original inventory, two validated completion reports and the persisted second output inventory.
# Outputs: Refusal if a report describes another artifact, drops metadata or changes an already complete module.
def validate_inventory_chain(original: dict, completed: dict, repeated: dict, persisted: dict) -> None:
    if (
        completed["before"] != original
        or completed["after"] != repeated["before"]
        or repeated["before"] != repeated["after"]
        or repeated["after"] != persisted
        or repeated["completed_records"]
        or repeated["retired_profile_groups"]
    ):
        raise ValueError("Native registration inventory evidence is disconnected or not idempotent")


class NativeChecks:
    # Purpose: Own fresh, bounded native probe evidence. Inputs: Explicit same-compilation tools/files. Outputs: Owner.
    def __init__(self, tool: Path, compiler: Path, host: Path, device: Path, output: Path):
        self.tool, self.compiler, self.host, self.device = map(coverage.existing_input, (tool, compiler, host, device))
        self.output = owned_path(coverage.ROOT, output)
        self.output.mkdir(parents=True, exist_ok=False)
        self.text = ""
        self.outcomes = []
        self.positive_outcomes = []

    # Purpose: Execute static analysis only. Inputs: Explicit paths and optional output. Outputs: Parsed native report.
    def inspect(self, host: Path, name: str, destination: Path | None = None, *, image: Path | None = None) -> dict:
        command = [str(self.tool), "--host", str(host), "--device", str(self.device if image is None else image)]
        if destination is not None:
            command.extend(["--complete", "-o", str(destination)])
        return native_metadata(command, self.output / (name + ".json"), 30)

    # Purpose: Require normal refusal before output publication. Inputs: One malformed owned fixture. Outputs: Evidence.
    def rejected(self, name: str, diagnostic: str, *, ir: str | None = None, image: Path | None = None) -> None:
        case = self.output / name
        case.mkdir()
        host = self.host
        if ir is not None:
            host = case / "host.ll"
            host.write_text(ir, encoding="utf-8")
        target = case / "must-not-exist.bc"
        command = [
            str(self.tool),
            "--host",
            str(host),
            "--device",
            str(image or self.device),
            "--complete",
            "-o",
            str(target),
        ]
        result = coverage.process_owner().run_bounded_command(command, timeout_seconds=30, max_output_bytes=65536)
        (case / "process.json").write_text(json.dumps({"command": command, **result}, indent=2), encoding="utf-8")
        if (
            result["exit_code"] != 1
            or result["timed_out"]
            or result["output_limit_exceeded"]
            or result["output_truncated"]
            or diagnostic not in result["stderr"]
            or target.exists()
        ):
            raise ValueError(f"Native registration rejection contract failed: {name}")
        self.outcomes.append(name)

    # Purpose: Change one profile call in a negative fixture. Inputs: Local replacement. Outputs: IR text.
    def change_call(self, transform, *, table: bool = False) -> str:
        prefix = "__llvm_profile_sections_" if table else "__llvm_profile_shadow_data_"
        pattern = r"^  call void @__hipRegisterVar\(ptr %0, ptr @" + prefix + r"[^\n]+$"
        matches = re.findall(pattern, self.text, flags=re.MULTILINE)
        if len(matches) != 1:
            raise ValueError("Native rejection controls require the single-kernel qualification probe")
        return self.text.replace(matches[0], transform(matches[0]), 1)

    # Purpose: Prove success, closure, idempotence and output preservation using the actual native tool.
    # Inputs: Unmodified probe artifacts. Outputs: Verified completed bitcode and retained existing-output refusal.
    def valid_controls(self) -> None:
        original = self.inspect(self.host, "original")
        if original["device_data_records"] != 2 or original["registered_data_records"] != 1:
            raise ValueError("Native rejection controls require the original helper-inclusive probe")
        self.digest_controls(original)
        completed = self.output / "completed.bc"
        report = self.inspect(self.host, "complete", completed)
        validate_completion(report)
        second = self.output / "completed-again.bc"
        repeated = self.inspect(completed, "idempotence", second)
        validate_completion(repeated)
        persisted = self.inspect(second, "idempotence-persisted")
        validate_inventory_chain(original, report, repeated, persisted)
        before = coverage.digest(completed)
        result = coverage.process_owner().run_bounded_command(
            [
                str(self.tool),
                "--host",
                str(self.host),
                "--device",
                str(self.device),
                "--complete",
                "-o",
                str(completed),
            ],
            timeout_seconds=30,
        )
        (self.output / "existing-output.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
        if (
            result["exit_code"] != 1
            or result["timed_out"]
            or result["output_limit_exceeded"]
            or result["output_truncated"]
            or coverage.digest(completed) != before
        ):
            raise ValueError("Native existing-output refusal lacks complete evidence or changed the output")

    # Purpose: Prove the IR digest ignores only artifact identity and detects changed program storage.
    # Inputs: Unmodified inventory and exact host bytes. Outputs: Independent same-content and changed-content controls.
    def digest_controls(self, original: dict) -> None:
        relocated = self.output / "identical-content-different-path.bc"
        relocated.write_bytes(self.host.read_bytes())
        same = self.inspect(relocated, "digest-relocated")
        if original["module_ir_sha256"] != same["module_ir_sha256"]:
            raise ValueError("IR digest depends on its artifact path")
        changed = self.output / "changed-program-storage.ll"
        changed.write_text(
            self.text + "\n@__flitzzip_profile_digest_control = internal global i32 17\n", encoding="utf-8"
        )
        baseline = self.output / "digest-original.ll"
        baseline.write_text(self.text, encoding="utf-8")
        before = self.inspect(baseline, "digest-text-original")
        after = self.inspect(changed, "digest-text-changed")
        if before["module_ir_sha256"] == after["module_ir_sha256"]:
            raise ValueError("IR digest failed to detect changed program storage")
        self.positive_outcomes.append("complete-ir-digest-content-and-path-controls")

    # Purpose: Reject cross-TU shadow coalescing for helpers shared by independent device images.
    # Inputs: Metadata-only alternate CUID in static IR/ELF fixtures. Outputs: Disjoint host shadow names; no execution.
    def namespace_controls(self) -> None:
        match = re.search(r"__llvm_profile_sections_([a-f0-9]{1,16})\b", self.text)
        if not match:
            raise ValueError("Namespace control lacks its translation-unit identity")
        cuid = match[1]
        replacement = ("f" if cuid[0] != "f" else "e") + cuid[1:]
        host_text, image_bytes = self.text, self.device.read_bytes()
        for prefix in ("__llvm_profile_sections_", "__llvm_prf_nm_"):
            host_text = host_text.replace(prefix + cuid, prefix + replacement)
            image_bytes = image_bytes.replace((prefix + cuid).encode() + b"\0", (prefix + replacement).encode() + b"\0")
        host = self.output / "other-translation-unit.ll"
        host.write_text(host_text, encoding="utf-8")
        image = self.output / "other-translation-unit.elf"
        image.write_bytes(image_bytes)
        completed = self.output / "other-translation-unit.bc"
        report = self.inspect(host, "other-translation-unit", completed, image=image)
        validate_completion(report)
        shadows = []
        for index, artifact in enumerate((self.output / "completed.bc", completed)):
            readable = self.output / f"namespace-{index}.ll"
            coverage.checked(
                [str(self.compiler), "-x", "ir", "-S", "-emit-llvm", "-O0", str(artifact), "-o", str(readable)],
                self.output / f"namespace-{index}.json",
            )
            shadows.append(
                set(re.findall(r"^@(__flitzzip_hip_profile_shadow_[a-f0-9]{64}) =", readable.read_text(), re.M))
            )
        if len(shadows[0]) != 3 or len(shadows[1]) != 3 or shadows[0] & shadows[1]:
            raise ValueError("Shared-helper profiling shadows collide across translation units")
        self.positive_outcomes.append("shared-helper-shadow-namespace-is-translation-unit-specific")

    # Purpose: Reject wrong HIP flags, context, extent and pointer ownership before any output is written.
    # Inputs: The actual generated host IR. Outputs: Independent single-boundary refusal receipts.
    def host_abi_controls(self) -> None:
        for index in (4, 6, 7):
            # Purpose: Alter one fixture flag. Inputs: Call text/index. Outputs: Valid IR with unsupported ABI flags.
            def flag(line, index=index):
                arguments = line.split("(")[1].rstrip(")").split(", ")
                arguments[index] = "i32 1"
                return line.split("(")[0] + "(" + ", ".join(arguments) + ")"

            self.rejected(
                f"flag-{index}", "Unexpected profiling variable registration flags", ir=self.change_call(flag)
            )
        for table in (False, True):
            suffix = "table" if table else "data"
            self.rejected(
                "context-" + suffix,
                "Unsupported profiling variable registration ABI",
                ir=self.change_call(lambda line: line.replace("(ptr %0,", "(ptr null,", 1), table=table),
            )
            self.rejected(
                "size-" + suffix,
                "Unexpected profiling shadow extent",
                ir=self.change_call(lambda line: line.replace("i64 8,", "i64 16,", 1), table=table),
            )
        shadow = re.search(r"^@__llvm_profile_shadow_data_[^\n]+ = global ptr null$", self.text, re.MULTILINE)
        if not shadow:
            raise ValueError("Native control lacks its expected pointer shadow")
        for name, replacement in (
            ("constant", shadow[0].replace("global ptr", "constant ptr")),
            ("nonzero", shadow[0].replace("ptr null", "ptr inttoptr (i64 1 to ptr)")),
            ("thread-local", shadow[0].replace("= global", "= thread_local global")),
        ):
            self.rejected(
                name,
                "Unsupported profiling shadow storage ABI",
                ir=self.text[: shadow.start()] + replacement + self.text[shadow.end() :],
            )

    # Purpose: Prove absent profiling globals can be reconciled without changing program code or inventing counters.
    # Inputs: Same-length metadata-only name mutation. Outputs: Exact closure, idempotence and bad-ABI refusal.
    def absent_metadata_controls(self) -> None:
        data = re.search(r'c"__profd_([^"\n]+)\\00"', self.text)
        if not data:
            raise ValueError("Native control lacks its profile data identity")
        stem = data[1]
        replacement = stem[:-1] + ("j" if stem[-1] != "j" else "k")
        phantom = self.text
        for prefix in ("__profd_", "__profc_", "__llvm_prf_unifcnt_"):
            pattern = (
                r'(?m)^(@[A-Za-z0-9_.$]+ = private(?: unnamed_addr)? constant \[\d+ x i8\] c")'
                + re.escape(prefix + stem)
                + r'(\\00"[^\n]*)$'
            )
            matched = re.search(pattern, phantom)
            if not matched:
                raise ValueError("Native control lacks a complete immutable profile name tuple")
            start = matched.start() + len(matched[1])
            phantom = phantom[:start] + prefix + replacement + phantom[start + len(prefix + stem) :]
        host = self.output / "absent-profile-group.ll"
        host.write_text(phantom, encoding="utf-8")
        original = self.inspect(host, "absent-original")
        completed = self.output / "absent-completed.bc"
        report = self.inspect(host, "absent-completion", completed)
        validate_completion(report)
        if (
            report["before"]["registered_data_records"] != 0
            or report["before"]["absent_host_profile_groups"] != 1
            or report["completed_records"] != 2
            or report["retired_profile_groups"] != 1
        ):
            raise ValueError("Native repair did not reconcile the independent absent-global control")
        again = self.output / "absent-completed-again.bc"
        repeated = self.inspect(completed, "absent-idempotence", again)
        validate_completion(repeated)
        persisted = self.inspect(again, "absent-idempotence-persisted")
        validate_inventory_chain(original, report, repeated, persisted)
        self.positive_outcomes.append("absent-profile-only-registrations-reconciled")
        call = re.search(
            r"^  call void @__hipRegisterVar\(ptr %0, ptr @__llvm_profile_shadow_data_[^\n]+$", phantom, re.MULTILINE
        )
        if not call:
            raise ValueError("Native control lacks its original data registration")
        invalid = phantom.replace(call[0], call[0].replace("i64 8,", "i64 16,", 1), 1)
        self.rejected("absent-profile-bad-extent", "Unexpected profiling shadow extent", ir=invalid)

    # Purpose: Reject partial, duplicate, short and mismatched compiler identities without unsafe substring handling.
    # Inputs: Single-kernel generated IR. Outputs: Refusal evidence for each independently malformed group.
    def host_group_controls(self) -> None:
        lines = re.findall(
            r"^  call void @__llvm_profile_offload_register_section_shadow_variable\([^\n]+$", self.text, re.MULTILINE
        )
        if len(lines) != 4:
            raise ValueError("Native control must contain one complete four-section registration group")
        self.rejected(
            "non-C-hook",
            "Unexpected profile section-registration ABI",
            ir=self.text.replace(lines[0], lines[0].replace("call void", "call fastcc void"), 1),
        )
        self.rejected("partial-group", "Incomplete profile shadow group", ir=self.text.replace(lines[-1] + "\n", "", 1))
        self.rejected(
            "duplicate-data",
            "Repeated non-name profile section registration",
            ir=self.text.replace(lines[-1], lines[0], 1),
        )
        table = re.search(r"__llvm_profile_sections_([a-f0-9]{1,16})\b", self.text)
        if not table:
            raise ValueError("Native control lacks its expected table identity")
        altered = ("f" if table[1][0] != "f" else "e") + table[1][1:]
        self.rejected(
            "mismatched-cuid",
            "Host and device profiling translation-unit identities differ",
            ir=self.text.replace(table[0], "__llvm_profile_sections_" + altered),
        )
        data = re.search(r'(@\d+ = private unnamed_addr constant )\[\d+ x i8\] c"__profd_[^"\n]+\\00"', self.text)
        if not data:
            raise ValueError("Native control lacks its immutable profile data name")
        short = data[1] + '[9 x i8] c"__profd_\\00"'
        self.rejected(
            "short-data-prefix",
            "Host profile group does not match the actual ELF inventory",
            ir=self.text[: data.start()] + short + self.text[data.end() :],
        )
        self.rejected("malformed-ir", "Host IR parse failed:", ir="this is not LLVM IR\n")

    # Purpose: Reject truncated/foreign images and incomplete actual ELF symbol inventories.
    # Inputs: Tiny owned corruptions and native objcopy stripping. Outputs: Static refusal; no generated code executes.
    def device_controls(self) -> None:
        for name, data in (
            ("empty", b""),
            ("truncated", self.device.read_bytes()[:32]),
            ("non-elf", b"MZ" + bytes(100)),
        ):
            image = self.output / (name + ".bin")
            image.write_bytes(data)
            self.rejected(name, "profile registration analysis:", image=image)
        for prefix in ("__profc_", "__profd_"):
            image = self.output / ("stripped-" + prefix + ".elf")
            coverage.checked(
                [
                    str(self.compiler.parent / "llvm-objcopy.exe"),
                    "--strip-symbol=" + prefix + "_Z21coverage_helper_probei",
                    str(self.device),
                    str(image),
                ],
                self.output / ("strip-" + prefix + ".json"),
            )
            self.rejected("missing-" + prefix, "Profile symbols", image=image)

    # Purpose: Exercise bounded metadata traversal without exhausting the host or executing mutated device bytes.
    # Inputs: Small ELF and explicit section-table mutations. Outputs: Large names handled; excess sections refused.
    def metadata_budget_controls(self) -> None:
        original = self.device.read_bytes()
        if len(original) > 1024 * 1024 or original[:6] != b"\x7fELF\x02\x01":
            raise ValueError("Metadata budget controls require the small ELF64 qualification probe")
        table = struct.unpack_from("<Q", original, 40)[0]
        extent, count, strings = struct.unpack_from("<HHH", original, 58)
        if extent != 64 or not 1 < count < 1000 or not 0 < strings < count or table + count * extent > len(original):
            raise ValueError("Unsupported qualification probe section-table geometry")
        string_header = table + strings * extent
        string_offset, string_bytes = struct.unpack_from("<QQ", original, string_header + 24)
        names = original[string_offset : string_offset + string_bytes]
        text_index = next(
            (
                index
                for index in range(1, count)
                if names[struct.unpack_from("<I", original, table + index * extent)[0] :].split(b"\0", 1)[0] == b".text"
            ),
            None,
        )
        if text_index is None or string_offset + string_bytes > len(original):
            raise ValueError("Qualification probe lacks its ordinary text section")
        long_name = bytearray(original)
        extended_names = names + b"unrelated_" + b"x" * 65536 + b"\0"
        struct.pack_into("<QQ", long_name, string_header + 24, len(long_name), len(extended_names))
        struct.pack_into("<I", long_name, table + text_index * extent, len(names))
        long_name.extend(extended_names)
        image = self.output / "long-section-name.elf"
        image.write_bytes(long_name)
        result = coverage.checked(
            [str(self.tool), "--host", str(self.host), "--device", str(image)],
            self.output / "long-section-name.json",
            30,
        )
        report = report_object(result["stdout"])
        if report["device_data_records"] != 2 or report["registered_data_records"] != 1:
            raise ValueError("Unrelated long section names changed native profile admission")
        self.positive_outcomes.append("long-unrelated-section-name")
        excessive = bytearray(original)
        section_count = 100002
        new_table = bytearray(original[table : table + count * extent])
        new_table.extend(bytes((section_count - count) * extent))
        struct.pack_into("<Q", new_table, 32, section_count)
        struct.pack_into("<Q", excessive, 40, len(excessive))
        struct.pack_into("<H", excessive, 60, 0)
        excessive.extend(new_table)
        image = self.output / "excessive-sections.elf"
        image.write_bytes(excessive)
        self.rejected(
            "excessive-section-count", "Device ELF section inventory exceeds the analysis budget", image=image
        )

    # Purpose: Freeze inputs and run meaningful native parser/ABI controls through the bounded process owner.
    # Inputs: Explicit qualification artifacts. Outputs: Source-bound native-check receipt; failures retain raw reports.
    def run(self) -> dict:
        inputs = [
            self.tool,
            LOCK,
            LOCK.with_name("hip_coverage_toolchain.py"),
            self.compiler,
            self.host,
            self.device,
            Path(__file__).resolve(),
            self.compiler.parent / "llvm-objcopy.exe",
            SOURCE_DIRECTORY / "CMakeLists.txt",
            SOURCE_DIRECTORY / "hip_profile_registration.cpp",
        ]
        before = coverage.identities(inputs)
        admit_tool(self.tool, self.output)
        readable = self.output / "host.ll"
        coverage.checked(
            [str(self.compiler), "-x", "ir", "-S", "-emit-llvm", "-O0", str(self.host), "-o", str(readable)],
            self.output / "render.json",
        )
        self.text = readable.read_text(encoding="utf-8")
        self.valid_controls()
        self.namespace_controls()
        self.absent_metadata_controls()
        self.host_abi_controls()
        self.host_group_controls()
        self.device_controls()
        self.metadata_budget_controls()
        if coverage.identities(inputs) != before:
            raise ValueError("Native profiling control inputs changed during static checks")
        receipt = {
            "qualification": "passed",
            "scope": "Native static parser and registration controls; no GPU execution",
            "inputs": before,
            "negative_cases": self.outcomes,
            "positive_cases": self.positive_outcomes,
            "ir_idempotent": True,
            "existing_output_preserved": True,
        }
        (self.output / "qualification.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
        return receipt


# Purpose: Expose explicit fresh-artifact native checks. Inputs: Compiler, tool, same-TU files/output. Outputs: Summary.
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("tool", "compiler", "host", "device", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    result = NativeChecks(args.tool, args.compiler, args.host, args.device, args.output).run()
    print(json.dumps({"qualification": result["qualification"], "negative_cases": len(result["negative_cases"])}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
