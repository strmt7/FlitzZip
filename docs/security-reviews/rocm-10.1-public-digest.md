# ROCm 10.1.0 Public Archive Digest Review

## Individual Decision

### Alert 8451

DevSkim `DS173237` reports `tools/rocm-sdk-lock.json:5`, columns 12–78, at
`f333e5fc04f8c5fba2631d446d2bf8422c8c7b84`. The value is the SHA-256 of
AMD's public `therock-dist-windows-multiarch-10.1.0.tar.gz` distribution at the
official HTTPS URL in the same lock. It was computed from the complete downloaded
archive and is explicitly marked as locally observed, not an AMD-published
checksum. `tools/bootstrap_rocm_sdk.py` compares the archive length and digest
before extraction. This value grants no access and is not a credential.

This decision covers only the reported source bytes and location. The current
review ledger binds the source, bootstrap implementation, scanner configuration
and this evidence by digest. Raw DevSkim output remains available; any changed
source or context, different rule, or additional finding remains blocking.
