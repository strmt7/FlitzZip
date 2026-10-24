# ROCm Redirect Rejection Fixture Review

## Individual Decision

### Alert 8432

DevSkim `DS137138` reports the literal HTTP destination in
`tools/test_bootstrap_rocm_sdk.py`. The current report identifies line 304,
columns 26 through 48. This is a rejection fixture, not a download endpoint.

`test_redirects_are_not_silently_followed` calls the production
`NoRedirect.redirect_request` directly inside an exception assertion. That method
rejects redirects without making a request to the supplied destination. The
test cannot follow this URL, and the real downloader separately requires HTTPS,
the admitted provenance and exact archive bytes. Changing the fixture to HTTPS
would remove its explicit insecure-destination control.

The fresh report, complete source, production redirect handler and scanner
configuration have been reviewed individually. Its source and exact location
match the current negative control, which passed in the bootstrap contract suite.
The earlier alert number is not reused as authorization for a new report.

Under the maintainer's standing authorization for individually proven false
positives, the current ledger binds this report to those exact identities and
this review. Raw reports remain retained. Any changed identity requires another
review, and every unmatched finding remains blocking.
