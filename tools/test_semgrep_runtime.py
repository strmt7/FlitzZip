"""Compatibility gates for the exact Semgrep distribution installed by production CI."""

import asyncio
import base64
import importlib
import importlib.metadata
import json
import os
import re
import secrets
import subprocess
import sys
import tempfile
import time
import unittest
from functools import partial
from pathlib import Path
from unittest.mock import Mock, patch

from tools.semgrep_distribution import recipe

REQUIREMENTS = Path(__file__).resolve().parents[1] / ".github/requirements"


# Purpose: Read one exact package identity from a tooling input or generated lock.
# Inputs: requirements text and package name. Outputs: the unique pinned version or ValueError.
def pinned_version(contents: str, package: str) -> str:
    matches = re.findall(rf"(?m)^{re.escape(package)}(?:\[[a-z0-9,]+\])?==([a-zA-Z0-9.+-]+)(?=\s|$)", contents)
    if len(matches) != 1:
        raise ValueError(f"Expected one exact pin for {package}")
    return matches[0]


class SemgrepRuntimeTests(unittest.TestCase):
    # Purpose: Load the installed scanner and generate disposable signing keys without external services.
    # Inputs: production CI virtual environment. Outputs: in-memory test state; missing dependencies fail explicitly.
    @classmethod
    def setUpClass(cls):
        cls.jwt = importlib.import_module("jwt")
        cls.rsa = importlib.import_module("cryptography.hazmat.primitives.asymmetric.rsa")
        cls.verifier_type = importlib.import_module("semgrep.mcp.utilities.token_verifier").IntrospectionTokenVerifier
        locked = (REQUIREMENTS / "requirements-semgrep-linux.txt").read_text(encoding="utf-8")
        cls.scanner_version = pinned_version(locked, "semgrep")
        cls.jwt_version = pinned_version(locked, "pyjwt")
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

    # Purpose: Require the identified scanner revision and official fixed MCP/JWT distributions.
    # Inputs: Installed metadata and pinned recipe. Outputs: Exact identities and satisfied declared dependencies.
    def test_distribution_identity(self):
        from packaging.requirements import Requirement
        from packaging.version import Version

        self.assertEqual(importlib.metadata.version("semgrep"), self.scanner_version)
        self.assertEqual(self.scanner_version, recipe()["version"])
        self.assertEqual(importlib.metadata.version("mcp"), recipe()["mcp_after"])
        self.assertEqual(importlib.metadata.version("pyjwt"), self.jwt_version)
        requirements = [Requirement(item) for item in importlib.metadata.requires("semgrep")]
        jwt_requirements = [item for item in requirements if item.name.lower() == "pyjwt"]
        self.assertEqual(len(jwt_requirements), 1)
        self.assertIn("crypto", jwt_requirements[0].extras)
        self.assertIn(Version(self.jwt_version), jwt_requirements[0].specifier)

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

    def test_tracestate_rejects_hostile_headers_with_bounded_work(self):
        """Purpose: Detect the quadratic untrusted-header parser, independently of implementation details.
        Inputs: Valid propagation and 200 KB hostile whitespace runs in an isolated bounded child.
        Outputs: Preserves valid entries and rejects hostile headers without retaining a busy worker.
        """
        program = (
            "from opentelemetry.trace import TraceState\n"
            "assert dict(TraceState.from_header(['alpha=one,beta=two'])) == {'alpha':'one','beta':'two'}\n"
            "for whitespace in (' ', '\\t'):\n"
            " assert not dict(TraceState.from_header(['a=b' + whitespace * 200000 + 'x']))\n"
            "print('tracestate bounded rejection passed')\n"
        )
        completed = subprocess.run(
            [sys.executable, "-c", program], capture_output=True, text=True, timeout=5, check=True
        )
        self.assertEqual(completed.stdout.strip(), "tracestate bounded rejection passed")

    def test_semgrep_telemetry_exports_through_fixed_cohort(self):
        """Purpose: Exercise the scanner's resource and exporter consumers with the coordinated SDK upgrade.
        Inputs: One in-memory span and a mocked HTTP transport; no network or persistent telemetry.
        Outputs: A real protobuf export succeeds, keeps its resource, and releases its provider/session.
        """
        import requests
        from opentelemetry.sdk.resources import Resource
        from opentelemetry.sdk.trace import TracerProvider
        from opentelemetry.sdk.trace.export import SimpleSpanProcessor, SpanExportResult
        from opentelemetry.sdk.trace.export.in_memory_span_exporter import InMemorySpanExporter
        from semgrep.telemetry import MutableResource, OTLPSpanExporter

        resource = MutableResource(Resource({"service.name": "scanner-fixture"}))
        provider = TracerProvider(resource=resource)
        spans = InMemorySpanExporter()
        provider.add_span_processor(SimpleSpanProcessor(spans))
        with requests.Session() as session:
            exporter = OTLPSpanExporter(endpoint="https://fixture.invalid/v1/traces", session=session)
            try:
                with provider.get_tracer("consumer-fixture").start_as_current_span("local-scan") as span:
                    span.set_attribute("scan.engine_type", "fixture")
                finished = spans.get_finished_spans()
                self.assertEqual(len(finished), 1)
                self.assertEqual(finished[0].resource.attributes["service.name"], "scanner-fixture")
                response = Mock(ok=True, status_code=200, content=b"")
                with patch.object(session, "send", return_value=response) as send:
                    self.assertEqual(exporter.export(finished), SpanExportResult.SUCCESS)
                    send.assert_called_once()
                    self.assertTrue(send.call_args.args[0].body)
            finally:
                exporter.shutdown()
                provider.shutdown()

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


class McpSessionTests(unittest.IsolatedAsyncioTestCase):
    # Purpose: Exercise the repaired session lifecycle through actual HTTP requests without sockets.
    # Inputs: The installed official MCP implementation and two small initialized sessions.
    # Outputs: Closed/refused sessions release their state; capacity recovers without restarting the server.
    async def test_session_capacity_cleanup_and_reuse(self):
        from httpx import ASGITransport, AsyncClient
        from mcp.server.lowlevel import Server
        from mcp.server.streamable_http_manager import StreamableHTTPSessionManager

        manager = StreamableHTTPSessionManager(Server("session-fixture"), json_response=True, max_sessions=2)
        headers = {"Accept": "application/json, text/event-stream"}
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": "2025-03-26",
                "clientInfo": {"name": "fixture", "version": "1"},
                "capabilities": {},
            },
        }
        async with (
            manager.run(),
            AsyncClient(
                transport=ASGITransport(app=manager.handle_request), base_url="https://fixture.invalid"
            ) as client,
        ):
            sessions = []
            for _ in range(2):
                response = await client.post("/", json=request, headers=headers)
                self.assertEqual(response.status_code, 200, response.text)
                self.assertEqual(response.json()["result"]["serverInfo"]["name"], "session-fixture")
                sessions.append(response.headers["mcp-session-id"])
            self.assertNotEqual(*sessions)
            refused = await client.post("/", json=request, headers=headers)
            self.assertEqual(refused.status_code, 503)
            self.assertEqual(len(manager._server_instances), 2)
            for session in sessions:
                session_headers = {**headers, "mcp-session-id": session, "mcp-protocol-version": "2025-03-26"}
                removed = await client.delete("/", headers=session_headers)
                self.assertEqual(removed.status_code, 200, removed.text)
                self.assertNotIn(session, manager._server_instances)
                stale = await client.post("/", json=request, headers=session_headers)
                self.assertEqual(stale.status_code, 404)
            invalid = await client.post("/", json=request, headers={"Accept": "text/plain"})
            self.assertEqual(invalid.status_code, 406)
            self.assertEqual(manager._server_instances, {})
            reopened = await client.post("/", json=request, headers=headers)
            self.assertEqual(reopened.status_code, 200, reopened.text)
            self.assertNotIn(reopened.headers["mcp-session-id"], sessions)
        self.assertEqual(manager._server_instances, {})


if __name__ == "__main__":
    unittest.main()
