"""Render reviewed SuperZip RAM benchmark records as a deterministic SVG chart."""

from __future__ import annotations

import argparse
import json
import math
import re
import statistics
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict
from pathlib import Path

try:
    from tools.native_build_receipt import validate_receipt
    from tools.render_svg_graph import SVG, new_document
except ModuleNotFoundError:
    from native_build_receipt import validate_receipt
    from render_svg_graph import SVG, new_document

MAX_CONFIRMATION_COUNT = 1024
MAX_EXACT_REQUESTED_COUNT = 2**53 - 1


# Purpose: Validate optional runtime provenance while preserving historical unknown identities.
# Inputs: A dotted four-component numeric DLL version or unavailable/missing evidence.
# Outputs: Returns canonical version text or None; raises ValueError on malformed/private metadata.
def runtime_version_identity(value: object) -> str | None:
    if value is None or value == "unavailable":
        return None
    if (
        not isinstance(value, str)
        or not re.fullmatch(r"(?:0|[1-9][0-9]{0,4})(?:\.(?:0|[1-9][0-9]{0,4})){3}", value)
        or any(int(component) > 65535 for component in value.split("."))
    ):
        raise ValueError("invalid HIP runtime version evidence")
    return value


# Purpose: Validate declared descriptive thresholds and fixed sample counts before inspecting measurements.
# Inputs: Policy is an untrusted schema-three sampling dictionary.
# Outputs: Returns counts by block; raises ValueError on malformed bounds, discarded samples or unfrozen counts.
def validate_sampling_plan(policy: object) -> dict:
    if not isinstance(policy, dict) or policy.get("method") not in ("fixed_count", "pilot_fixed_confirmation"):
        raise ValueError("missing or invalid sampling protocol")
    for field, lower, upper in (
        ("minimum_count", 1, MAX_CONFIRMATION_COUNT),
        ("maximum_count", 1, MAX_CONFIRMATION_COUNT),
        ("pilot_count", 0, 10),
    ):
        value = policy.get(field)
        if type(value) is not int or not lower <= value <= upper:
            raise ValueError("invalid sampling count bound")
    for field, lower, upper in (
        ("minimum_measured_seconds", 1, 600),
        ("target_relative_standard_error_pct", 0.1, 25),
        ("max_relative_std_dev_pct", 0.1, 50),
    ):
        value = policy.get(field)
        if type(value) not in (int, float) or not math.isfinite(value) or not lower <= value <= upper:
            raise ValueError("invalid sampling threshold")
    if (
        policy.get("discarded_sample_count") != 0
        or type(policy.get("discarded_sample_count")) is not int
        or policy.get("warmup_count") != 0
        or type(policy.get("warmup_count")) is not int
        or policy.get("inference") != "descriptive_only_no_confidence_or_significance_claim"
        or policy.get("block_order") != "reverse_on_even_iterations"
    ):
        raise ValueError("sampling protocol must retain observations and disclose descriptive inference")
    plans = policy.get("case_plans")
    if not isinstance(plans, list) or not plans:
        raise ValueError("missing fixed confirmation plans")
    plan_by_block = {}
    for plan in plans:
        if not isinstance(plan, dict):
            raise ValueError("invalid confirmation plan")
        block, count, requested = (plan.get(key) for key in ("block_size_kib", "confirmation_count", "requested_count"))
        if (
            type(block) is not int
            or block in plan_by_block
            or type(count) is not int
            or type(requested) is not int
            or requested > MAX_EXACT_REQUESTED_COUNT
            or not policy["minimum_count"] <= count <= policy["maximum_count"]
            or requested < count
            or count != min(requested, policy["maximum_count"])
            or type(plan.get("count_capped")) is not bool
            or plan["count_capped"] != (requested > count)
            or plan.get("stopping_rule") != "count_fixed_before_confirmation"
        ):
            raise ValueError("invalid fixed confirmation count")
        plan_by_block[block] = count
    return plan_by_block


# Purpose: Verify predeclared admission geometry against every pilot and confirmation observation.
# Inputs: The current record and its frozen per-lane, per-block planning policy.
# Outputs: Rejects absent, duplicate, unmatched or changed geometry without modifying any observations.
def validate_frozen_geometry(record: dict, policy: dict) -> None:
    if policy.get("geometry_policy") != "exact_depths_frozen_before_pilot_abort_on_admission_or_identity_change":
        raise ValueError("missing frozen geometry policy")
    plans = policy.get("geometry_plans")
    if not isinstance(plans, list) or not isinstance(record.get("pilot_runs"), list):
        raise ValueError("missing frozen geometry plans or pilot sample")
    fields = ("workers", "inflight_chunks", "codec_workers", "decode_inflight_chunks", "decode_codec_workers")
    by_case = {}
    for plan in plans:
        if not isinstance(plan, dict) or not isinstance(plan.get("case"), str) or plan["case"] in by_case:
            raise ValueError("invalid or duplicate frozen geometry plan")
        if any(type(plan.get(field)) is not int or not 1 <= plan[field] <= 64 for field in fields):
            raise ValueError("invalid frozen geometry bounds")
        by_case[plan["case"]] = plan
    cases = set()
    for run in record["runs"] + record["pilot_runs"]:
        case = f"{run.get('lane')}:{run.get('block_size_kib')}"
        cases.add(case)
        plan = by_case.get(case)
        if plan is None or any(run.get(field) != plan[field] for field in fields):
            raise ValueError("observation differs from frozen geometry")
    if cases != set(by_case):
        raise ValueError("frozen geometry plans do not cover exactly the observed cases")


# Purpose: Recompute scientific descriptive safeguards from all retained confirmation observations.
# Inputs: A schema-three record with separate pilot samples and already frozen confirmation plans.
# Outputs: Returns policy identity; refuses incomplete, noisy or incompatible raw evidence without trimming.
def validate_sampling_protocol(record: dict, allow_dirty: bool) -> tuple:
    policy = record.get("sampling_policy")
    plan_by_block = validate_sampling_plan(policy)
    pilot = record.get("pilot_runs")
    validate_frozen_geometry(record, policy)
    adaptive = policy["method"] == "pilot_fixed_confirmation"
    if (
        not isinstance(pilot, list)
        or (adaptive and policy["pilot_count"] < 3)
        or (not adaptive and (pilot or policy["pilot_count"]))
    ):
        raise ValueError("invalid separate pilot sample")
    if adaptive:
        validate_record(dict(record, schema_version=2, runs=pilot), allow_dirty)
    grouped = defaultdict(list)
    for run in record["runs"]:
        grouped[(run["block_size_kib"], run["lane"])].append(run)
    if set(plan_by_block) != {block for block, _ in grouped}:
        raise ValueError("confirmation plan does not cover the observed cases")
    for (block, lane), runs in grouped.items():
        count = plan_by_block[block]
        if len(runs) != count or {run["iteration"] for run in runs} != set(range(1, count + 1)) or count < 3:
            raise ValueError("confirmation sample is incomplete or insufficient")
        pilots = [run for run in pilot if run["block_size_kib"] == block and run["lane"] == lane]
        if adaptive and (
            len(pilots) != policy["pilot_count"]
            or {run["iteration"] for run in pilots} != set(range(1, policy["pilot_count"] + 1))
        ):
            raise ValueError("pilot sample is incomplete")
        if {run["lane"] for run in pilot} - {run["lane"] for run in record["runs"]} or {
            run["block_size_kib"] for run in pilot
        } - set(plan_by_block):
            raise ValueError("pilot contains unmatched cases")
        for field in (
            "input_bytes",
            "output_bytes",
            "archive_bytes",
            "workers",
            "inflight_chunks",
            "codec_workers",
            "decode_inflight_chunks",
            "decode_codec_workers",
        ):
            values = [run.get(field) for run in runs + pilots]
            if any(type(value) is not int or value < 1 for value in values) or len(set(values)) != 1:
                raise ValueError("inconsistent exact sizes or resource policy")
            if ("workers" in field or "chunks" in field) and values[0] > 64:
                raise ValueError("resource policy exceeds production bound")
        for run in runs + pilots:
            if any(run[field] <= 0 for field in ("compress_seconds", "verify_seconds", "extract_seconds")):
                raise ValueError("missing positive phase timing")
            if (
                type(run.get("resource_sample_count")) is not int
                or run["resource_sample_count"] < 1
                or (lane == "GPU" and (type(run.get("gpu_sample_count")) is not int or run["gpu_sample_count"] < 1))
            ):
                raise ValueError("resource counters unavailable")
            if lane == "GPU" and any(
                type(run.get(field)) is not int or run[field] < 1
                for field in ("gpu_h2d_bytes", "gpu_d2h_bytes", "gpu_device_allocation_bytes")
            ):
                raise ValueError("GPU lane lacks HIP transfer or allocation evidence")
            if run["archive_bytes"] <= run["output_bytes"]:
                raise ValueError("serialized archive size is incomplete")
        totals = [
            sum(run[field] for field in ("compress_seconds", "verify_seconds", "extract_seconds")) for run in runs
        ]
        if sum(totals) < policy["minimum_measured_seconds"]:
            raise ValueError("insufficient measured confirmation time")
        phases = [
            [run[field] for run in runs] for field in ("compress_seconds", "verify_seconds", "extract_seconds")
        ] + [totals]
        for values in phases:
            if any(value <= 0 for value in values):
                raise ValueError("missing positive phase timing")
            cv = 100 * statistics.stdev(values) / statistics.mean(values)
            if (
                cv > policy["max_relative_std_dev_pct"]
                or cv / math.sqrt(count) > policy["target_relative_standard_error_pct"]
            ):
                raise ValueError("confirmation measurements remain inconclusive")
    return tuple(
        policy[field]
        for field in (
            "method",
            "minimum_count",
            "maximum_count",
            "pilot_count",
            "minimum_measured_seconds",
            "target_relative_standard_error_pct",
            "max_relative_std_dev_pct",
        )
    )


# Purpose: Keep historical CRC-only records separate from independently validated bytewise observations.
# Inputs: Record declares optional new measurement protocol; every pilot and confirmation must match it.
# Outputs: Returns a protocol identity or rejects missing coverage, changed protocol or invalid validation cost.
def validate_integrity_protocol(record: dict) -> str:
    protocol = record.get("measurement_protocol", "crc-only-historical")
    bytewise_protocols = ("bytewise-regenerated-v1", "bytewise-regenerated-v2")
    if protocol not in ("crc-only-historical", *bytewise_protocols):
        raise ValueError("unsupported integrity measurement protocol")
    runs, pilots = record.get("runs"), record.get("pilot_runs", [])
    if not isinstance(runs, list) or not isinstance(pilots, list):
        raise ValueError("invalid integrity observation collections")
    if protocol == "crc-only-historical":
        if any(
            isinstance(run, dict) and run.get("measurement_protocol") in bytewise_protocols for run in runs + pilots
        ):
            raise ValueError("bytewise validation protocol declaration is missing")
        return protocol
    for run in runs + pilots:
        if not isinstance(run, dict):
            raise ValueError("invalid bytewise validation observation")
        if protocol == "bytewise-regenerated-v2" and (
            type(run.get("validation_worker_limit")) is not int
            or not 1 <= run["validation_worker_limit"] <= 64
            or run["validation_worker_limit"] != run.get("workers")
        ):
            raise ValueError("invalid bytewise validation worker budget")
        validation, wall = run.get("validation_seconds"), run.get("wall_seconds")
        if (
            run.get("measurement_protocol") != protocol
            or type(run.get("validated_bytes")) is not int
            or run["validated_bytes"] != run.get("input_bytes")
            or type(validation) not in (int, float)
            or not math.isfinite(validation)
            or validation <= 0
            or type(wall) not in (int, float)
            or not math.isfinite(wall)
            or wall < validation
        ):
            raise ValueError("incomplete or invalid bytewise validation evidence")
    return protocol


# Purpose: Recompute whether the prescribed confirmation workload fit its declared remaining wall budget.
# Inputs: Current raw pilots, frozen case counts, pause duration and a maximum-observed-time budget estimate.
# Outputs: Rejects missing, malformed or inconsistent budget evidence; makes no hard duration guarantee.
def validate_confirmation_wall_budget(record: dict) -> None:
    policy = record.get("sampling_policy", {})
    if not isinstance(policy, dict):
        raise ValueError("invalid confirmation wall budget policy")
    if policy.get("method") == "fixed_count":
        return
    budget = policy.get("confirmation_wall_budget", {})
    if not isinstance(budget, dict):
        raise ValueError("invalid confirmation wall budget")
    remaining = budget.get("remaining_seconds")
    estimate = budget.get("estimated_seconds")
    if (
        budget.get("method") != "maximum_pilot_observation_wall_plus_pause_times_frozen_counts"
        or type(remaining) not in (int, float)
        or not math.isfinite(remaining)
        or remaining <= 0
        or type(policy.get("suite_timeout_seconds")) is not int
        or remaining > policy["suite_timeout_seconds"]
        or type(estimate) not in (int, float)
        or not math.isfinite(estimate)
        or estimate <= 0
        or budget.get("fits_in_remaining_time") is not True
        or estimate > remaining
    ):
        raise ValueError("invalid confirmation wall budget")
    pilots = record.get("pilot_runs", [])
    lanes = {run.get("lane") for run in pilots}
    expected = 0.0
    pause = record.get("inter_run_pause_ms", 0) / 1000
    plans = policy.get("case_plans", [])
    if not isinstance(plans, list) or any(not isinstance(plan, dict) for plan in plans):
        raise ValueError("invalid confirmation wall budget plans")
    for plan in plans:
        count, block = plan.get("confirmation_count"), plan.get("block_size_kib")
        if type(count) is not int or not 1 <= count <= MAX_CONFIRMATION_COUNT:
            raise ValueError("invalid confirmation wall budget count")
        for lane in lanes:
            seconds = [
                run["observation_wall_seconds"]
                for run in pilots
                if run.get("lane") == lane and run.get("block_size_kib") == block
            ]
            if not seconds:
                raise ValueError("confirmation wall budget lacks a pilot case")
            expected += count * (max(seconds) + pause)
    if not expected or not math.isclose(estimate, expected, rel_tol=1e-9, abs_tol=1e-9):
        raise ValueError("confirmation wall budget differs from raw pilots")


# Purpose: Keep receipt-backed cohorts separate from historical observation or endpoint checks.
# Inputs: Source/CLI/app-local DLL identity and matching raw pilot/confirmation snapshots with controller wall times.
# Outputs: Returns a grouping identity or rejects metadata; evidence is unauthenticated and excludes loaded HIP drivers.
def validate_artifact_measurement_identity(record: dict) -> tuple:
    policy = record.get("measurement_identity_policy", "historical-endpoint-only")
    runs = record.get("runs", []) + record.get("pilot_runs", [])
    if any(not isinstance(run, dict) for run in runs):
        raise ValueError("invalid measurement identity observation")
    if policy == "historical-endpoint-only":
        if any(run.get("measurement_identity") is not None for run in runs):
            raise ValueError("measurement identity policy declaration is missing")
        return (policy,)
    if policy not in ("source-artifacts-around-observation-v1", "native-build-receipt-around-observation-v2"):
        raise ValueError("unsupported measurement identity policy")
    dependencies = record.get("binary_dependencies_sha256")
    if not isinstance(dependencies, dict) or any(
        not isinstance(name, str)
        or not name.lower().endswith(".dll")
        or any(char in name for char in "/\\:")
        or not isinstance(digest, str)
        or not re.fullmatch(r"[0-9a-f]{64}", digest)
        for name, digest in dependencies.items()
    ):
        raise ValueError("invalid app-local runtime identity")
    expected = {name: record.get(name) for name in ("source_commit", "source_dirty", "binary_sha256")}
    expected["binary_dependencies_sha256"] = dependencies
    receipt_identity = ()
    if policy == "native-build-receipt-around-observation-v2":
        receipt_identity = validate_native_receipt_record(record)
        expected["native_build_receipt_sha256"] = record["native_build_receipt_sha256"]
        expected["native_inputs_sha256"] = record["native_inputs_sha256"]
    elif any(
        record.get(key) is not None
        for key in ("native_build_receipt", "native_build_receipt_sha256", "native_inputs_sha256")
    ):
        raise ValueError("native receipt identity policy declaration is missing")
    for run in runs:
        identity = run.get("measurement_identity")
        wall = run.get("observation_wall_seconds")
        native_wall = run.get("wall_seconds")
        if (
            not isinstance(identity, dict)
            or type(identity.get("source_dirty")) is not bool
            or identity != expected
            or type(wall) not in (int, float)
            or not math.isfinite(wall)
            or type(native_wall) not in (int, float)
            or not math.isfinite(native_wall)
            or native_wall <= 0
            or wall < native_wall
            or wall <= 0
        ):
            raise ValueError("observation measurement identity or wall time changed")
    validate_confirmation_wall_budget(record)
    if receipt_identity:
        return (policy, tuple(sorted(dependencies.items())), receipt_identity)
    return (policy, tuple(sorted(dependencies.items())))


# Purpose: Bind a historical or current exported receipt to its actual measured artifacts.
# Inputs: Untrusted exported receipt, canonical digests and observation lanes; no live build is qualified.
# Outputs: Distinct toolchain-scope grouping identity or fail-closed error; missing historical evidence stays missing.
def validate_native_receipt_record(record: dict) -> tuple:
    receipt = record.get("native_build_receipt")
    digest = validate_receipt(receipt, allow_historical_toolchain=True)
    inputs_digest = receipt["inputs"]["inputs_sha256"]
    if record.get("native_build_receipt_sha256") != digest or record.get("native_inputs_sha256") != inputs_digest:
        raise ValueError("native receipt or input digest differs from measured identity")
    configuration = receipt["recipe"]["configuration"]
    outputs = receipt["outputs_sha256"]
    prefix = f"build/{configuration}/"
    if outputs[prefix + "superzip_cli.exe"] != record.get("binary_sha256"):
        raise ValueError("measured CLI differs from its native build receipt")
    dependencies = {
        name[len(prefix) :]: value
        for name, value in outputs.items()
        if name.startswith(prefix) and name.lower().endswith(".dll")
    }
    if dependencies != record.get("binary_dependencies_sha256"):
        raise ValueError("measured app-local DLLs differ from their native build receipt")
    if receipt["recipe"]["SUPERZIP_ENABLE_HIP"] != "ON" and any(
        run.get("lane") == "GPU" for run in record.get("runs", []) + record.get("pilot_runs", [])
    ):
        raise ValueError("required-HIP observations have a CPU-only build receipt")
    return (digest, inputs_digest, configuration, receipt["toolchain"]["scope"])


# Purpose: Reject malformed or unreviewed benchmark data before charting it.
# Inputs: A parsed historical or current RAM record and a local-preview switch.
# Outputs: Returns normalized run groups or raises ValueError with the invalid field.
def validate_record(record: dict, allow_dirty: bool) -> tuple[tuple, dict]:
    schema = record.get("schema_version")
    if type(schema) is not int or schema not in (1, 2, 3) or record.get("benchmark_kind") != "suzip_ram":
        raise ValueError("unsupported RAM benchmark schema or kind")
    if schema >= 2 and record.get("gpu_utilization_metric") != "process_busiest_engine_pct":
        raise ValueError("unsupported GPU utilization metric")
    lane_order = record.get("lane_order", "cpu_then_gpu")
    pause_ms = record.get("inter_run_pause_ms", 0)
    if (
        lane_order not in ("cpu_then_gpu", "alternating_when_both")
        or type(pause_ms) is not int
        or not 0 <= pause_ms <= 1000
    ):
        raise ValueError("invalid benchmark lane order or pause")
    if type(record.get("source_dirty")) is not bool or (record["source_dirty"] and not allow_dirty):
        raise ValueError("benchmark source must be clean for publication")
    commit = record.get("source_commit")
    binary_hash = record.get("binary_sha256")
    if not isinstance(commit, str) or not re.fullmatch(r"[0-9a-fA-F]{40}", commit):
        raise ValueError("source_commit must be a complete Git SHA")
    if not isinstance(binary_hash, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", binary_hash):
        raise ValueError("binary_sha256 must be a SHA-256 digest")
    profile = record.get("profile")
    if not isinstance(profile, str) or not re.fullmatch(r"[A-Za-z][A-Za-z0-9_-]*", profile):
        raise ValueError("invalid workload profile")
    size_mib = record.get("size_mib")
    level = record.get("compression_level")
    if type(size_mib) is not int or size_mib < 10240 or type(level) is not int or level not in range(1, 10):
        raise ValueError("invalid workload size or effort")
    identity = (
        commit.lower(),
        binary_hash.lower(),
        record.get("cpu_model"),
        record.get("gpu_model"),
        record["source_dirty"],
        schema,
        lane_order,
        pause_ms,
        runtime_version_identity(record.get("hip_runtime_version")),
        validate_integrity_protocol(record),
        validate_artifact_measurement_identity(record),
    )
    groups = defaultdict(lambda: defaultdict(dict))
    runs = record.get("runs")
    if not isinstance(runs, list) or not runs:
        raise ValueError("benchmark record has no runs")
    for run in runs:
        if not isinstance(run, dict) or run.get("lane") not in ("CPU", "GPU"):
            raise ValueError("invalid benchmark lane")
        if run.get("memory_only") is not True or run.get("disk_write_bytes") != 0:
            raise ValueError("benchmark run is not RAM-only")
        if schema >= 2:
            for field in ("gpu_avg_pct", "gpu_peak_pct"):
                value = run.get(field)
                if value is not None and (
                    type(value) not in (int, float) or not math.isfinite(value) or not 0 <= value <= 100
                ):
                    raise ValueError("invalid busiest-engine GPU percentage")
        block_kib = run.get("block_size_kib")
        iteration = run.get("iteration")
        if type(block_kib) is not int or block_kib not in (256, 512, 1024, 2048, 4096, 8192, 16384):
            raise ValueError("invalid production block size")
        if type(iteration) is not int or iteration < 1:
            raise ValueError("invalid iteration")
        if type(run.get("input_bytes")) is not int or run["input_bytes"] != size_mib * 1024 * 1024:
            raise ValueError("run input size differs from workload")
        output_bytes = run.get("output_bytes")
        if type(output_bytes) is not int or output_bytes <= 0:
            raise ValueError("output size must be an exact positive integer")
        phases = [run.get(name) for name in ("compress_seconds", "verify_seconds", "extract_seconds")]
        if any(type(value) not in (int, float) or not math.isfinite(value) or value < 0 for value in phases):
            raise ValueError("invalid phase timing")
        seconds = sum(phases)
        if seconds <= 0:
            raise ValueError("run has no positive elapsed time")
        if run["lane"] == "GPU" and (
            type(run.get("gpu_kernel_launches")) is not int or run["gpu_kernel_launches"] <= 0
        ):
            raise ValueError("GPU lane lacks HIP launch evidence")
        key = (profile, size_mib, level, block_kib)
        lane_runs = groups[key][run["lane"]]
        if iteration in lane_runs:
            raise ValueError("duplicate lane iteration")
        lane_runs[iteration] = (output_bytes, seconds)
    if schema == 3:
        identity += (validate_sampling_protocol(record, allow_dirty),)
    return identity, groups


# Purpose: Derive throughput from median elapsed time without dropping observed timing variation.
# Inputs: Validated records from the same binary and host, with at least three paired runs per case.
# Outputs: Returns exact bytes, median-time throughput, and all-sample dispersion for current records.
def summarize_records(records: list[dict], allow_dirty: bool) -> tuple[tuple, list[dict]]:
    identity = None
    groups = defaultdict(lambda: defaultdict(dict))
    for record in records:
        current_identity, current_groups = validate_record(record, allow_dirty)
        if identity is None:
            identity = current_identity
        elif identity != current_identity:
            raise ValueError("benchmark records use different binaries, hardware, or measurement schemas")
        for key, lanes in current_groups.items():
            for lane, iterations in lanes.items():
                if set(groups[key][lane]).intersection(iterations):
                    raise ValueError("duplicate run across benchmark records")
                groups[key][lane].update(iterations)
    if not groups:
        raise ValueError("no benchmark cases")
    rows = []
    for key, lanes in sorted(groups.items()):
        if set(lanes) != {"CPU", "GPU"} or set(lanes["CPU"]) != set(lanes["GPU"]):
            raise ValueError("every chart case needs paired CPU and HIP iterations")
        if len(lanes["CPU"]) < 3:
            raise ValueError("publishable charts need at least three paired iterations")
        metrics = {}
        for lane, iterations in lanes.items():
            sizes = {size for size, _ in iterations.values()}
            if len(sizes) != 1:
                raise ValueError("encoded size changed between identical lane runs")
            elapsed = statistics.median(seconds for _, seconds in iterations.values())
            metrics[lane] = {
                "output_bytes": sizes.pop(),
                "throughput_gib_s": (key[1] / 1024) / elapsed,
            }
            if identity[5] == 3:
                timings = [seconds for _, seconds in iterations.values()]
                metrics[lane].update(
                    throughput_min_gib_s=(key[1] / 1024) / max(timings),
                    throughput_max_gib_s=(key[1] / 1024) / min(timings),
                    elapsed_std_dev_seconds=statistics.stdev(timings),
                    sample_count=len(timings),
                )
        rows.append(
            {
                "profile": key[0],
                "size_mib": key[1],
                "level": key[2],
                "block_kib": key[3],
                "iterations": len(lanes["CPU"]),
                "metrics": metrics,
            }
        )
    return identity, rows


# Purpose: Add one accessible SVG text label without manual XML escaping.
# Inputs: Parent element, placement, label, style, and optional text anchor.
# Outputs: Appends a namespaced SVG text node.
def add_text(
    parent: ET.Element,
    x: int,
    y: int,
    label: str,
    size: int = 14,
    weight: str = "normal",
    fill: str = "#263238",
    anchor: str = "start",
) -> None:
    node = ET.SubElement(
        parent,
        f"{{{SVG}}}text",
        {
            "x": str(x),
            "y": str(y),
            "font-size": str(size),
            "font-weight": weight,
            "fill": fill,
            "text-anchor": anchor,
            "letter-spacing": "0",
        },
    )
    node.text = label


# Purpose: Render one lane bar with an exact-byte tooltip and a fixed external value column.
# Inputs: Row placement, lane metric, normalization maxima, and selected size/throughput panel.
# Outputs: Adds a bar and its legible numeric label without changing chart geometry.
def add_lane_bar(parent: ET.Element, y: int, x: int, metric: dict, maximum: float, kind: str, color: str) -> None:
    value = metric["output_bytes"] / (1024 * 1024) if kind == "size" else metric["throughput_gib_s"]
    bar = ET.SubElement(
        parent,
        f"{{{SVG}}}rect",
        {
            "x": str(x),
            "y": str(y - 13),
            "width": str(max(2, round(260 * value / maximum))),
            "height": "17",
            "rx": "2",
            "fill": color,
        },
    )
    title = ET.SubElement(bar, f"{{{SVG}}}title")
    title.text = (
        f"{metric['output_bytes']:,} encoded bytes"
        if kind == "size"
        else f"{metric['throughput_gib_s']:.3f} GiB/s, encode + verify + decode"
    )
    label = f"{value:,.1f} MiB" if kind == "size" else f"{value:,.2f} GiB/s"
    add_text(parent, x + 275, y + 1, label, 13, fill="#263238")
    if kind == "speed" and "throughput_min_gib_s" in metric:
        add_sample_range(parent, y, x, metric, maximum)


# Purpose: Expose the full observed throughput range without implying a confidence interval.
# Inputs: One lane's complete sample extrema, elapsed-time sample SD, count, and shared plot scale.
# Outputs: Adds exact-scale range whiskers and an accessible evidence tooltip, including zero-width ranges.
def add_sample_range(parent: ET.Element, y: int, x: int, metric: dict, maximum: float) -> None:
    left = x + 260 * metric["throughput_min_gib_s"] / maximum
    right = x + 260 * metric["throughput_max_gib_s"] / maximum
    center = y - 4.5
    group = ET.SubElement(parent, f"{{{SVG}}}g", {"class": "sample-range", "stroke": "#17252b"})
    ET.SubElement(group, f"{{{SVG}}}title").text = (
        f"All {metric['sample_count']} confirmation samples: "
        f"{metric['throughput_min_gib_s']:.6f} to {metric['throughput_max_gib_s']:.6f} GiB/s; "
        f"elapsed-time sample SD {metric['elapsed_std_dev_seconds']:.6f} s. "
        "Observed min-max range; not a confidence interval."
    )
    for x1, x2, y1, y2 in (
        (left, right, center, center),
        (left, left, center - 4, center + 4),
        (right, right, center - 4, center + 4),
    ):
        ET.SubElement(
            group,
            f"{{{SVG}}}line",
            {"x1": f"{x1:.3f}", "x2": f"{x2:.3f}", "y1": str(y1), "y2": str(y2)},
        )


# Purpose: Plot exact size and throughput from paired median elapsed times with current-sample dispersion.
# Inputs: One source/binary identity and validated chart rows.
# Outputs: Returns deterministic UTF-8 SVG bytes without network access or third-party graphics packages.
def render_svg(identity: tuple, rows: list[dict]) -> bytes:
    if len(rows) > 16:
        raise ValueError("README chart is limited to 16 explicit cases; split larger studies")
    height = 182 + len(rows) * 92
    description = (
        "Paired forced-CPU and required-AMD-HIP SUZIP results. Left: exact encoded payload size, lower is better. "
        "Right: median encode, verify, and decode throughput, higher is better. Synthetic workloads only."
    )
    if identity[5] == 3:
        description = (
            "Paired forced-CPU and required-HIP RAM-only synthetic SUZIP results. "
            "Left: exact encoded payload bytes, excluding archive metadata. "
            "Right: throughput from median elapsed time; "
            "whiskers retain every confirmation sample's min-max range, not confidence intervals."
        )
    root = new_document(
        {
            "viewBox": f"0 0 1200 {height}",
            "width": "1200",
            "height": str(height),
            "font-family": "Segoe UI, Arial, sans-serif",
            "role": "img",
            "aria-labelledby": "title desc",
        },
        "SuperZip RAM-only benchmark",
        description,
    )
    add_text(root, 32, 42, "SuperZip | measured native-format performance", 25, "bold", "#17252b")
    subtitle = "RAM-only synthetic workloads. Paired runs; no archive disk writes."
    if identity[4]:
        subtitle += " DIRTY SOURCE - LOCAL PREVIEW ONLY."
    add_text(root, 32, 70, subtitle, 14, fill="#53656d")
    add_text(root, 318, 116, "Encoded size", 16, "bold")
    add_text(root, 318, 136, "Lower is better", 12, fill="#53656d")
    add_text(root, 786, 116, "End-to-end throughput", 16, "bold")
    speed_caption = "Median elapsed time; whiskers: all-sample min-max" if identity[5] == 3 else "Higher is better"
    add_text(root, 786, 136, speed_caption, 12, fill="#53656d")
    max_size = max(metric["output_bytes"] / (1024 * 1024) for row in rows for metric in row["metrics"].values())
    max_speed = max(
        metric.get("throughput_max_gib_s", metric["throughput_gib_s"])
        for row in rows
        for metric in row["metrics"].values()
    )
    for index, row in enumerate(rows):
        top = 158 + index * 92
        ET.SubElement(
            root,
            f"{{{SVG}}}line",
            {"x1": "32", "x2": "1168", "y1": str(top - 7), "y2": str(top - 7), "stroke": "#dce5e7"},
        )
        size_label = f"{row['size_mib'] / 1024:g} GiB"
        add_text(
            root,
            32,
            top + 10,
            f"{row['profile']} | {size_label} | level {row['level']} | {row['block_kib']} KiB blocks",
            14,
            "bold",
        )
        for lane, offset, color in (("CPU", 38, "#394b53"), ("GPU", 66, "#087f8c")):
            y = top + offset
            add_text(root, 50, y + 1, "Required HIP" if lane == "GPU" else "Forced CPU", 13)
            add_lane_bar(root, y, 318, row["metrics"][lane], max_size, "size", color)
            add_lane_bar(root, y, 786, row["metrics"][lane], max_speed, "speed", color)
    footer_y = height - 20
    minimum_runs = min(row["iterations"] for row in rows)
    add_text(
        root,
        32,
        footer_y,
        (
            f"Median elapsed time of at least {minimum_runs} paired runs | "
            if identity[5] == 3
            else f"Median of at least {minimum_runs} paired runs | "
        )
        + f"commit {identity[0][:12]} | binary SHA-256 {identity[1][:12]}...",
        12,
        fill="#53656d",
    )
    return (ET.tostring(root, encoding="utf-8", xml_declaration=True) + b"\n").replace(b"\n", b"\r\n")


# Purpose: Validate input records and either create or byte-check the committed chart.
# Inputs: Command-line paths and the local-preview dirty-source policy.
# Outputs: Returns zero only when generation or deterministic comparison succeeds.
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", action="append", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--allow-dirty", action="store_true", help="Local preview only; not for publication")
    args = parser.parse_args()
    try:
        records = [json.loads(path.read_text(encoding="utf-8")) for path in args.input]
        identity, rows = summarize_records(records, args.allow_dirty)
        graphic = render_svg(identity, rows)
        if args.check:
            if not args.output.exists() or args.output.read_bytes() != graphic:
                raise ValueError("committed benchmark graph differs from reviewed JSON; regenerate it")
        else:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_bytes(graphic)
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        print(f"benchmark graph: {exc}", file=sys.stderr)
        return 1
    print(f"benchmark graph: {'verified' if args.check else 'created'}; cases={len(rows)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
