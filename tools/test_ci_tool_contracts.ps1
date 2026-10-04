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
Assert-CiContractPlan -Path 'src/core/checksum.cpp' -Required @()
Assert-CiContractPlan -Path 'tools/verify_changes.ps1' -Required @('verification-runner-tests')
Assert-CiContractPlan -Path 'tools/native_component_tests.ps1' -Required @('native-selection-contracts')
Assert-CiContractPlan -Path 'tests/cpp/test_main.cpp' -Required @('native-runner-contracts')
Assert-CiContractPlan -Path 'tools/native_ci.ps1' -Required @('native-ci-contracts')
Assert-CiContractPlan -Path '.github/workflows/rocm-qualification.yml' -Required @('native-ci-contracts', 'rocm-toolchain-tests')
Assert-CiContractPlan -Path '.github/workflows/release.yml' -Required @('release-workflow-contracts')
Assert-CiContractPlan -Path 'cmake/ZstdLegacyHistoryV05.c' -Required @('zstd-rewrite-policy-contracts')
Assert-CiContractPlan -Path '.github/scanner-source-reviews.csv' -Required @('scanner-metadata-review-tests')
Write-Output 'CI component projection inclusion/exclusion contracts passed.'
