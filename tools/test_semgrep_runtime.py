"""Compatibility gates for the exact Semgrep distribution installed by production CI."""

import asyncio
import base64
import importlib
import importlib.metadata
import json
import os
import secrets
import subprocess
import sys
import tempfile
import time
import unittest
from functools import partial
from pathlib import Path
from unittest.mock import patch

from tools.prepare_semgrep_wheel import MANIFEST, read_manifest


class SemgrepRuntimeTests(unittest.TestCase):
    # Purpose: Load the installed scanner and generate disposable signing keys without external services.
    # Inputs: production CI virtual environment. Outputs: in-memory test state; missing dependencies fail explicitly.
    @classmethod
    def setUpClass(cls):
        cls.jwt = importlib.import_module("jwt")
        cls.rsa = importlib.import_module("cryptography.hazmat.primitives.asymmetric.rsa")
        cls.verifier_type = importlib.import_module("semgrep.mcp.utilities.token_verifier").IntrospectionTokenVerifier
        cls.manifest = read_manifest(MANIFEST)
        cls.key = cls.rsa.generate_private_key(public_exponent=65537, key_size=2048)
        cls.secret = secrets.token_bytes(64)
        cls.claims = {
            "sub": "runtime-fixture",
            "client_id": "fixture-client",
            "scope": "scan read",
            "iss": "local-fixture",
            "aud": "scanner-fixture",
            "exp": int(time.time()) + 300,
        }

    # Purpose: Prevent the tested CI environment from silently reverting to an upstream conflict.
    # Inputs: installed distribution metadata. Outputs: exact scanner/JWT identity and declaration assertions.
    def test_distribution_identity(self):
        self.assertEqual(importlib.metadata.version("semgrep"), self.manifest["local_version"])
        expected = self.manifest["tested_requirement"].split("==")[1]
        self.assertEqual(importlib.metadata.version("pyjwt"), expected)
        requirements = importlib.metadata.requires("semgrep")
        self.assertIn(self.manifest["tested_requirement"], requirements)
        self.assertNotIn(self.manifest["original_requirement"], requirements)

    # Purpose: Preserve signed decoding, claim checks and signature refusal in the newer JWT library.
    # Inputs: fresh HMAC/RSA keys and ephemeral claims. Outputs: accepted legitimate claims and rejected invalid tokens.
    def test_jwt_controls(self):
        for algorithm in ("HS256", "HS384", "HS512", "RS256"):
            with self.subTest(algorithm=algorithm):
                key = self.key if algorithm == "RS256" else self.secret
                public = self.key.public_key() if algorithm == "RS256" else key
                token = self.jwt.encode(self.claims, key, algorithm=algorithm)
                kwargs = {"algorithms": [algorithm], "issuer": "local-fixture", "audience": "scanner-fixture"}
                self.assertEqual(self.jwt.decode(token, public, **kwargs), self.claims)
                self.assertEqual(self.jwt.get_unverified_header(token)["alg"], algorithm)
                self.assertEqual(self.jwt.decode(token, options={"verify_signature": False}), self.claims)
                wrong = (
                    self.rsa.generate_private_key(public_exponent=65537, key_size=2048).public_key()
                    if algorithm == "RS256"
                    else secrets.token_bytes(64)
                )
                with self.assertRaises(self.jwt.InvalidSignatureError):
                    self.jwt.decode(token, wrong, **kwargs)
                expired = self.jwt.encode({**self.claims, "exp": int(time.time()) - 60}, key, algorithm=algorithm)
                with self.assertRaises(self.jwt.ExpiredSignatureError):
                    self.jwt.decode(expired, public, **kwargs)
                with self.assertRaises(self.jwt.InvalidAudienceError):
                    self.jwt.decode(token, public, algorithms=[algorithm], audience="different-fixture")

    # Purpose: Exercise Semgrep's actual JWKS verifier without networking or persistent credentials.
    # Inputs: controlled JWKS and signed claims. Outputs: exact AccessToken or refusal.
    def test_semgrep_mcp_consumer(self):
        verifier = self.verifier_type(
            "https://fixture.invalid/introspection", "https://fixture.invalid/jwks", "https://fixture.invalid/mcp"
        )
        jwk = json.loads(self.jwt.algorithms.RSAAlgorithm.to_jwk(self.key.public_key()))
        jwk.update({"kid": "fixture-key", "alg": "RS256", "use": "sig"})
        kwargs = {"algorithm": "RS256", "headers": {"kid": "fixture-key"}}
        token = self.jwt.encode(self.claims, self.key, **kwargs)
        with patch.object(verifier._jwks_client, "fetch_data", return_value={"keys": [jwk]}):
            access = asyncio.run(verifier.verify_token(token))
            self.assertIsNotNone(access)
            self.assertEqual(access.client_id, "fixture-client")
            self.assertEqual(access.scopes, ["scan", "read"])
            self.assertEqual(access.resource, "scanner-fixture")
            self.assertEqual(access.expires_at, self.claims["exp"])
            expired = self.jwt.encode({**self.claims, "exp": int(time.time()) - 60}, self.key, **kwargs)
            wrong_key = self.rsa.generate_private_key(public_exponent=65537, key_size=2048)
            invalid = self.jwt.encode(self.claims, wrong_key, **kwargs)
            for rejected in (expired, invalid, "invalid"):
                self.assertIsNone(asyncio.run(verifier.verify_token(rejected)))

    # Purpose: Verify recursive payload failure containment independently of interpreter nesting limits.
    # Inputs: local unsigned token and deterministic JSON fault. Outputs: DecodeError before any JWKS fetch.
    # Method: PyJWT's upstream cross-version regression, commit 9bc06658f875b9b40091539140bbbdc4639161c3.
    def test_preverification_parser_failure(self):
        header_data = {"alg": "RS256", "kid": "fixture-key"}
        header = base64.urlsafe_b64encode(json.dumps(header_data).encode()).rstrip(b"=")
        payload = base64.urlsafe_b64encode(b"{}").rstrip(b"=")
        token = b".".join((header, payload, b""))
        client = self.jwt.PyJWKClient("https://fixture.invalid/jwks")
        consumers = (partial(self.jwt.decode, options={"verify_signature": False}), client.get_signing_key_from_jwt)
        with patch.object(client, "fetch_data") as fetch:
            for consumer in consumers:
                with (
                    patch("jwt.api_jwt.json.loads", side_effect=[header_data, RecursionError("local parser fault")]),
                    self.assertRaises(self.jwt.DecodeError) as failure,
                ):
                    consumer(token)
                self.assertIsInstance(failure.exception.__cause__, RecursionError)
            fetch.assert_not_called()

    # Purpose: Preserve scanner exit semantics, negative controls and standard SARIF output.
    # Inputs: temporary benign rule and files. Outputs: detected/clean controls and one SARIF finding.
    def test_scanner_controls(self):
        executable = Path(sys.executable).parent / ("semgrep.exe" if os.name == "nt" else "semgrep")
        self.assertTrue(executable.is_file(), "Semgrep executable must belong to the tested interpreter")
        with tempfile.TemporaryDirectory(prefix="semgrep-runtime-") as directory:
            root = Path(directory)
            rule = root / "control.yml"
            rule.write_text(
                "rules:\n- id: runtime-print\n  languages: [python]\n  pattern: print(...)\n"
                "  message: Runtime positive control\n  severity: INFO\n",
                encoding="utf-8",
            )
            positive = root / "positive.py"
            positive.write_text('print("runtime fixture")\n', encoding="utf-8")
            negative = root / "negative.py"
            negative.write_text("value = 42\n", encoding="utf-8")
            env = {
                **os.environ,
                "HOME": str(root),
                "SEMGREP_SETTINGS_FILE": str(root / "settings.yml"),
                "SEMGREP_SEND_METRICS": "off",
                "SEMGREP_ENABLE_VERSION_CHECK": "0",
            }
            env.pop("SEMGREP_APP_TOKEN", None)
            common = [
                str(executable),
                "scan",
                "--metrics=off",
                "--disable-version-check",
                "--jobs",
                "1",
                "--no-git-ignore",
                "--config",
                str(rule),
            ]
            for target, code, count in ((positive, 1, 1), (negative, 0, 0)):
                completed = subprocess.run(
                    common + ["--error", "--json", str(target)],
                    cwd=root,
                    env=env,
                    capture_output=True,
                    check=False,
                    timeout=120,
                )
                self.assertEqual(completed.returncode, code, completed.stderr.decode(errors="replace")[-2000:])
                scan = json.loads(completed.stdout)
                self.assertEqual(len(scan["results"]), count)
                self.assertEqual(scan["errors"], [])
            completed = subprocess.run(
                common + ["--sarif", str(positive)], cwd=root, env=env, capture_output=True, check=False, timeout=120
            )
            self.assertEqual(completed.returncode, 0, completed.stderr.decode(errors="replace")[-2000:])
            sarif = json.loads(completed.stdout)
            self.assertEqual(sarif["version"], "2.1.0")
            self.assertEqual(len(sarif["runs"][0]["results"]), 1)


if __name__ == "__main__":
    unittest.main()
