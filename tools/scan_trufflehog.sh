#!/usr/bin/env bash
set -euo pipefail

# Scanner diagnostics can contain matching input. Publish only safe exit codes and allowlisted findings.
# GitHub invokes Bash with errexit; suspend it only until both pipeline statuses have been captured.
set +e
docker run --rm \
  -v "$PWD:/repo:ro" \
  trufflesecurity/trufflehog:3.97.4@sha256:562bc231afa9de3d04de44cfe624252b08207de1fc3cebc5e7ed92bed7f279e4 \
  git file:///repo --results=verified,unknown --fail --fail-on-scan-errors --no-update --json \
  2>/dev/null | python tools/redact_trufflehog.py > reports/secrets/trufflehog-git.jsonl
statuses=("${PIPESTATUS[@]}")
set -e
printf 'TruffleHog exit code: %s; report redaction exit code: %s\n' "${statuses[0]}" "${statuses[1]}"
if (( statuses[0] != 0 || statuses[1] != 0 )) || [[ -s reports/secrets/trufflehog-git.jsonl ]]; then
  exit 1
fi
