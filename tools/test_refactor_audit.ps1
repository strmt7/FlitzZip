$ErrorActionPreference = "Stop"
$auditScript = Join-Path $PSScriptRoot "refactor_audit.ps1"
$shell = (Get-Process -Id $PID).Path
$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$fixture = Join-Path $temporaryRoot ("flitzzip-refactor-audit-" + [Guid]::NewGuid().ToString("N"))
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

    $output = & git -C $fixture -c user.name=FlitzZip-Test -c user.email=tests@users.noreply.github.com -c commit.gpgsign=false -c core.hooksPath=NUL @GitArguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Fixture Git operation failed."
    }
    return $output
}

# Purpose: Exercise the real audit entry point in a separate, bounded PowerShell process.
# Inputs: Options is a fixed test suffix; the copied audit derives its root from the fixture.
# Outputs: Returns captured UTF-8 output and exit status without changing the parent native status.
function Invoke-AuditFixture {
    param([string]$Options = "", [int]$FunctionLineLimit = 0)

    $path = (Join-Path $fixture "tools/refactor_audit.ps1").Replace("'", "''")
    $command = "[Console]::OutputEncoding = [Text.UTF8Encoding]::new(`$false); & '$path' -MaxFunctionLines $FunctionLineLimit -FailOnFindings $Options"
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
    param([string]$Name, [string]$Options = "", [string[]]$Expected = @(),
        [string[]]$OtherFindings = @(), [int]$FunctionLineLimit = 0,
        [int[]]$ExpectedLines = @(), [switch]$ExpectGitFailure, [switch]$ExpectParameterFailure)

    ++$script:CaseCount
    $result = Invoke-AuditFixture -Options $Options -FunctionLineLimit $FunctionLineLimit
    if ($ExpectGitFailure) {
        $passed = $result.ExitCode -ne 0 -and $result.Output -notmatch 'status=clean' -and
            $result.Error -match 'comparison revision is unavailable'
    } elseif ($ExpectParameterFailure) {
        $passed = $result.ExitCode -ne 0 -and $result.Output -notmatch 'status=clean' -and
            $result.Error -match 'NamedParameterNotFound|CheckContracts'
    } else {
        $findingMatches = [regex]::Matches($result.Output, 'refactor_finding category=([^ ]+) path="([^"]+)" line=([0-9]+)')
        $actual = @($findingMatches | ForEach-Object { $_.Groups[1].Value + ':' + $_.Groups[2].Value.Replace('\', '/') } | Sort-Object)
        $expectedSorted = @(@($Expected | ForEach-Object { 'large-function:' + $_ }) + $OtherFindings | Sort-Object)
        $passed = ($result.ExitCode -eq $(if ($expectedSorted.Count) { 1 } else { 0 })) -and
            (($actual -join "`n") -ceq ($expectedSorted -join "`n")) -and
            ($result.Output -match 'refactor_audit status=')
        if ($ExpectedLines.Count) {
            $actualLines = @($findingMatches | ForEach-Object { [int]$_.Groups[3].Value } | Sort-Object)
            $passed = $passed -and (($actualLines -join ',') -ceq (($ExpectedLines | Sort-Object) -join ','))
        }
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
    Invoke-FixtureGit -GitArguments @("add", "--all") | Out-Null
    Invoke-FixtureGit -GitArguments @("commit", "--quiet", "-m", "fixture contract baseline") | Out-Null
    # A zero-line threshold makes each real function observable through the public
    # size gate. This tests discovery without treating comment labels as semantics.
    $vendorPath = 'third_party/sdk/parser.c'
    $syntaxCases = @(
        @{ Name = 'vendor C with next-line braces'; Code = @('static int parse(', '    int value)', '{', '    return value;', '}'); Count = 1 },
        @{ Name = 'plain adjacent documentation'; Code = @('/* Returns the caller-owned value without mutation. */', 'int parse(int value) {', '    return value;', '}'); Count = 1 },
        @{ Name = 'nested parameter parentheses'; Code = @('void parse(int count,', ' const std::function<void()>& callback) {', '    callback();', '}'); Count = 1 },
        @{ Name = 'template declaration'; Code = @('template <typename Stream>', 'void parse(Stream& input) {', '    input.read();', '}'); Count = 1 },
        @{ Name = 'constrained template'; Code = @('template <typename Number>', ' requires (sizeof(Number) == 8 || sizeof(Number) == 12)', 'int parse(Number value) {', '    return value;', '}'); Count = 1 },
        @{ Name = 'wrapped constraint'; Code = @('template <typename Number>', ' requires (sizeof(Number) == 8 ||', '           sizeof(Number) == 12)', 'int parse(Number value) {', '    return value;', '}'); Count = 1 },
        @{ Name = 'unparenthesized constraint'; Code = @('template <typename Number>', ' requires std::is_integral_v<Number> && std::is_unsigned_v<Number>', 'int parse(Number value) {', '    return value;', '}'); Count = 1 },
        @{ Name = 'concept requires expression is not a function'; Code = @('template <typename Number>', 'concept HasValue = requires(Number value) {', '    value.size();', '};'); Count = 0 },
        @{ Name = 'trailing boolean constraint'; Code = @('template <typename Number>', 'int parse(Number value) requires (sizeof(Number) > 1 && sizeof(Number) < 8) {', '    return value;', '}'); Count = 1 },
        @{ Name = 'wrapped trailing constraint'; Code = @('template <typename Number>', 'int parse(Number value)', ' requires (sizeof(Number) > 1 &&', '           sizeof(Number) < 8) {', '    return value;', '}'); Count = 1 },
        @{ Name = 'preceding declaration and constrained function'; Code = @('int unrelated;', 'template <typename Number>', 'int parse(Number value)', ' requires (sizeof(Number) > 1) {', '    return value;', '}'); Count = 1 },
        @{ Name = 'wrapped return type and parameters'; Code = @('std::string', 'parse(std::string_view input,', ' std::string_view suffix = "bounded fixture") {', '    return std::string(input) + std::string(suffix);', '}'); Count = 1 },
        @{ Name = 'class access label'; Code = @('class Owner {', 'public:', 'std::string', 'parse(int value) {', '    return {};', '}', '};'); Count = 1 },
        @{ Name = 'test macro and multiline conditions are not functions'; Code = @('TEST_CASE(registered_test) {', '    if (first_call() ||', '        second_call()) {', '        return;', '    }', '}'); Count = 0 },
        @{ Name = 'multiline C linkage'; Code = @('extern "C"', 'int parse(int first,', ' int second) {', '    return first + second;', '}'); Count = 1 },
        @{ Name = 'constructor initializer'; Code = @('Owner(int count)', ' : allocation(count) {', '    initialize();', '}'); Count = 1 },
        @{ Name = 'mixed constructor initializers'; Code = @('explicit Owner(int count)', ' : type{count, 0},', '   stream(create(count)),', '   reader(stream ? create(count) : nullptr) {', '    use(reader);', '}'); Count = 1 },
        @{ Name = 'constructor followed by empty function'; Code = @('Owner(int count) : type{count, 0}, stream(create(count)) {', '}', 'int next() {', '}'); Count = 2 },
        @{ Name = 'comments and literals cannot invent functions'; Code = @('int parse(void)', '{', '    const char *literal = "missing() { /* fake */ }";', '    const char *raw = R"tag(fake() { })tag";', '    /* imaginary() { } */', '    if (literal && raw) { return 1; }', '    return 0;', '}'); Count = 1 },
        @{ Name = 'attributes with operator brackets'; Code = @('[[nodiscard("see operator[]")]] constexpr std::optional<int>', 'bounded()', '{', '    return 0;', '}'); Count = 1 },
        @{ Name = 'wrapped attributes and return type'; Code = @('[[nodiscard]]', 'constexpr std::optional<int>', 'bounded()', '{', '    return 0;', '}'); Count = 1 },
        @{ Name = 'catch else and attributed controls'; Code = @('int controls() {', '    try { operation(); } catch (...) { recover(); }', '    [[likely]] if (first) { use(); } else if (second) { other(); }', '    else { finish(); }', '    return 0;', '}'); Count = 1 },
        @{ Name = 'independent adjacent functions'; Code = @('int previous() { return 0; }', 'int next() { return 1; }'); Count = 2 }
    )
    foreach ($case in $syntaxCases) {
        Write-AuditFixture -RelativePath $vendorPath -Lines $case.Code
        Test-AuditCase -Name $case.Name -Options '-ChangedOnly' -Expected (@($vendorPath) * $case.Count)
    }
    $shortFunction = @('int parse(void)', '{', '    return 1;', '}')
    Write-AuditFixture -RelativePath $vendorPath -Lines $shortFunction
    Test-AuditCase -Name 'clear private helper needs no template' -Options '-ChangedOnly' -FunctionLineLimit 120
    Test-AuditCase -Name 'removed comment-template flag fails explicitly' -Options '-CheckContracts' -ExpectParameterFailure

    $longBody = @('int parse(void)', '{', '    int value = 0;') + @('    ++value;') * 104 + @('    return value;', '}')
    Write-AuditFixture -RelativePath $vendorPath -Lines $longBody
    Test-AuditCase -Name 'comment density is not a correctness oracle' -Options '-ChangedOnly' -FunctionLineLimit 120
    Write-AuditFixture -RelativePath $vendorPath -Lines (@('int parse(void)', '{') + @('    operation();') * 124 + @('}'))
    Test-AuditCase -Name 'oversized vendor function still fails' -Options '-ChangedOnly' -FunctionLineLimit 120 -Expected @($vendorPath)
    Write-AuditFixture -RelativePath $vendorPath -Lines (@('int parse(void)', '{') + @('    if (ready && valid) { use(); }') * 20 + @('}'))
    Test-AuditCase -Name 'complex vendor function still fails' -Options '-ChangedOnly -MaxComplexityMarkers 35' -FunctionLineLimit 120 -OtherFindings @("complex-function:$vendorPath")
    Write-AuditFixture -RelativePath $vendorPath -Lines @('int parse(void) { ' + ('if (ready && valid) { use(); } ' * 20) + '}')
    Test-AuditCase -Name 'single-line functions cannot evade the complexity gate' -Options '-ChangedOnly -MaxComplexityMarkers 35' -FunctionLineLimit 120 -OtherFindings @("complex-function:$vendorPath")
    Write-AuditFixture -RelativePath $vendorPath -Lines (@('int parse(void)', '{') + @('    if (ready && valid) { use(); }') * 17 + @('}'))
    Test-AuditCase -Name 'below-threshold complexity remains accepted' -Options '-ChangedOnly -MaxComplexityMarkers 35' -FunctionLineLimit 120

    $documentation = @('/* Keeps the result independent of borrowed state.', ' * The caller retains ownership of all inputs. */')
    Write-AuditFixture -RelativePath $vendorPath -Lines ($documentation + $shortFunction)
    Invoke-FixtureGit -GitArguments @('add', '--', $vendorPath) | Out-Null
    Invoke-FixtureGit -GitArguments @('commit', '--quiet', '-m', 'fixture documentation baseline') | Out-Null
    Write-AuditFixture -RelativePath $vendorPath -Lines $shortFunction
    Test-AuditCase -Name 'complete documentation deletion still selects the function' -Options '-ChangedOnly' -Expected @($vendorPath) -ExpectedLines @(1)
    Write-AuditFixture -RelativePath $vendorPath -Lines (@('/* The caller retains ownership. */') + $shortFunction)
    Test-AuditCase -Name 'documentation edits still select their function' -Options '-ChangedOnly' -Expected @($vendorPath) -ExpectedLines @(1)
    Write-AuditFixture -RelativePath $vendorPath -Lines ($documentation + $shortFunction)
    $neighbor = 'third_party/sdk/neighbor.c'
    Write-AuditFixture -RelativePath $neighbor -Lines $shortFunction
    Invoke-FixtureGit -GitArguments @('add', '--', $neighbor) | Out-Null
    Invoke-FixtureGit -GitArguments @('commit', '--quiet', '-m', 'fixture inherited neighbor') | Out-Null
    Write-AuditFixture -RelativePath $neighbor -Lines (@('int added() { return 2; }', '') + $shortFunction)
    Test-AuditCase -Name 'added helper does not implicate unchanged neighbor' -Options '-ChangedOnly' -Expected @($neighbor) -ExpectedLines @(1)
    Invoke-FixtureGit -GitArguments @('add', '--', $neighbor) | Out-Null
    Invoke-FixtureGit -GitArguments @('commit', '--quiet', '-m', 'fixture additional helper') | Out-Null

    $header = 'src/attributed.hpp'
    Write-AuditFixture -RelativePath $header -Lines @('[[nodiscard]] int bounded()', '{', '    return 0;', '}')
    Test-AuditCase -Name 'inline header functions retain size auditing' -Options '-ChangedOnly' -Expected @($header)
    Invoke-FixtureGit -GitArguments @('add', '--', $header) | Out-Null
    Invoke-FixtureGit -GitArguments @('commit', '--quiet', '-m', 'fixture header') | Out-Null
    $modulePath = 'src/observer.psm1'
    $module = @('function Invoke-Observer {', '    return 1', '}')
    Write-AuditFixture -RelativePath $modulePath -Lines $module
    Test-AuditCase -Name 'PowerShell functions retain size auditing' -Options '-ChangedOnly' -Expected @($modulePath)
    Test-AuditCase -Name 'small PowerShell helpers need no template' -Options '-ChangedOnly' -FunctionLineLimit 120
    Write-AuditFixture -RelativePath $modulePath -Lines @('function Invoke-Observer {',
        '    $literal = "function Fake-Function { }"', '    # function Another-Fake { }', '    return $literal', '}')
    Test-AuditCase -Name 'PowerShell AST ignores fake string and comment declarations' -Options '-ChangedOnly' -Expected @($modulePath)
    Write-AuditFixture -RelativePath $modulePath -Lines (@('function Invoke-Observer {') + @('    Write-Output 1') * 124 + @('}'))
    Test-AuditCase -Name 'full audits retain PowerShell size findings' -FunctionLineLimit 120 -Expected @($modulePath)
    Write-AuditFixture -RelativePath $modulePath -Lines @('function Broken {')
    Test-AuditCase -Name 'malformed modules still fail' -Options '-ChangedOnly' -OtherFindings @("invalid-syntax:$modulePath")
    Write-AuditFixture -RelativePath $modulePath -Lines $module
} finally {
    if (Test-Path -LiteralPath $fixture) {
        $resolved = [IO.Path]::GetFullPath((Get-Item -LiteralPath $fixture).FullName)
        $prefix = $temporaryRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar + "flitzzip-refactor-audit-"
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
