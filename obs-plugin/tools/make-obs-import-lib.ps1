[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $ObsDll,

    [Parameter(Mandatory = $true)]
    [string] $OutputDirectory,

    [string] $Dumpbin,
    [string] $LibTool
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $ObsDll -PathType Leaf)) {
    throw "OBS DLL not found: $ObsDll"
}

if (-not $Dumpbin) {
    $Dumpbin = (Get-ChildItem -Path 'C:\Program Files\Microsoft Visual Studio' -Filter 'dumpbin.exe' -File -Recurse |
        Where-Object { $_.FullName -like '*Hostx64*x64*' } |
        Select-Object -First 1 -ExpandProperty FullName)
}
if (-not $LibTool) {
    $LibTool = (Get-ChildItem -Path 'C:\Program Files\Microsoft Visual Studio' -Filter 'lib.exe' -File -Recurse |
        Where-Object { $_.FullName -like '*Hostx64*x64*' } |
        Select-Object -First 1 -ExpandProperty FullName)
}
if (-not $Dumpbin -or -not (Test-Path -LiteralPath $Dumpbin)) { throw 'dumpbin.exe was not found.' }
if (-not $LibTool -or -not (Test-Path -LiteralPath $LibTool)) { throw 'lib.exe was not found.' }

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$exportsPath = Join-Path $OutputDirectory 'obs-exports.txt'
$defPath = Join-Path $OutputDirectory 'obs.def'
$libPath = Join-Path $OutputDirectory 'obs.lib'

& $Dumpbin /nologo /exports $ObsDll | Set-Content -LiteralPath $exportsPath -Encoding ASCII
if ($LASTEXITCODE -ne 0) { throw "dumpbin.exe failed with exit code $LASTEXITCODE." }

$names = foreach ($line in Get-Content -LiteralPath $exportsPath) {
    if ($line -match '^\s*\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+?)(?:\s+=\s+\S+)?\s*$') {
        $matches[1]
    }
}
$names = $names | Sort-Object -Unique
if (-not $names -or $names.Count -lt 20) { throw 'The OBS DLL export list was unexpectedly small.' }

$defLines = @('LIBRARY obs.dll', 'EXPORTS') + ($names | ForEach-Object { "    $_" })
Set-Content -LiteralPath $defPath -Value $defLines -Encoding ASCII

& $LibTool /nologo /def:$defPath /machine:x64 /out:$libPath
if ($LASTEXITCODE -ne 0) { throw "lib.exe failed with exit code $LASTEXITCODE." }

Get-Item -LiteralPath $libPath | Select-Object FullName, Length
