param([string]$CompilerRoot='C:\msys64\mingw64\bin')
$ErrorActionPreference='Stop'
$project=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$wdi=Join-Path $project 'third_party\driver\libwdi-1.5.1'
$usb=Join-Path $project 'third_party\driver\libusbK-3.1.0.0\libusbK-3.1.0.0-bin\bin'
$env:PATH=$CompilerRoot+';'+$env:PATH
function Run-Checked([string]$Exe,[string[]]$ArgList) { & $Exe @ArgList; if($LASTEXITCODE -ne 0){ throw "$Exe failed: $LASTEXITCODE" } }
$cfg=@"
#pragma once
#define OPT_M64 1
#define WDF_VER 1009
#define LIBUSBK_DIR "$($usb.Replace('\','/'))"
#define ENABLE_LOGGING 1
#define INCLUDE_DEBUG_LOGGING 1
"@
[IO.File]::WriteAllText((Join-Path $wdi 'config.h'),$cfg)
Push-Location (Join-Path $wdi 'libwdi')
try {
    # Embed the official 1.9 KMDF coinstaller with its actual zero-padded filename.
    $embed=Get-Content -Raw 'embedder_files.h'
    $embed=$embed.Replace('WdfCoInstaller" STR(WDF_VER)','WdfCoInstaller0" STR(WDF_VER)')
    [IO.File]::WriteAllText((Join-Path (Get-Location) 'embedder_files.h'),$embed)
    Run-Checked 'gcc.exe' @('-O2','-static','installer.c','-o','installer_x64.exe','-lsetupapi','-lnewdev','-lole32')
    Run-Checked 'gcc.exe' @('-O2','-static','-I..','embedder.c','-o','embedder.exe')
    Run-Checked '.\embedder.exe' @('embedded.h')
    Run-Checked 'windres.exe' @('-i','libwdi.rc','-o','libwdi-res.o')
    Run-Checked 'gcc.exe' @('-O2','-shared','-static-libgcc','-DLIBWDI_DLL_EXPORT','-I..','logging.c','tokenizer.c','vid_data.c','pki.c','libwdi_dlg.c','libwdi.c','libwdi-res.o','-o','libwdi.dll','-Wl,--out-implib,libwdi.dll.a','-lsetupapi','-lole32','-lntdll','-lversion','-lcomctl32','-luuid')
} finally { Pop-Location }
Push-Location $PSScriptRoot
try {
    Run-Checked 'windres.exe' @('-i','och_driver.rc','-o','och_driver-res.o')
    Run-Checked 'g++.exe' @('-std=c++17','-O2','-Wall','-Wextra','-Werror','-static','-municode',('-I'+(Join-Path $wdi 'libwdi')),'och_driver.cpp','och_driver-res.o','-o','och-driver.exe','-lbcrypt','-lwintrust','-lcrypt32','-lshlwapi','-lshell32','-lole32','-ladvapi32','-luuid')
    Copy-Item (Join-Path $wdi 'libwdi\libwdi.dll') 'libwdi.dll' -Force
} finally { Pop-Location }
Write-Host 'Built och-driver.exe + libwdi.dll. This command does not install a driver.'
