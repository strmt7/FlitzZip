"""Install and invoke FlitzZip's isolated, self-hosted development crawler.

This product includes software developed by UncleCode (https://x.com/unclecode)
as part of the Crawl4AI project (https://github.com/unclecode/crawl4ai).
"""

from __future__ import annotations

import argparse
import contextlib
import errno
import hashlib
import importlib.util
import json
import os
import platform
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time
import venv
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# Purpose: Support script use. Inputs: This file's package context. Outputs: Exact checkout imports.
if not __package__:
    sys.path.insert(0, str(ROOT))
LOCK = ROOT / "tools/requirements/crawl4ai.txt"
VERSION = "0.9.5+flitzzip.portable2"
ATTRIBUTION = (
    "This product includes software developed by UncleCode (https://x.com/unclecode) "
    "as part of the Crawl4AI project (https://github.com/unclecode/crawl4ai)."
)


def cache_home(override: str | None = None) -> Path:
    """Purpose: Locate a portable tool cache. Inputs: Optional absolute override. Outputs: Absolute path."""
    selected = override or os.environ.get("FLITZZIP_CRAWL4AI_HOME")
    if selected:
        result = Path(selected).expanduser()
        if not result.is_absolute():
            raise ValueError("Crawler home must be an absolute path")
    elif sys.platform == "win32":
        result = Path(os.environ.get("LOCALAPPDATA", str(Path.home() / "AppData/Local"))) / "FlitzZip/crawl4ai"
    elif sys.platform == "darwin":
        result = Path.home() / "Library/Caches/FlitzZip/crawl4ai"
    else:
        result = Path(os.environ.get("XDG_CACHE_HOME", str(Path.home() / ".cache"))) / "flitzzip/crawl4ai"
    result = result.resolve()
    if result == ROOT or ROOT in result.parents:
        raise ValueError("Crawler environment must stay outside the source checkout")
    return result


def environment_paths(home: Path) -> tuple[Path, Path, str]:
    """Purpose: Isolate dependency revisions. Inputs: Cache root. Outputs: Environment, Python and lock identity."""
    from tools import crawl4ai_source_build, nltk_distribution

    identity = hashlib.sha256(
        (text_identity(LOCK) + nltk_distribution.identity() + crawl4ai_source_build.identity()).encode("ascii")
    ).hexdigest()
    name = f"{VERSION}-py{sys.version_info.major}{sys.version_info.minor}-{identity[:16]}"
    directory = home / name
    python = directory / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    return directory, python, identity


def text_identity(path: Path) -> str:
    """Purpose: Match Git's portable text identity. Inputs: Text file. Outputs: LF-normalized SHA-256 digest."""
    return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def tool_environment(home: Path) -> dict[str, str]:
    """Purpose: Keep browser/cache writes scoped. Inputs: Tool cache. Outputs: Child-only environment."""
    env = dict(os.environ)
    env.update(
        FLITZZIP_CRAWL4AI_HOME=str(home),
        PLAYWRIGHT_BROWSERS_PATH=str(home / "browsers"),
        CRAWL4AI_BASE_DIRECTORY=str(home / "data"),
        PYTHONUTF8="1",
        PYTHONNOUSERSITE="1",
        DO_NOT_TRACK="1",
        LITELLM_TELEMETRY="False",
        HF_HUB_DISABLE_TELEMETRY="1",
        TOKENIZERS_PARALLELISM="false",
    )
    return env


def retain_setup_output(stream, tail: bytearray, overflow: threading.Event) -> None:
    """Purpose: Retain bounded setup diagnostics. Inputs: Pipe/tail/limit event. Outputs: Last 8 KiB and overflow."""
    total = 0
    for chunk in iter(lambda: stream.read(8192), b""):
        total += len(chunk)
        tail.extend(chunk)
        del tail[:-8192]
        if total > 4 * 1024 * 1024:
            overflow.set()


def run_owned(command: list[str], env: dict[str, str], timeout: float = 900, *, quiet: bool = False) -> int:
    """Purpose: Run hidden, owned children. Inputs: Command/environment/deadline. Outputs: Exit status or timeout."""
    containment = None
    options = {"start_new_session": True}
    if os.name == "nt":
        # Load the repository's tested process contract, never an installed MCP SDK with the same package name.
        spec = importlib.util.spec_from_file_location("flitzzip_owned_child", ROOT / "mcp/flitzzip_mcp.py")
        if spec is None or spec.loader is None:
            raise ValueError("Repository process ownership contract is unavailable")
        owner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(owner)
        admitted_bytes = owner.admit_child_memory_bytes()

        options = {
            "creationflags": subprocess.CREATE_NO_WINDOW
            | subprocess.BELOW_NORMAL_PRIORITY_CLASS
            | owner.CREATE_SUSPENDED
        }
    child = subprocess.Popen(
        command,
        env=env,
        cwd=ROOT,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE if quiet else sys.stdout,
        stderr=sys.stderr,
        **options,
    )
    tail = bytearray()
    overflow = threading.Event()
    reader = None
    failed = True
    try:
        if os.name == "nt":
            containment = owner.ChildContainment(child, admitted_bytes)
            owner.resume_owned_child(child)
        if quiet:
            reader = threading.Thread(target=retain_setup_output, args=(child.stdout, tail, overflow), daemon=True)
            reader.start()
            deadline = time.monotonic() + timeout
            while child.poll() is None:
                if overflow.is_set():
                    raise ValueError("Crawler setup exceeded its 4 MiB output budget")
                if time.monotonic() >= deadline:
                    raise subprocess.TimeoutExpired(command, timeout)
                time.sleep(0.05)
        code = child.wait(timeout=timeout)
        if overflow.is_set():
            raise ValueError("Crawler setup exceeded its 4 MiB output budget")
        failed = code != 0
        return code
    finally:
        try:
            if containment is not None:
                containment.close()
            elif os.name != "nt":
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(child.pid, signal.SIGKILL)
        finally:
            if child.poll() is None:
                child.kill()
            child.wait(timeout=5)
            if reader is not None:
                reader.join(timeout=5)
            if quiet and child.stdout is not None:
                child.stdout.close()
                if (failed or overflow.is_set()) and tail:
                    print(tail.decode("utf-8", errors="replace"), file=sys.stderr)
                if overflow.is_set() and not failed:
                    raise ValueError("Crawler setup exceeded its 4 MiB output budget")


def run_source_python(command: list[str], env: dict[str, str], timeout: float = 900, *, quiet: bool = False) -> int:
    """Purpose: Import source. Inputs: Trusted Python argv/environment. Outputs: Owned exit without cache reuse."""
    if not command:
        raise ValueError("Source-only Python requires an interpreter command")
    temporary_root = Path(tempfile.gettempdir()).resolve()
    with tempfile.TemporaryDirectory(prefix="flitzzip-crawl4ai-bytecode-", dir=temporary_root) as temporary:
        directory = Path(temporary)
        if directory.resolve().parent != temporary_root:
            raise ValueError("Source-only import cache escaped its owned temporary root")
        runtime_env = {**env, "PYTHONDONTWRITEBYTECODE": "1", "PYTHONPYCACHEPREFIX": str(directory)}
        runtime = [command[0], "-B", "-X", f"pycache_prefix={directory}", *command[1:]]
        return run_owned(runtime, runtime_env, timeout, quiet=quiet)


def run(command: list[str], env: dict[str, str], timeout: int = 900, *, source_only: bool = False) -> None:
    """Purpose: Require setup success. Inputs: Command, environment and import policy. Outputs: Success or error."""
    execute = run_source_python if source_only else run_owned
    code = execute(command, env, timeout, quiet=True)
    if code:
        raise subprocess.CalledProcessError(code, command)


def verify_versions(python: Path, env: dict[str, str]) -> None:
    """Purpose: Verify dependency/repair bytes. Inputs: Isolated runtime/environment. Outputs: Exact pins or error."""
    from tools import crawl4ai_source_build, nltk_distribution

    pins = dict(re.findall(r"^([\w-]+)==([^\s]+)", LOCK.read_text(encoding="utf-8"), re.M))
    pins["nltk"] = nltk_distribution.distribution()["version"]
    script = (
        "import importlib.metadata,json,sys,hashlib; pins=json.loads(sys.argv[1]); "
        "bad=[n for n,v in pins.items() if importlib.metadata.version(n)!=v]; "
        "dist=importlib.metadata.distribution('crawl4ai'); expected=json.loads(sys.argv[2]); "
        "bad.extend(n for n,h in expected.items() if "
        "hashlib.sha256(dist.locate_file(n).read_bytes().replace(b'\\r\\n',b'\\n')).hexdigest()!=h); "
        "sys.exit('Locked dependency or crawler repair bytes changed: '+','.join(bad)) if bad else None; "
        "from tools.nltk_distribution import verify_installed_runtime; verify_installed_runtime(sys.argv[3])"
    )
    run(
        [
            str(python),
            "-c",
            script,
            json.dumps(pins),
            json.dumps(crawl4ai_source_build.runtime_hashes()),
            nltk_distribution.runtime_source_sha256(cache_home(env.get("FLITZZIP_CRAWL4AI_HOME"))),
        ],
        env,
        60,
        source_only=True,
    )


def lock_contended(error: OSError) -> bool:
    """Purpose: Classify portable advisory-lock contention.
    Inputs: Open or lock error. Outputs: True only for retryable ownership conflicts.
    """
    windows_code = getattr(error, "winerror", None)
    if windows_code is not None:
        return windows_code in (32, 33)
    return error.errno in (errno.EACCES, errno.EAGAIN, errno.EDEADLK)


@contextlib.contextmanager
def installation_lock(directory: Path, timeout: float = 900):
    """Purpose: Serialize shared-cache setup without following redirects.
    Inputs: Cache directory and deadline. Outputs: Owned regular-file lock or refusal/timeout.
    """
    with contextlib.suppress(FileExistsError):
        directory.mkdir(parents=True, exist_ok=False)
    if directory.is_symlink() or directory.is_junction() or not directory.is_dir():
        raise ValueError("Crawler installation cache is missing, special or redirected")
    from tools.crawl4ai_download_opener import open_download

    deadline = time.monotonic() + timeout
    with contextlib.ExitStack() as opened:
        while True:
            try:
                lock = opened.enter_context(open(directory / ".install.lock", "a+b", opener=open_download))
                break
            except OSError as error:
                if not lock_contended(error):
                    raise
                if time.monotonic() >= deadline:
                    raise TimeoutError("Another crawler installation still owns this cache") from error
                time.sleep(0.05)
        if lock.tell() == 0:
            lock.write(b"0")
            lock.flush()
        if os.name == "nt":
            import msvcrt
        else:
            import fcntl
        while True:
            lock.seek(0)
            try:
                if os.name == "nt":
                    msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except OSError as error:
                if not lock_contended(error):
                    raise
                if time.monotonic() >= deadline:
                    raise TimeoutError("Another crawler installation still owns this cache") from error
                time.sleep(0.05)
        try:
            yield
        finally:
            lock.seek(0)
            if os.name == "nt":
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(lock.fileno(), fcntl.LOCK_UN)


def write_install_receipt(receipt: Path, recorded: dict) -> None:
    """Purpose: Publish qualified setup atomically without following a predictable alias.
    Inputs: Owned receipt path and admitted data. Outputs: Exclusively staged and replaced receipt.
    """
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="\n",
            prefix=receipt.name + ".",
            suffix=".tmp",
            dir=receipt.parent,
            delete=False,
        ) as stream:
            temporary = Path(stream.name)
            json.dump(recorded, stream, separators=(",", ":"))
        os.replace(temporary, receipt)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def provision(home: Path, directory: Path, python: Path, identity: str) -> Path:
    """Purpose: Set up an owned environment. Inputs: Cache/paths/lock identity. Outputs: Pinned interpreter."""
    env = tool_environment(home)
    receipt = directory / "flitzzip-install.json"
    model_contract = text_identity(ROOT / "tools/test_nltk_model_security.py")
    model_command = [str(python), "-B", "-m", "unittest", "tools.test_nltk_model_security"]
    download_contract = text_identity(ROOT / "tools/test_crawl4ai_downloads.py")
    download_command = [str(python), "-B", "-m", "unittest", "tools.test_crawl4ai_downloads"]
    if python.exists() and receipt.exists():
        recorded = json.loads(receipt.read_text(encoding="utf-8"))
        if recorded.get("lock_sha256") == identity and recorded.get("platform") == platform.system():
            verify_versions(python, env)
            if recorded.get("model_contract_sha256") != model_contract:
                run(model_command, env, 60, source_only=True)
                recorded["model_contract_sha256"] = model_contract
                write_install_receipt(receipt, recorded)
            if recorded.get("download_contract_sha256") != download_contract:
                run(download_command, env, 60, source_only=True)
                recorded["download_contract_sha256"] = download_contract
                write_install_receipt(receipt, recorded)
            return python
    if not python.exists():
        venv.EnvBuilder(with_pip=True).create(directory)
    from tools import crawl4ai_source_build, nltk_distribution

    nltk_wheel = nltk_distribution.wheel_path(home)
    crawler_wheel = crawl4ai_source_build.ensure_wheel(home)
    run(
        [
            str(python),
            "-m",
            "pip",
            "install",
            "--quiet",
            "--require-hashes",
            "--only-binary=:all:",
            "--find-links",
            str(nltk_wheel.parent),
            "--find-links",
            str(crawler_wheel.parent),
            "-r",
            str(LOCK),
        ],
        env,
    )
    run([str(python), "-m", "pip", "check"], env, 60)
    verify_versions(python, env)
    run(model_command, env, 60, source_only=True)
    run(download_command, env, 60, source_only=True)
    run([str(python), "-m", "playwright", "install", "chromium"], env)
    write_install_receipt(
        receipt,
        {
            "schema": 1,
            "version": VERSION,
            "lock_sha256": identity,
            "platform": platform.system(),
            "model_contract_sha256": model_contract,
            "download_contract_sha256": download_contract,
        },
    )
    return python


def install(home: Path) -> Path:
    """Purpose: Provision the pinned package/browser once. Inputs: Portable cache. Outputs: Isolated interpreter."""
    if not (3, 13) <= sys.version_info[:2] <= (3, 14):
        raise ValueError("Use CPython 3.13 or 3.14; this lock has not qualified other Python versions")
    directory, python, identity = environment_paths(home)
    with installation_lock(directory):
        return provision(home, directory, python, identity)


def check_lock(home: Path, requirements: Path = LOCK) -> None:
    """Purpose: Reject incompatible dependencies. Inputs: Cache/lock. Outputs: Resolver result; no runtime install."""
    from tools import crawl4ai_source_build, nltk_distribution

    if not (3, 13) <= sys.version_info[:2] <= (3, 14):
        raise ValueError("Use CPython 3.13 or 3.14; this lock has not qualified other Python versions")
    home.mkdir(parents=True, exist_ok=True)
    nltk_wheel = nltk_distribution.wheel_path(home)
    crawler = crawl4ai_source_build.ensure_wheel(home)
    with tempfile.TemporaryDirectory(prefix="lock-validation-", dir=home) as owned:
        directory = Path(owned)
        venv.EnvBuilder(with_pip=True).create(directory)
        python = directory / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
        run(
            [
                str(python),
                "-m",
                "pip",
                "install",
                "--dry-run",
                "--ignore-installed",
                "--require-hashes",
                "--only-binary=:all:",
                "--find-links",
                str(nltk_wheel.parent),
                "--find-links",
                str(crawler.parent),
                "-r",
                str(requirements),
            ],
            tool_environment(home),
        )
    print(json.dumps({"dependency_lock": "resolved", "lock_sha256": text_identity(requirements)}))


def main() -> int:
    """Purpose: Expose installer and crawler entry points. Inputs: CLI options. Outputs: Child exit status."""
    parser = argparse.ArgumentParser(description=__doc__, epilog=ATTRIBUTION)
    parser.add_argument("--home", help="Absolute external cache root; environment override: FLITZZIP_CRAWL4AI_HOME")
    parser.add_argument("command", choices=("install", "doctor", "crawl", "qualify", "check-lock"))
    parser.add_argument("arguments", nargs=argparse.REMAINDER, help="Arguments forwarded to the selected tool")
    args = parser.parse_args()
    remaining = args.arguments
    home = cache_home(args.home)
    env = tool_environment(home)
    if args.command == "check-lock":
        if remaining:
            parser.error("check-lock takes no additional arguments")
        check_lock(home)
        return 0
    if args.command == "install":
        if remaining:
            parser.error("install takes no additional arguments")
        python = install(home)
        print(json.dumps({"installed": VERSION, "python": str(python)}))
        return 0
    python = install(home)
    if args.command == "doctor":
        run([str(python), "-m", "pip", "check"], env, 60)
    if args.command == "crawl":
        command = [str(python), "-m", "crawl4ai.cli", "crawl", *remaining]
    else:
        command = [str(python), "-m", "tools.crawl4ai_research", args.command, *remaining]
    return run_source_python(command, env, timeout=1800)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"crawl4ai: {error}", file=sys.stderr)
        raise SystemExit(1) from error
