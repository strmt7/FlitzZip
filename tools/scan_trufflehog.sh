#!/usr/bin/env bash
set -euo pipefail

# Scanner diagnostics can contain matching input. Publish only safe exit codes and allowlisted findings.
# GitHub invokes Bash with errexit; suspend it only until both pipeline statuses have been captured.
set +e
scanner_image='trufflesecurity/trufflehog:3.97.4@sha256:562bc231afa9de3d04de44cfe624252b08207de1fc3cebc5e7ed92bed7f279e4'
docker run --rm \
  -v "$PWD:/repo:ro" \
  "$scanner_image" \
  git file:///repo --results=verified,unknown --fail --fail-on-scan-errors --no-update --json \
  2>/dev/null | python tools/redact_trufflehog.py --scanner-image "$scanner_image" \
    --review-report reports/secrets/trufflehog-review.json > reports/secrets/trufflehog-git.jsonl
statuses=("${PIPESTATUS[@]}")
set -e
printf 'TruffleHog exit code: %s; report redaction exit code: %s\n' "${statuses[0]}" "${statuses[1]}"
# Redactor status 4 means every retained finding matched an exact approved public fixture.
# Status 3 is unresolved; 2 is malformed/unreadable. A scanner error can never pass.
if (( statuses[0] == 0 && statuses[1] == 0 )) || (( statuses[0] == 183 && statuses[1] == 4 )); then
  exit 0
fi
exit 1
