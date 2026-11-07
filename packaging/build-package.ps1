param(
    [Parameter(Mandatory=$true)][string]$StageDir,
    [string]$Version='0.7.0',
    [string]$OutputDir='',
    [string]$InnoCompiler=''
)
$ErrorActionPreference='Stop'
$project=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$stage=[IO.Path]::GetFullPath($StageDir)
if(-not (Test-Path -LiteralPath $stage -PathType Container)){throw 'StageDir does not exist'}
if($Version -notmatch '^\d+\.\d+\.\d+$'){throw 'Version must be major.minor.patch'}
if(!$OutputDir){$OutputDir=Join-Path $project 'dist'}
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
if(!$InnoCompiler){$InnoCompiler=Join-Path $project 'tools\installer\inno-6.2.2\app\ISCC.exe'}
if(-not (Test-Path -LiteralPath $InnoCompiler -PathType Leaf)){throw 'Inno Setup compiler not found'}
# CMake install merges into an existing directory. Refuse stale SDK/test/debug
# payloads instead of silently including them in Inno's recursive file entry.
$allowedRoots=@('bin','docs','examples','include','lib','licenses','README.md','LICENSE','PAYLOAD_SHA256.txt')
foreach($item in Get-ChildItem -LiteralPath $stage -Force){
    if($item.Name -notin $allowedRoots){throw "Unexpected stage entry '$($item.Name)'. Recreate a clean stage using CMake install."}
}
foreach($item in Get-ChildItem -LiteralPath $stage -Recurse -Force){
    if(($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw "Stage links are not allowed: $($item.FullName)"}
    if(($item.PSIsContainer -and $item.Name -match '^(firmware|tests?|validation|backups?|debug)$') -or
       (!$item.PSIsContainer -and ($item.Extension -match '^\.(elf|bit|ltx|dmp|log|pdb|ilk|obj|o)$' -or
        $item.Name -match '^validation(?:[.-].*)?$'))){
        throw "Non-release stage entry: $($item.FullName). Recreate a clean stage."
    }
}
foreach($required in @('bin\och.exe','bin\omnicore.dll','include\omnicore')){
    if(-not (Test-Path -LiteralPath (Join-Path $stage $required))){throw "Missing stage file: $required"}
}
$allowedBin=@('och.exe','omnicore.dll','libusbK.dll','och-driver.exe','libwdi.dll','och-shell.cmd')
foreach($item in Get-ChildItem -LiteralPath (Join-Path $stage 'bin') -Force){
    if($item.PSIsContainer -or $item.Name -notin $allowedBin){throw "Unexpected stage binary: $($item.Name)"}
}
$runtimeVersion=& (Join-Path $stage 'bin\och.exe') version --json
if($LASTEXITCODE -ne 0){throw 'Staged och.exe cannot run'}
$runtimeVersion=$runtimeVersion|ConvertFrom-Json
if($runtimeVersion.version -ne $Version){throw "Stage version $($runtimeVersion.version) does not match package version $Version"}
$driver=Join-Path $project 'third_party\driver'
$tools=Join-Path $project 'tools\installer'
foreach($f in @('och-driver.exe','libwdi.dll')){Copy-Item -LiteralPath (Join-Path $tools $f) -Destination (Join-Path $stage 'bin') -Force}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'och-shell.cmd') -Destination (Join-Path $stage 'bin') -Force
Copy-Item -LiteralPath (Join-Path $driver 'libusbK-3.1.0.0\libusbK-3.1.0.0-bin\bin\dll\amd64\libusbK.dll') -Destination (Join-Path $stage 'bin') -Force
& (Join-Path $tools 'build-msvc-import.ps1') -DllPath (Join-Path $stage 'bin\omnicore.dll') -OutputDir (Join-Path $stage 'lib')
if($LASTEXITCODE -ne 0){throw 'MSVC import-library generation failed'}
foreach($d in @('docs','licenses\libusbK','licenses\libwdi','licenses\mingw','licenses\sources','licenses\driver-build')){New-Item -ItemType Directory -Force (Join-Path $stage $d)|Out-Null}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'INSTALL_WINDOWS.txt') -Destination (Join-Path $stage 'docs') -Force
Copy-Item -Path (Join-Path $driver 'libusbK-3.1.0.0-src\license\*') -Destination (Join-Path $stage 'licenses\libusbK') -Force
Copy-Item -LiteralPath (Join-Path $driver 'libwdi-1.5.1\COPYING'),(Join-Path $driver 'libwdi-1.5.1\COPYING-LGPL') -Destination (Join-Path $stage 'licenses\libwdi') -Force
Copy-Item -LiteralPath (Join-Path $driver 'downloads\libwdi-v1.5.1.zip'),(Join-Path $driver 'downloads\libusbK-3.1.0.0-src.7z') -Destination (Join-Path $stage 'licenses\sources') -Force
Copy-Item -LiteralPath (Join-Path $driver 'NOTICES.txt'),(Join-Path $driver 'SOURCES.json') -Destination (Join-Path $stage 'licenses') -Force
Copy-Item -Path (Join-Path $driver 'runtime_licenses\*') -Destination (Join-Path $stage 'licenses\mingw') -Recurse -Force
foreach($f in @('och_driver.cpp','och_driver.rc','och_driver.manifest','build-driver.ps1','build-msvc-import.ps1')){Copy-Item -LiteralPath (Join-Path $tools $f) -Destination (Join-Path $stage 'licenses\driver-build') -Force}

# Record the exact payload and reject unexpected external runtime dependencies before packaging.
$objdump='C:\msys64\mingw64\bin\objdump.exe'
if(Test-Path $objdump){
    foreach($f in @('och.exe','omnicore.dll','och-driver.exe','libwdi.dll','libusbK.dll')){
        $imports=& $objdump -p (Join-Path $stage "bin\$f")
        if($LASTEXITCODE-ne 0){throw "Cannot inspect PE: $f"}
        $bad=$imports|Select-String -Pattern 'DLL Name: (libstdc\+\+|libgcc|libwinpthread|VCRUNTIME|MSVCP)'
        if($bad){throw "Unbundled compiler runtime dependency in $f : $bad"}
    }
}
$manifest=Get-ChildItem -LiteralPath $stage -File -Recurse | Where-Object {$_.Name -ne 'PAYLOAD_SHA256.txt'} | Sort-Object FullName | ForEach-Object {
    $relative=$_.FullName.Substring($stage.Length).TrimStart('\').Replace('\','/')
    "{0}  {1}" -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(),$relative
}
[IO.File]::WriteAllLines((Join-Path $stage 'PAYLOAD_SHA256.txt'),$manifest)
New-Item -ItemType Directory -Force $OutputDir|Out-Null
& $InnoCompiler ('/DStageDir='+$stage) ('/DOutputDir='+$OutputDir) ('/DAppVersion='+$Version) (Join-Path $PSScriptRoot 'omnicore-host.iss')
if($LASTEXITCODE-ne 0){throw "Inno Setup failed: $LASTEXITCODE"}
$setup=Join-Path $OutputDir "OmniCoreHost-$Version-win64-setup.exe"
$hash=(Get-FileHash -LiteralPath $setup).Hash.ToLowerInvariant()
[IO.File]::WriteAllText(($setup+'.sha256'),$hash+'  '+[IO.Path]::GetFileName($setup)+[Environment]::NewLine)
Write-Host "Installer: $setup"
Write-Host "SHA256: $hash"
