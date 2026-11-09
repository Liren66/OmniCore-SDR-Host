param(
    [Parameter(Mandatory=$true)][string]$DllPath,
    [Parameter(Mandatory=$true)][string]$OutputDir,
    [string]$LibExe='',
    [string]$Objdump='C:\msys64\mingw64\bin\objdump.exe'
)
$ErrorActionPreference='Stop'
$dll=(Resolve-Path -LiteralPath $DllPath).Path
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
if(!$LibExe){
    $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if(Test-Path -LiteralPath $vswhere){
        $vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if($vs){$LibExe=(Get-ChildItem -LiteralPath (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | ForEach-Object {Join-Path $_.FullName 'bin\Hostx64\x64\lib.exe'} | Where-Object {Test-Path -LiteralPath $_} | Select-Object -First 1)}
    }
}
if(!$LibExe -or !(Test-Path -LiteralPath $LibExe)){throw 'Microsoft lib.exe not found. Install VS C++ Build Tools or pass -LibExe. No GNU archive is mislabeled as a Microsoft .lib.'}
if(!(Test-Path -LiteralPath $Objdump)){throw 'objdump.exe is required to read the final DLL export table'}
$dump=& $Objdump -p $dll
if($LASTEXITCODE-ne 0){throw 'Cannot read DLL export table'}
$exports=@($dump | ForEach-Object {if($_ -match '^\s*\[\s*\d+\].*\s(och_[A-Za-z0-9_]+)\s*$'){$Matches[1]}} | Sort-Object -Unique)
if($exports.Count -lt 20 -or !($exports -contains 'och_version') -or !($exports -contains 'och_recv') -or !($exports -contains 'och_send')){throw 'Unexpected OmniCore Host export table'}
New-Item -ItemType Directory -Force -Path $OutputDir|Out-Null
$def=Join-Path $OutputDir 'omnicore.def'
[IO.File]::WriteAllLines($def,(@('LIBRARY omnicore.dll','EXPORTS')+$exports),[Text.Encoding]::ASCII)
$lib=Join-Path $OutputDir 'omnicore.lib'
& $LibExe /nologo /machine:x64 ('/def:'+$def) ('/out:'+$lib)
if($LASTEXITCODE-ne 0){throw 'Microsoft import-library generation failed'}
[PSCustomObject]@{dll=$dll;dll_sha256=(Get-FileHash -LiteralPath $dll).Hash.ToLowerInvariant();lib_tool=$LibExe;export_count=$exports.Count;import_library=$lib;import_sha256=(Get-FileHash -LiteralPath $lib).Hash.ToLowerInvariant()}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $OutputDir 'msvc-import.json') -Encoding utf8
Write-Host "Created native Microsoft x64 import library for $($exports.Count) C exports: $lib"
