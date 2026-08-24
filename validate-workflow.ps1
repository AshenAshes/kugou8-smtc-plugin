$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$workflowPath = Join-Path $projectRoot '.github\workflows\build.yml'
$workflow = Get-Content -LiteralPath $workflowPath -Raw

$usesDirectArtifact =
    $workflow -match 'uses:\s*actions/upload-artifact@v7' -and
    $workflow -match 'archive:\s*false'
if ($usesDirectArtifact -and
    $workflow -notmatch 'uses:\s*actions/download-artifact@v8') {
    throw 'Direct artifacts uploaded by upload-artifact v7 require download-artifact v8 or newer.'
}
if ($workflow -match 'gh release create' -and
    $workflow -notmatch 'GH_REPO:\s*\$\{\{ github\.repository \}\}') {
    throw 'A release job without a checkout must provide GH_REPO explicitly.'
}

Write-Host 'Workflow artifact contract is valid.'
