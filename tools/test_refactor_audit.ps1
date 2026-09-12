$ErrorActionPreference = "Stop"
$auditScript = Join-Path $PSScriptRoot "refactor_audit.ps1"
$shell = (Get-Process -Id $PID).Path
$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$fixture = Join-Path $temporaryRoot ("superzip-refactor-audit-" + [Guid]::NewGuid().ToString("N"))
$script:CaseCount = 0
$script:Failures = [System.Collections.Generic.List[string]]::new()

# Purpose: Create a bounded source fixture without depending on its host text encoding.
# Inputs: RelativePath stays under the test-owned fixture and Lines contains synthetic source.
# Outputs: Writes UTF-8 text only inside the fixture.
function Write-AuditFixture {
    param([string]$RelativePath, [string[]]$Lines)

    $path = Join-Path $fixture $RelativePath
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
    [IO.File]::WriteAllLines($path, $Lines, [Text.UTF8Encoding]::new($false))
}

# Purpose: Change only the isolated fixture's Git state with explicit synthetic commit identity.
# Inputs: GitArguments selects a fixed test operation; no remote or global configuration is used.
# Outputs: Returns Git output or throws on an unsuccessful command.
function Invoke-FixtureGit {
    param([string[]]$GitArguments)

    $output = & git -C $fixture -c user.name=SuperZip-Test -c user.email=tests@users.noreply.github.com -c commit.gpgsign=false -c core.hooksPath=NUL @GitArguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Fixture Git operation failed."
    }
    return $output
}

# Purpose: Exercise the real audit entry point in a separate, bounded PowerShell process.
# Inputs: Options is a fixed test suffix; the copied audit derives its root from the fixture.
# Outputs: Returns captured UTF-8 output and exit status without changing the parent native status.
function Invoke-AuditFixture {
    param([string]$Options = "")

    $path = (Join-Path $fixture "tools/refactor_audit.ps1").Replace("'", "''")
    $command = "[Console]::OutputEncoding = [Text.UTF8Encoding]::new(`$false); & '$path' -CheckContracts -FailOnFindings $Options"
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $shell
    $start.Arguments = "-NoProfile -NonInteractive -OutputFormat Text -ExecutionPolicy Bypass -EncodedCommand " + [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.Encoding]::UTF8
    $start.StandardErrorEncoding = [Text.Encoding]::UTF8
    $process = [Diagnostics.Process]::Start($start)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(30000)) {
            $process.Kill()
            throw "Refactor audit fixture exceeded its 30-second deadline."
        }
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Output = $stdout.GetAwaiter().GetResult()
            Error = $stderr.GetAwaiter().GetResult()
        }
    } finally {
        $process.Dispose()
    }
}

# Purpose: Check one entry-point scenario while retaining evidence for every failed scenario.
# Inputs: Name labels the case, Options selects the audit mode, and Expected names its exact findings.
# Outputs: Adds a bounded failure description or prints one passing case.
function Test-AuditCase {
    param([string]$Name, [string]$Options = "", [string[]]$Expected = @(), [switch]$ExpectGitFailure)

    ++$script:CaseCount
    $result = Invoke-AuditFixture -Options $Options
    if ($ExpectGitFailure) {
        $passed = $result.ExitCode -ne 0 -and $result.Output -notmatch 'status=clean' -and
            $result.Error -match 'comparison revision is unavailable'
    } else {
        $actual = @([regex]::Matches($result.Output, 'refactor_finding category=missing-contract path="([^"]+)"') | ForEach-Object { $_.Groups[1].Value.Replace('\', '/') } | Sort-Object)
        $expectedSorted = @($Expected | Sort-Object)
        $passed = ($result.ExitCode -eq $(if ($Expected.Count) { 1 } else { 0 })) -and
            (($actual -join "`n") -ceq ($expectedSorted -join "`n")) -and
            ($result.Output -match 'refactor_audit status=')
    }
    if (-not $passed) {
        $script:Failures.Add($Name)
        Write-Output "FAIL: $Name (exit=$($result.ExitCode))"
        $detail = ($result.Output + $result.Error).Replace($fixture, "[fixture]")
        Write-Output $detail.Substring(0, [Math]::Min(1600, $detail.Length))
    } else {
        Write-Output "PASS: $Name"
    }
}

try {
    if (Test-Path -LiteralPath $fixture) {
        throw "Refactor audit fixture already exists."
    }
    [IO.Directory]::CreateDirectory((Join-Path $fixture "tools")) | Out-Null
    Copy-Item -LiteralPath $auditScript -Destination (Join-Path $fixture "tools/refactor_audit.ps1")
    Invoke-FixtureGit -GitArguments @("init", "--quiet") | Out-Null
    Write-AuditFixture -RelativePath ".gitignore" -Lines @(".mirror/", "tools/", "out/")
    Write-AuditFixture -RelativePath "src/tracked.cpp" -Lines @("int tracked() {", "    return 1;", "}")
    Write-AuditFixture -RelativePath ".mirror/ignored.cpp" -Lines @("int ignored() {", "    return 1;", "}")
    Invoke-FixtureGit -GitArguments @("add", "--", ".gitignore", "src/tracked.cpp") | Out-Null
    Test-AuditCase -Name "unborn repository still audits source" -Options "-ChangedOnly" -Expected @("src/tracked.cpp")
    Invoke-FixtureGit -GitArguments @("commit", "--quiet", "-m", "fixture baseline") | Out-Null
    Test-AuditCase -Name "full inventory ignores an untracked mirror" -Expected @("src/tracked.cpp")
    $unicodePath = "src/untracked " + [char]0x00E9 + ".cpp"
    Write-AuditFixture -RelativePath $unicodePath -Lines @("int untracked() {", "    return 1;", "}")
    Test-AuditCase -Name "full inventory includes untracked Unicode paths" -Expected @("src/tracked.cpp", $unicodePath)
    Test-AuditCase -Name "changed inventory includes untracked source" -Options "-ChangedOnly" -Expected @($unicodePath)
    Write-AuditFixture -RelativePath "src/tracked.cpp" -Lines @("int tracked() {", "    return 2;", "}")
    Test-AuditCase -Name "changed inventory includes unstaged source" -Options "-ChangedOnly" -Expected @("src/tracked.cpp", $unicodePath)
    Invoke-FixtureGit -GitArguments @("add", "--", "src/tracked.cpp") | Out-Null
    Invoke-FixtureGit -GitArguments @("mv", "--", "src/tracked.cpp", "src/renamed file.cpp") | Out-Null
    Test-AuditCase -Name "changed inventory includes staged renames" -Options "-ChangedOnly" -Expected @("src/renamed file.cpp", $unicodePath)
    Invoke-FixtureGit -GitArguments @("add", "-f", "--", ".mirror/ignored.cpp") | Out-Null
    Test-AuditCase -Name "tracked source is not hidden by ignore rules" -Expected @(".mirror/ignored.cpp", "src/renamed file.cpp", $unicodePath)
    Invoke-FixtureGit -GitArguments @("commit", "--quiet", "-m", "fixture rename") | Out-Null
    Invoke-FixtureGit -GitArguments @("rm", "--quiet", "--", "src/renamed file.cpp") | Out-Null
    Test-AuditCase -Name "deleted paths do not hide remaining additions" -Options "-ChangedOnly" -Expected @($unicodePath)
    Test-AuditCase -Name "invalid explicit Git base fails closed" -Options "-ChangedOnly -GitBase refs/heads/no-such-audit-base" -ExpectGitFailure
} finally {
    if (Test-Path -LiteralPath $fixture) {
        $resolved = [IO.Path]::GetFullPath((Get-Item -LiteralPath $fixture).FullName)
        $prefix = $temporaryRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar + "superzip-refactor-audit-"
        if (-not $resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or $resolved -cne $fixture) {
            throw "Refusing cleanup outside the exact test-owned fixture."
        }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
if ($script:Failures.Count) {
    throw "$($script:Failures.Count)/$script:CaseCount refactor audit cases failed: $($script:Failures -join ', ')"
}
Write-Output "$script:CaseCount refactor audit entry-point cases passed."
