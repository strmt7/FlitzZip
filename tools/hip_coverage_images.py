"""Verify linked HIP bundles and export complete native LLVM device coverage."""

from __future__ import annotations

import json
from pathlib import Path

from tools import hip_coverage as coverage
from tools.hip_coverage_product import require_identities, unique_input
from tools.hip_coverage_toolchain import report_object
from tools.native_coverage import run_checked


# Purpose: Verify every complete compiled device bundle survives the final Windows DLL link unchanged.
# Inputs: Admitted object manifest, current module and fresh owned directory. Outputs: Verified native-extracted ELFs.
def linked_images(record: dict, module: Path, output: Path) -> dict[str, Path]:
    output.mkdir()
    llvm = unique_input([Path(name) for name in record["inputs"]], "clang++.exe").parent
    section = output / "linked.hipfb"
    run_checked(
        [
            str(llvm / "llvm-objcopy.exe"),
            f"--dump-section=.hip_fat={section}",
            str(module),
            str(output / "module-copy.dll"),
        ],
        30,
        output / "extract-module.json",
    )
    if not 0 < section.stat().st_size <= 64 * 1024**2:
        raise ValueError("Linked HIP section exceeds its supported analysis extent")
    data = section.read_bytes()
    ranges, images = [], {}
    for source, obj in record["objects"].items():
        unit = output / Path(source).stem
        unit.mkdir()
        elf = coverage.device_image(llvm, Path(obj), record["architecture"], unit, coff_object=True)
        original = Path(obj).parent / "device.elf"
        require_identities({str(original): record["artifacts"][str(original)]})
        if elf.read_bytes() != original.read_bytes():
            raise ValueError("Product object differs from its admitted device ELF")
        bundle = (unit / "image.hipfb").read_bytes()
        if not bundle or data.count(bundle) != 1:
            raise ValueError("Linked module lacks exactly one copy of an admitted HIP bundle")
        begin = data.index(bundle)
        ranges.append((begin, begin + len(bundle)))
        images[source] = elf
    cursor = 0
    for begin, end in sorted(ranges):
        if begin < cursor or any(data[cursor:begin]):
            raise ValueError("Linked HIP section contains unreviewed or overlapping bundle bytes")
        cursor = end
    if any(data[cursor:]):
        raise ValueError("Linked HIP section contains unreviewed trailing bytes")
    return images


# Purpose: Validate native per-file source coverage without counting SDK files stored inside the checkout's cache.
# Inputs: Complete LLVM export object. Outputs: Unique repository src summaries with consistent integer counts.
def project_summary(report: dict) -> list[dict]:
    if report.get("type") != "llvm.coverage.json.export" or report.get("version") != "3.1.0":
        raise ValueError("Unsupported device coverage export schema")
    data = report.get("data")
    if (
        not isinstance(data, list)
        or len(data) != 1
        or not isinstance(data[0], dict)
        or not isinstance(data[0].get("files"), list)
    ):
        raise ValueError("Device coverage must contain one complete combined-image export")
    files, seen = [], set()
    for item in data[0]["files"]:
        if not isinstance(item, dict) or not isinstance(item.get("filename"), str):
            raise ValueError("Device coverage contains malformed source metadata")
        path = Path(item["filename"]).resolve()
        try:
            relative = path.relative_to(coverage.ROOT / "src")
        except ValueError:
            continue
        summary = item.get("summary")
        lines = summary.get("lines") if isinstance(summary, dict) else None
        if not isinstance(lines, dict):
            raise ValueError("Device coverage source lacks a line summary")
        count, covered = lines.get("count"), lines.get("covered")
        if type(count) is not int or type(covered) is not int or not 0 <= covered <= count or path in seen:
            raise ValueError("Device coverage contains inconsistent or duplicate source counts")
        seen.add(path)
        files.append(
            {"source": "src/" + relative.as_posix(), "lines": count, "covered": covered, "missed": count - covered}
        )
    if not files:
        raise ValueError("Device export contains no repository product source")
    return sorted(files, key=lambda row: (-row["missed"], row["source"]))


# Purpose: Preserve complete native metadata and refuse any mapping or collection diagnostic.
# Inputs: Admitted native command and fresh log destination. Outputs: Strict JSON and its retained native bytes.
def native_report(command: list[str], log: Path) -> tuple[dict, Path]:
    with log.with_name(log.stem + "-command.json").open("x", encoding="utf-8") as stream:
        json.dump(command, stream, indent=2)
    result = run_checked(command, 120, log)
    if result["output_truncated"] or result["stderr"]:
        raise ValueError("Native device export is incomplete or reported a mapping diagnostic")
    native = log.with_name(log.stem + "-native.json")
    native.write_text(result["stdout"], encoding="utf-8")
    return report_object(result["stdout"]), native


# Purpose: Prevent independent translation units' equal function names from corrupting one another's CFG mappings.
# Inputs: Native LLVM reader, admitted image paths and unique raw profiles. Outputs: Exact per-image associations.
def associate_profiles(tool: Path, images: dict[str, Path], profiles: list[Path], output: Path) -> dict:
    objects = {path.resolve(): source for source, path in images.items()}
    if not objects or len(objects) != len(images) or len(objects) > 64 or not 0 < len(profiles) <= 325:
        raise ValueError("Device profile association requires bounded unique images and profiles")
    if len({path.resolve() for path in profiles}) != len(profiles):
        raise ValueError("Device profiles must not be repeated")
    groups = {source: [] for source in images}
    candidates = [arg for path in objects for arg in ("--match-device", str(path))]
    for index, profile in enumerate(profiles):
        report, _ = native_report(
            [str(tool), "--profile", str(profile), *candidates], output / f"association-{index:03}.json"
        )
        matched, count = report.get("matched_device"), report.get("records")
        if (
            not isinstance(matched, str)
            or not Path(matched).is_absolute()
            or Path(matched).resolve() not in objects
            or type(count) is not int
            or not 0 < count <= 100000
            or report.get("scope") != "Native profile/ELF inventory association; no execution"
        ):
            raise ValueError("Native profile lacks an exact admitted device association")
        groups[objects[Path(matched).resolve()]].append(profile)
    if any(not values for values in groups.values()):
        raise ValueError("Every admitted device image requires actual profiles")
    return groups


# Purpose: Combine native mapped-line hits without double-counting shared headers or summing incompatible CFGs.
# Inputs: Complete per-image LLVM line reports. Outputs: Boolean union by exact source path and line number.
def project_line_summary(reports: list[dict]) -> list[dict]:
    sources = {}
    for report in reports:
        if report.get("scope") != "LLVM per-image mapped line hits; no execution" or not isinstance(
            report.get("files"), list
        ):
            raise ValueError("Unsupported native mapped-line report")
        seen = set()
        for item in report["files"]:
            if not isinstance(item, dict) or not isinstance(item.get("filename"), str):
                raise ValueError("Native line report lacks a source filename")
            path = Path(item["filename"])
            if not path.is_absolute() or path.resolve() in seen or not isinstance(item.get("lines"), list):
                raise ValueError("Native line report has ambiguous source metadata")
            path = path.resolve()
            seen.add(path)
            lines = {}
            for pair in item["lines"]:
                if (
                    not isinstance(pair, list)
                    or len(pair) != 2
                    or type(pair[0]) is not int
                    or not 0 < pair[0] < 2**32
                    or type(pair[1]) is not bool
                    or pair[0] in lines
                ):
                    raise ValueError("Native line report has invalid or duplicate mapped hits")
                lines[pair[0]] = pair[1]
            if not path.is_relative_to(coverage.ROOT / "src"):
                continue
            combined = sources.setdefault(path, {})
            for number, hit in lines.items():
                combined[number] = combined.get(number, False) or hit
    files = []
    for path, lines in sources.items():
        count, covered = len(lines), sum(lines.values())
        files.append(
            {
                "source": path.relative_to(coverage.ROOT).as_posix(),
                "lines": count,
                "covered": covered,
                "missed": count - covered,
            }
        )
    if not files:
        raise ValueError("Device export contains no repository product source")
    return sorted(files, key=lambda row: (-row["missed"], row["source"]))


# Purpose: Merge only same-image profiles and preserve every complete native export before combining line hits.
# Inputs: Qualified LLVM and reader tools, preserved ELFs, raw profiles and fresh owned directory.
# Outputs: Native per-image reports and source-scoped line union; any association or mapping diagnostic fails.
def export_images(llvm: Path, tool: Path, images: dict[str, Path], profiles: list[Path], output: Path) -> dict:
    output.mkdir()
    inputs = coverage.identities([tool, llvm / "llvm-profdata.exe", llvm / "llvm-cov.exe", *images.values(), *profiles])
    groups = associate_profiles(tool, images, profiles, output)
    reports, artifacts = [], []
    for index, (source, elf) in enumerate(images.items()):
        unit = output / f"image-{index:02}"
        unit.mkdir()
        merged = unit / "device.profdata"
        command = [str(llvm / "llvm-profdata.exe"), "merge", "-sparse", "-o", str(merged), *map(str, groups[source])]
        (unit / "merge-command.json").write_text(json.dumps(command, indent=2), encoding="utf-8")
        result = run_checked(command, 60, unit / "merge.json")
        if result["output_truncated"] or result["stderr"]:
            raise ValueError("Native device profile merge reported a diagnostic")
        exported, native = native_report(
            [str(llvm / "llvm-cov.exe"), "export", str(elf), "-instr-profile=" + str(merged)],
            unit / "export-process.json",
        )
        # Validate native schema/counts even though shared-source totals use the exact mapped-line union below.
        project_summary(exported)
        report, lines = native_report(
            [str(tool), "--device", str(elf), "--coverage-profile", str(merged)], unit / "line-process.json"
        )
        reports.append(report)
        artifacts.extend([merged, native, lines])
    files = project_line_summary(reports)
    require_identities(inputs)
    return {
        "files": files,
        "lines": sum(row["lines"] for row in files),
        "covered": sum(row["covered"] for row in files),
        "artifacts": coverage.identities(
            [*artifacts, *sorted(output.glob("association-*.json")), *sorted(output.glob("image-*/*-command.json"))]
        ),
    }
