$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$releaseDirectory = Join-Path $projectRoot 'build\Win32\Release'

& (Join-Path $projectRoot 'validate-workflow.ps1')

$tests = @(
    'ProxyExportsTest.exe',
    'MetadataSourceTest.exe',
    'RuntimePolicyTest.exe',
    'SharedMetadataTest.exe',
    'MediaFeaturesTest.exe',
    'SmtcHostTest.exe'
)

foreach ($test in $tests) {
    $testPath = Join-Path $releaseDirectory $test
    if (-not (Test-Path -LiteralPath $testPath)) {
        throw "Test executable not found: $testPath"
    }
    Write-Host "Running $test"
    & $testPath
    if ($LASTEXITCODE -ne 0) {
        throw "$test failed with exit code $LASTEXITCODE"
    }
}

$faultTest = Join-Path $releaseDirectory 'FaultSafetyTest.exe'
$faultFixture = Join-Path $projectRoot 'build\fault-fixtures\ui.dll'
foreach ($case in @('file-leak', 'module-leak', 'constructor', 'completion',
                    'completion-memory', 'cover-retry')) {
    $arguments = @($case)
    if ($case -eq 'file-leak') {
        $arguments += Join-Path $projectRoot ('build\temp\fault-' + [guid]::NewGuid() + '.bin')
    } elseif ($case -eq 'module-leak') {
        $arguments += $faultFixture
    }
    Write-Host "Running fault safety probe: $case"
    & $faultTest @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Fault safety probe $case failed with exit code $LASTEXITCODE"
    }
}

Write-Host 'All tests passed.'
