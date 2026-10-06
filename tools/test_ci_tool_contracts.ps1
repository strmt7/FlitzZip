$ErrorActionPreference = 'Stop'
$runner = Join-Path $PSScriptRoot 'ci_tool_contracts.ps1'

# Purpose: Check the actual CI projection using representative classified paths without executing its commands.
# Inputs: Changed path and the required offline mechanism ids. Outputs: Fails on omissions or product admission.
function Assert-CiContractPlan {
    param([string]$Path, [string[]]$Required)
    $json = @(& $runner -ChangedPath $Path -PlanOnly) -join [Environment]::NewLine
    $parsed = ConvertFrom-Json -InputObject $json
    $commands = @($parsed)
    $ids = @($commands | ForEach-Object { $_.id })
    foreach ($id in $Required) {
        if ($id -notin $ids) { throw "CI projection lost $id for $Path" }
    }
    foreach ($id in @('release-build', 'unit-tests', 'gui-smoke', 'package-smoke',
            'short-fuzz-smoke', 'ram-benchmark-sweep', 'security-scan', 'language-lint', 'zstd-sanitizer-tests')) {
        if ($id -in $ids) { throw "CI tool lane admitted unrelated product command $id for $Path" }
    }
    if (@($ids | Sort-Object -Unique).Count -ne $ids.Count) {
        throw "CI projection repeats a contract for $Path"
    }
}

Assert-CiContractPlan -Path 'tools/cocoindex_agent_search.py' -Required @('agent-context-contracts')
Assert-CiContractPlan -Path 'mcp/superzip_mcp.py' -Required @('mcp-python-compile', 'mcp-bounded-child-tests')
Assert-CiContractPlan -Path 'tools/test_native_build_receipt.py' -Required @('native-build-receipt-tests')
Assert-CiContractPlan -Path 'tools/build.ps1' -Required @('native-build-receipt-tests', 'rocm-toolchain-tests')
Assert-CiContractPlan -Path 'cmake/SuperZipPrecompiledHeaders.cmake' -Required @('precompiled-header-tests')
Assert-CiContractPlan -Path 'tests/cmake/precompiled_headers/contract.c' -Required @('precompiled-header-tests')
Assert-CiContractPlan -Path 'src/core/checksum.cpp' -Required @()
Assert-CiContractPlan -Path 'tools/verify_changes.ps1' -Required @('verification-runner-tests')
Assert-CiContractPlan -Path 'tools/security_scan.ps1' -Required @('security-scan-tests')
Assert-CiContractPlan -Path 'tools/test_security_scan.ps1' -Required @('security-scan-tests')
Assert-CiContractPlan -Path 'tools/native_component_tests.ps1' -Required @('native-selection-contracts')
Assert-CiContractPlan -Path 'tests/cpp/test_main.cpp' -Required @('native-runner-contracts')
Assert-CiContractPlan -Path 'tools/native_ci.ps1' -Required @('native-ci-contracts')
Assert-CiContractPlan -Path '.github/workflows/rocm-qualification.yml' -Required @('native-ci-contracts', 'rocm-toolchain-tests')
Assert-CiContractPlan -Path '.github/workflows/release.yml' -Required @('release-workflow-contracts')
Assert-CiContractPlan -Path 'cmake/ZstdLegacyHistoryV05.c' -Required @('zstd-rewrite-policy-contracts')
Assert-CiContractPlan -Path '.github/scanner-source-reviews.csv' -Required @('scanner-metadata-review-tests')
Assert-CiContractPlan -Path '.github/scanner-governance-baseline.json' -Required @('github-post-push-audit-tests')
Assert-CiContractPlan -Path '.github/scanner-secret-reviews.json' -Required @('secret-report-tests')
Assert-CiContractPlan -Path '.github/scanner-secret-reviews-crawl4ai.json' -Required @('secret-report-tests')
Assert-CiContractPlan -Path '.github/scanner-secret-reviews-crawl4ai-tests.json' -Required @('secret-report-tests')
Assert-CiContractPlan -Path 'third_party/upstream/nltk/source.zip' -Required @('secret-report-tests')
Assert-CiContractPlan -Path 'tools/test_memory_benchmark_corpus.ps1' -Required @('binary-corpus-transport-tests')
Assert-CiContractPlan -Path 'tools/cpp_security_plan.py' -Required @('cpp-security-plan-tests')
Assert-CiContractPlan -Path 'tools/crawl4ai_download_opener.py' -Required @('crawl4ai-source-contracts', 'crawl4ai-contracts')
Assert-CiContractPlan -Path 'third_party/upstream/crawl4ai/0.9.4/build.json' -Required @('crawl4ai-source-contracts')
Assert-CiContractPlan -Path '.github/workflows/security-code-scanning.yml' -Required @('cpp-security-plan-tests')

# Load the actual production comparison helper without executing any selected command.
. $runner -ChangedPath 'README.md' -PlanOnly | Out-Null
$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ("superzip-ci-ancestry-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
try {
    & git -C $fixtureRoot init --quiet
    & git -C $fixtureRoot config user.name 'Contract Fixture'
    & git -C $fixtureRoot config user.email 'fixture@example.invalid'
    Set-Content -LiteralPath (Join-Path $fixtureRoot 'SECURITY.md') -Value 'Inherited security policy'
    & git -C $fixtureRoot add SECURITY.md
    & git -C $fixtureRoot commit --quiet -m 'Default branch fixture'
    & git -C $fixtureRoot update-ref refs/remotes/origin/main HEAD
    $empty = @(Get-SuperZipInitialPushPath -Root $fixtureRoot -DefaultBranch main)
    if ($empty.Count) { throw 'An unchanged initial branch selected inherited files.' }
    New-Item -ItemType Directory -Path (Join-Path $fixtureRoot 'tools/requirements') | Out-Null
    Set-Content -LiteralPath (Join-Path $fixtureRoot 'tools/requirements/crawl4ai.txt') -Value 'fixture==1.0'
    & git -C $fixtureRoot add tools/requirements/crawl4ai.txt
    & git -C $fixtureRoot commit --quiet -m 'Dependency change fixture'
    $paths = @(Get-SuperZipInitialPushPath -Root $fixtureRoot -DefaultBranch main)
    if ($paths.Count -ne 1 -or $paths[0] -ne 'tools/requirements/crawl4ai.txt') {
        throw 'Initial-push comparison lost the change or admitted inherited files.'
    }
    foreach ($branch in @('missing', 'main:invalid', '-invalid', '')) {
        $rejected = $false
        try { Get-SuperZipInitialPushPath -Root $fixtureRoot -DefaultBranch $branch 2>$null | Out-Null } catch { $rejected = $true }
        if (-not $rejected) { throw 'Initial-push comparison admitted missing or invalid ancestry.' }
    }
} finally {
    $resolvedFixture = [IO.Path]::GetFullPath($fixtureRoot)
    $temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if (-not $resolvedFixture.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolvedFixture) -notlike 'superzip-ci-ancestry-*') { throw 'Unsafe fixture cleanup path.' }
    Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
}
Write-Output 'CI component projection inclusion/exclusion contracts passed.'
