"""Independent controls for settings schema evolution and historical compatibility."""

import copy
import json
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

from jsonschema import Draft202012Validator

from tools.settings_contract import (
    ROOT,
    assert_fixtures_unchanged,
    load_contract,
    read_contract_json,
    unique_fields,
    validate_evolution,
    validate_profile_locations,
)


class SettingsContractTests(unittest.TestCase):
    # Purpose: Require direct migration from every older versioned profile without speculative unreachable loops.
    # Inputs: An independent future generation and ordered, omitted, duplicated or misregistered historical names.
    # Outputs: Exact newest-first history passes; each omission, reordering or identity mismatch is rejected.
    def test_native_profile_history_is_complete_and_ordered(self):
        contract = {"currentGeneration": 6, "frozenSchemas": {"4": "four", "5": "five", "6": "six"}}
        source = (
            "inline constexpr int kSettingsProfileGeneration = 6;\n"
            "inline constexpr std::array<std::wstring_view, 2> kPreviousSettingsFilenames"
            '{L"settings-v5.json", L"settings-v4.json"};'
        )
        validate_profile_locations(source, contract)
        mutations = (
            source.replace("Generation = 6", "Generation = 5"),
            source.replace(", 2>", ", 1>"),
            source.replace('L"settings-v5.json", ', ""),
            source.replace("settings-v5", "settings-v4"),
            source.replace("settings-v4", "settings-v7"),
            source.replace("settings-v5", "settings-v6"),
            source.replace('L"settings-v5.json", L"settings-v4.json"', 'L"settings-v4.json", L"settings-v5.json"'),
        )
        for changed in mutations:
            with self.subTest(source=changed), self.assertRaises(ValueError):
                validate_profile_locations(changed, contract)

    # Purpose: Check the maintained public contract and all canonical profile fixtures.
    # Inputs: Current schema, registry and independently authored profile documents.
    # Outputs: Every generation is retained and current compatible/future-field examples validate.
    def test_registry_and_current_examples(self):
        contract, schema, frozen = load_contract()
        validate_evolution(contract, schema, frozen)
        assert_fixtures_unchanged()
        validator = Draft202012Validator(schema)
        for name in ("current-v4.json", "future-fields-v4.json"):
            value = json.loads((ROOT / "tests/fixtures/settings" / name).read_text(encoding="utf-8"))
            validator.validate(value)
        future = json.loads((ROOT / "tests/fixtures/settings/unsupported-v999.json").read_text(encoding="utf-8"))
        self.assertFalse(validator.is_valid(future))
        for row in contract["fixtures"]:
            value = read_contract_json(ROOT, "tests/fixtures/settings/" + row["file"])
            self.assertEqual(value["schema"], f"flitzzip.settings.v{row['generation']}")

    # Purpose: Detect unsafe changes using mutations of the actual schema rather than implementation-derived values.
    # Inputs: Deleted/renamed keys, changed types/defaults/enums and reused retired identifiers.
    # Outputs: Breaking changes fail; an independent optional field with a declared default remains compatible.
    def test_breaking_changes_require_new_generation(self):
        contract, schema, frozen = load_contract()
        mutations = []
        deleted = copy.deepcopy(schema)
        del deleted["properties"]["overwrite"]
        mutations.append(deleted)
        for key, value in (("type", "string"), ("default", True)):
            altered = copy.deepcopy(schema)
            altered["properties"]["overwrite"][key] = value
            mutations.append(altered)
        reused = copy.deepcopy(schema)
        reused["properties"]["compressionLevelIndex"] = {"type": "integer", "default": 0}
        mutations.append(reused)
        enum_change = copy.deepcopy(schema)
        enum_change["properties"]["logLevel"]["enum"].append("trace")
        mutations.append(enum_change)
        required = copy.deepcopy(schema)
        required["required"].append("showOperationSummary")
        mutations.append(required)
        for altered in mutations:
            with self.subTest(properties=altered["properties"]), self.assertRaises(ValueError):
                validate_evolution(contract, altered, frozen)
        added = copy.deepcopy(schema)
        added["properties"]["newIndependentFeature"] = {"type": "boolean", "default": False}
        validate_evolution(contract, added, frozen)

    # Purpose: Prevent updates from silently dropping old readers, migration paths or version isolation.
    # Inputs: Missing/backward/cyclic migration edges, missing fixtures and a reused profile filename.
    # Outputs: Each defective compatibility registry is rejected independently.
    def test_migration_graph_and_profile_identity_controls(self):
        contract, schema, frozen = load_contract()
        for field, value in (
            ("migrationEdges", {"1": 4, "2": 1, "3": 4}),
            ("migrationEdges", {"1": 4, "3": 4}),
            ("readableGenerations", [2, 3, 4]),
            ("fixtures", contract["fixtures"][1:]),
            ("currentProfile", "settings.json"),
        ):
            with self.subTest(field=field, value=value):
                altered = copy.deepcopy(contract)
                altered[field] = value
                with self.assertRaises(ValueError):
                    validate_evolution(altered, schema, frozen)

    # Purpose: Keep newly serialized persistent identifiers explicit and documented before source can publish them.
    # Inputs: The production serializer's literal field names and the public schema properties.
    # Outputs: Missing declarations and silently renamed disk keys fail without relying on coverage percentages.
    def test_serializer_declares_only_registered_keys(self):
        _contract, schema, _frozen = load_contract()
        source = (ROOT / "src/app/main_window_archive_support.cpp").read_text(encoding="utf-8")
        start = source.index("std::string settings_to_json(")
        body = source[start : source.index("\n}\n", start)]
        keys = set(re.findall(r'\\"([A-Za-z][A-Za-z0-9]*)\\"\s*:', body))
        self.assertEqual(keys, set(schema["properties"]))
        profile = (ROOT / "src/app/settings_profile.hpp").read_text(encoding="utf-8")
        generation = int(re.search(r"kSettingsProfileGeneration\s*=\s*(\d+)", profile)[1])
        self.assertEqual(generation, _contract["currentGeneration"])

    # Purpose: Prevent ambiguous declarations from concealing a compatibility-breaking key replacement.
    # Inputs: Independently authored duplicate JSON members. Outputs: Refuses them rather than taking a last value.
    def test_contract_members_are_unambiguous(self):
        with self.assertRaisesRegex(ValueError, "duplicate"):
            json.loads('{"generation":4,"generation":5}', object_pairs_hook=unique_fields)

    # Purpose: Prove the history guard rejects rewritten evidence while allowing new generation fixtures.
    # Inputs: A disposable repository with one committed profile and an independent mutation.
    # Outputs: Unchanged/new evidence passes; editing or removing the old fixture fails.
    def test_fixture_history_guard_detects_edits(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fixture = root / "tests/fixtures/settings/legacy-v1.json"
            fixture.parent.mkdir(parents=True)
            fixture.write_text('{"schema":"v1"}\n', encoding="utf-8")
            commands = [
                ["init", "--quiet"],
                ["add", "."],
                [
                    "-c",
                    "user.name=Fixture",
                    "-c",
                    "user.email=fixture@example.invalid",
                    "-c",
                    "core.hooksPath=.empty-hooks",
                    "commit",
                    "--quiet",
                    "-m",
                    "Fixture",
                ],
            ]
            for args in commands:
                subprocess.run(["git", "-C", str(root), *args], check=True, capture_output=True, timeout=15)
            assert_fixtures_unchanged(root, "HEAD")
            (fixture.parent / "new-v2.json").write_text("{}\n", encoding="utf-8")
            assert_fixtures_unchanged(root, "HEAD")
            fixture.write_text('{"schema":"changed"}\n', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "append-only"):
                assert_fixtures_unchanged(root, "HEAD")
            fixture.unlink()
            with self.assertRaisesRegex(ValueError, "append-only"):
                assert_fixtures_unchanged(root, "HEAD")

    # Purpose: Reject new validation constraints even when existing field names still match.
    # Inputs: Root and property validation mutations. Outputs: Breaking changes require a new generation.
    def test_new_validation_rules_cannot_change_existing_generation(self):
        contract, schema, frozen = load_contract()
        for mutate_root in (False, True):
            altered = copy.deepcopy(schema)
            if mutate_root:
                altered["maxProperties"] = 1
            else:
                altered["properties"]["compressionFormat"]["minLength"] = 20
            with self.assertRaises(ValueError):
                validate_evolution(contract, altered, frozen)


if __name__ == "__main__":
    unittest.main()
