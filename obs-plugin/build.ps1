[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $ObsRoot,
    [Parameter(Mandatory = $true)]
    [string] $ObsSourceDir,
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo',
    [ValidateSet('Ninja', 'Visual Studio 17 2022')]
    [string] $Generator = 'Visual Studio 17 2022'
)

$ErrorActionPreference = 'Stop'

$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
if (-not $cmake) {
    $localTools = Join-Path $PSScriptRoot 'work\tools'
    $localCmake = if (Test-Path -LiteralPath $localTools -PathType Container) {
        Get-ChildItem -LiteralPath $localTools -Filter 'cmake.exe' -File -Recurse |
            Select-Object -First 1 -ExpandProperty FullName
    }
    if ($localCmake) {
        $cmake = [pscustomobject]@{ Source = $localCmake }
    } else {
        throw 'cmake.exe is required. Install CMake 3.28+ or put it on PATH.'
    }
}

$vsDevCmd = Get-ChildItem -Path 'C:\Program Files\Microsoft Visual Studio' -Filter 'VsDevCmd.bat' -File -Recurse |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $vsDevCmd) {
    throw 'VsDevCmd.bat was not found. Visual Studio 2022 C++ tools are required.'
}
$callerPath = $env:Path
$devEnvironment = cmd.exe /d /s /c "call `"$vsDevCmd`" -arch=x64 -host_arch=x64 && set"
foreach ($variable in @('INCLUDE', 'LIB', 'LIBPATH', 'VCToolsInstallDir', 'WindowsSdkDir', 'WindowsSDKVersion', 'UniversalCRTSdkDir', 'UCRTVersion')) {
    $line = $devEnvironment | Where-Object { $_ -clike "$variable=*" } | Select-Object -First 1
    if ($line) {
        Set-Item -Path ('Env:' + $variable) -Value $line.Substring($variable.Length + 1)
    }
}
$pathLine = $devEnvironment | Where-Object { $_ -clike 'PATH=*' } | Select-Object -First 1
if ($pathLine) {
    $env:Path = $pathLine.Substring(5) + ';' + $callerPath
}
$clPath = Get-ChildItem -Path 'C:\Program Files\Microsoft Visual Studio' -Filter 'cl.exe' -File -Recurse |
    Where-Object { $_.FullName -like '*Hostx64*x64*' } |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $clPath) {
    throw 'The x64 MSVC compiler was not found.'
}
$env:Path = (Split-Path -Parent $clPath) + ';' + $env:Path
if ($Generator -eq 'Ninja') {
    $ninja = Get-Command ninja.exe -ErrorAction SilentlyContinue
    if (-not $ninja) {
        throw 'ninja.exe is required for the Ninja generator. Install Ninja or select Visual Studio 17 2022.'
    }
}

function Invoke-CMake {
    param([string[]] $Arguments)

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $cmake.Source
    $startInfo.UseShellExecute = $false

    $seen = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in [Environment]::GetEnvironmentVariables('Process').GetEnumerator()) {
        $name = [string]$entry.Key
        if ($name -ieq 'PATH') {
            continue
        }
        if ($seen.Add($name)) {
            $startInfo.Environment[$name] = [string]$entry.Value
        }
    }
    $startInfo.Environment['Path'] = $env:Path
    foreach ($argument in $Arguments) {
        $startInfo.ArgumentList.Add($argument)
    }

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    [void]$process.Start()
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) {
        throw "CMake failed with exit code $($process.ExitCode)."
    }
}

$obsDll = Join-Path $ObsRoot 'bin\64bit\obs.dll'
if (-not (Test-Path -LiteralPath $obsDll -PathType Leaf)) {
    throw "Expected OBS 64-bit runtime was not found: $obsDll"
}
if (-not (Test-Path -LiteralPath (Join-Path $ObsSourceDir 'libobs\obs.h') -PathType Leaf)) {
    throw "Matching OBS source headers were not found: $ObsSourceDir"
}

$importDirectory = Join-Path $PSScriptRoot 'work\obs-sdk'
& (Join-Path $PSScriptRoot 'tools\make-obs-import-lib.ps1') `
    -ObsDll $obsDll `
    -OutputDirectory $importDirectory
if ($LASTEXITCODE -ne 0) { throw 'Creating the OBS import library failed.' }

$buildDirectory = Join-Path $PSScriptRoot $(if ($Generator -eq 'Ninja') { 'build\ninja' } else { 'build\vs2022' })
$installDirectory = Join-Path $PSScriptRoot 'outputs\install\alvr-capture'
$obsImportLib = Join-Path $importDirectory 'obs.lib'
$obsSourceCmake = $ObsSourceDir.Replace('\', '/')
$obsImportCmake = $obsImportLib.Replace('\', '/')
$installCmake = $installDirectory.Replace('\', '/')
$clCmake = $clPath.Replace('\', '/')

$configureArguments = @(
    '-S', $PSScriptRoot,
    '-B', $buildDirectory,
    '-G', $Generator,
    "-DOBS_SOURCE_DIR=$obsSourceCmake",
    "-DOBS_IMPORT_LIB=$obsImportCmake",
    "-DCMAKE_INSTALL_PREFIX=$installCmake",
    "-DCMAKE_CXX_COMPILER=$clCmake"
)
if ($Generator -eq 'Ninja') {
    $configureArguments += "-DCMAKE_BUILD_TYPE=$Configuration"
} else {
    $configureArguments += '-A', 'x64', '-DCMAKE_VS_GLOBALS=TrackFileAccess=false'
}
Invoke-CMake $configureArguments

if ($Generator -eq 'Ninja') {
    Invoke-CMake @('--build', $buildDirectory, '--parallel')
    Invoke-CMake @('--install', $buildDirectory)
} else {
    Invoke-CMake @('--build', $buildDirectory, '--config', $Configuration, '--parallel')
    Invoke-CMake @('--install', $buildDirectory, '--config', $Configuration)
}

Get-Item -LiteralPath (Join-Path $installDirectory 'obs-plugins\64bit\alvr-capture.dll') |
    Select-Object FullName, Length, LastWriteTime
