$ErrorActionPreference = 'Stop'

# MSBuild's legacy process launcher treats Path/PATH as duplicate keys even
# though Windows environment-variable lookup is case-insensitive. Remember the
# effective value here; each compiler process is launched with a freshly
# normalized environment below.
$buildPath = $env:PATH

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$taskTemp = Join-Path $projectRoot 'build\temp'
New-Item -ItemType Directory -Path $taskTemp -Force | Out-Null
$env:TEMP = $taskTemp
$env:TMP = $taskTemp

$vswhere = Join-Path ${env:ProgramFiles(x86)} `
    'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudioRoot = $null
$msbuild = $null
if (Test-Path -LiteralPath $vswhere) {
    $visualStudioRoot = & $vswhere -latest -products '*' `
        -requires Microsoft.Component.MSBuild -property installationPath |
        Select-Object -First 1
    $msbuild = & $vswhere -latest -products '*' `
        -requires Microsoft.Component.MSBuild `
        -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
}
if ([string]::IsNullOrWhiteSpace($msbuild) -or
    -not (Test-Path -LiteralPath $msbuild)) {
    throw 'MSBuild was not found. Install Visual Studio 2022 C++ Build Tools.'
}

$cmakeCommand = Get-Command 'cmake.exe' -ErrorAction SilentlyContinue
$cmake = if ($null -ne $cmakeCommand) { $cmakeCommand.Source } else { $null }
if ([string]::IsNullOrWhiteSpace($cmake) -and
    -not [string]::IsNullOrWhiteSpace($visualStudioRoot)) {
    $bundledCmake = Join-Path $visualStudioRoot `
        'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (Test-Path -LiteralPath $bundledCmake) {
        $cmake = $bundledCmake
    }
}
if ([string]::IsNullOrWhiteSpace($cmake) -or
    -not (Test-Path -LiteralPath $cmake)) {
    throw 'CMake was not found. Install CMake or the Visual Studio CMake component.'
}

function Invoke-NativeBuildTool {
    param(
        [Parameter(Mandatory)][string]$Executable,
        [Parameter(Mandatory)][string[]]$Arguments
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Executable
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($key in @($startInfo.Environment.Keys)) {
        if ($key -ieq 'Path') {
            [void]$startInfo.Environment.Remove($key)
        }
    }
    $startInfo.Environment['Path'] = $buildPath
    foreach ($argument in $Arguments) {
        $startInfo.ArgumentList.Add($argument)
    }

    $process = [System.Diagnostics.Process]::Start($startInfo)
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    if (-not [string]::IsNullOrWhiteSpace($stdout)) {
        [Console]::Out.Write($stdout)
    }
    if (-not [string]::IsNullOrWhiteSpace($stderr)) {
        [Console]::Error.Write($stderr)
    }
    if ($process.ExitCode -ne 0) {
        throw "$Executable failed with exit code $($process.ExitCode)"
    }
}

function Invoke-ProjectBuild {
    param([Parameter(Mandatory)][string]$Project)

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $msbuild
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($key in @($startInfo.Environment.Keys)) {
        if ($key -ieq 'Path') {
            [void]$startInfo.Environment.Remove($key)
        }
    }
    $startInfo.Environment['Path'] = $buildPath
    $startInfo.ArgumentList.Add($Project)
    $startInfo.ArgumentList.Add('/m:1')
    $startInfo.ArgumentList.Add('/nr:false')
    $startInfo.ArgumentList.Add('/p:Configuration=Release')
    $startInfo.ArgumentList.Add('/p:Platform=Win32')
    $startInfo.ArgumentList.Add('/v:minimal')

    $process = [System.Diagnostics.Process]::Start($startInfo)
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    if (-not [string]::IsNullOrWhiteSpace($stdout)) {
        [Console]::Out.Write($stdout)
    }
    if (-not [string]::IsNullOrWhiteSpace($stderr)) {
        [Console]::Error.Write($stderr)
    }
    if ($process.ExitCode -ne 0) {
        throw "MSBuild failed for $Project with exit code $($process.ExitCode)"
    }
}

$releaseDirectory = Join-Path $projectRoot 'build\Win32\Release'
New-Item -ItemType Directory -Path $releaseDirectory -Force | Out-Null
$legacyForwardingDll = Join-Path $releaseDirectory 'version_original.dll'
if (Test-Path -LiteralPath $legacyForwardingDll) {
    Remove-Item -LiteralPath $legacyForwardingDll -Force
}

$webpSource = Join-Path $projectRoot 'third_party\libwebp'
$webpBuild = Join-Path $webpSource 'build-win32'
if (-not (Test-Path -LiteralPath (Join-Path $webpBuild 'CMakeCache.txt'))) {
    Invoke-NativeBuildTool $cmake @(
        '-S', $webpSource,
        '-B', $webpBuild,
        '-G', 'Visual Studio 17 2022',
        '-A', 'Win32',
        '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
        '-DWEBP_BUILD_ANIM_UTILS=OFF',
        '-DWEBP_BUILD_CWEBP=OFF',
        '-DWEBP_BUILD_DWEBP=OFF',
        '-DWEBP_BUILD_GIF2WEBP=OFF',
        '-DWEBP_BUILD_IMG2WEBP=OFF',
        '-DWEBP_BUILD_VWEBP=OFF',
        '-DWEBP_BUILD_WEBPINFO=OFF',
        '-DWEBP_BUILD_LIBWEBPMUX=OFF',
        '-DWEBP_BUILD_WEBPMUX=OFF',
        '-DWEBP_BUILD_EXTRAS=OFF'
    )
}
Invoke-NativeBuildTool $cmake @(
    '--build', $webpBuild,
    '--config', 'Release',
    '--target', 'webpdecoder',
    '--', '/m:1'
)

Invoke-ProjectBuild (Join-Path $projectRoot 'KuGouSmtcPlugin.vcxproj')
Invoke-ProjectBuild (Join-Path $projectRoot 'ProxyExportsTest.vcxproj')
Invoke-ProjectBuild (Join-Path $projectRoot 'SmtcHostTest.vcxproj')
Invoke-ProjectBuild (Join-Path $projectRoot 'MetadataSourceTest.vcxproj')
Invoke-ProjectBuild (Join-Path $projectRoot 'RuntimePolicyTest.vcxproj')
Invoke-ProjectBuild (Join-Path $projectRoot 'SharedMetadataTest.vcxproj')
Invoke-ProjectBuild (Join-Path $projectRoot 'MediaFeaturesTest.vcxproj')
