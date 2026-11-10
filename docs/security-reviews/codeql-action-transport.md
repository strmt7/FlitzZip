# CodeQL Action transport review

The security and Scorecard workflows pin the official CodeQL Action v4.39.3
commit, `33d7b85da1a04171e211befe2c739303b9a4c066`. GitHub verifies the
commit signature. The upgrade replaces the immutable action reference in
`init`, `analyze`, and `upload-sarif`. [Official release](https://github.com/github/codeql-action/releases/tag/v4.39.3)

The two releases declare the same inputs, outputs, Node runtime, and
entry points for these three actions. The v4.39.3 default bundle is CodeQL
CLI 2.27.4. The preceding action already selected that bundle in the hosted
Windows CPU run. Fresh analysis must establish the tool and coverage used
for the upgraded workflow. [Hosted baseline](https://github.com/strmt7/FlitzZip/actions/runs/42915526432/job/130177407730)

The C++ jobs still trace complete Windows CPU and HIP-host builds under
their distinct categories. The added upstream tracer compatibility check
applies to macOS and Swift. The workflows run on push, schedule and manual
dispatch. Upstream changes to pull-request diff filtering do not change
the configured whole-database analysis. The raw SARIF retention steps and
their order are unchanged.

DevSkim and Semgrep still use their existing scanner pins, inputs,
rules, findings gates and raw report artifacts. Their upload step keeps
the same SARIF input and category. Scorecard uses its existing action and
artifact. The upload transport update changes neither the accepted source
finding coordinates nor the existing governance observations.

The exact-source ledger carries the original individual review for each
finding. The renewed contexts bind the changed workflow bytes and this
transport review. A changed report identity, scanner version, analysis
coverage or missing hosted result still requires investigation. This
review qualifies the workflow diff and action interface. The exact pushed
revision must still pass hosted analysis and the findings audit.
