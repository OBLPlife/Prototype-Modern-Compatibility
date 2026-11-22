param(
    [string]$VisualStudio,
    [string]$WindowsSdk,
    [string]$SdkVersion = '10.0.26100.0',
    [string]$MsvcVersion = '14.44.35207',
    [switch]$ReuseEngineModule,
    [switch]$ReuseStartupBridge,
    [switch]$ModuleOnly,
    [switch]$StartupBridgeOnly,
    [string]$CaptureRuntime
)
$ErrorActionPreference = 'Stop'
if($ModuleOnly -and $StartupBridgeOnly){throw 'Choose only one component build mode.'}
$projectRoot = $PSScriptRoot
$outputRoot = Join-Path $projectRoot 'build'
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
if(!$VisualStudio) {
    $finder=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if(Test-Path -LiteralPath $finder) {
        $VisualStudio=& $finder -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    }
}
if(!$WindowsSdk) {
    $WindowsSdk=(Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue).KitsRoot10
}
if(!$VisualStudio -or !$WindowsSdk){throw 'Install the Visual C++ x86/x64 tools and Windows SDK, or pass -VisualStudio and -WindowsSdk.'}
$toolRoot = Join-Path $VisualStudio "VC\Tools\MSVC\$MsvcVersion"
$compiler = Join-Path $toolRoot 'bin\Hostx64\x86\cl.exe'
$env:INCLUDE = @((Join-Path $toolRoot 'include'),(Join-Path $WindowsSdk "Include\$SdkVersion\ucrt"),(Join-Path $WindowsSdk "Include\$SdkVersion\shared"),(Join-Path $WindowsSdk "Include\$SdkVersion\um")) -join ';'
$env:LIB = @((Join-Path $toolRoot 'lib\x86'),(Join-Path $WindowsSdk "Lib\$SdkVersion\ucrt\x86"),(Join-Path $WindowsSdk "Lib\$SdkVersion\um\x86")) -join ';'
$env:PATH = (Split-Path -Parent $compiler) + ';' + $env:PATH
Push-Location $outputRoot
try {
    if(-not $ReuseEngineModule -and -not $StartupBridgeOnly) {
      & $compiler /nologo /LD /MT /O2 /EHsc /W4 /utf-8 /std:c++17 /D_WIN32_WINNT=0x0A00 /DUNICODE /D_UNICODE `
        (Join-Path $projectRoot 'src\compat.cpp') /Fe:PrototypeModern.dll /link `
        ("/DEF:"+(Join-Path $projectRoot 'src\compat.def')) /MACHINE:X86 /DYNAMICBASE /NXCOMPAT `
        /PDB:PrototypeModern.pdb /DEBUG /INCREMENTAL:NO kernel32.lib user32.lib setupapi.lib
      if($LASTEXITCODE -ne 0) {throw 'Compatibility module build failed.'}
    } elseif(-not $StartupBridgeOnly -and -not (Test-Path -LiteralPath '.\PrototypeModern.dll')) {throw 'Existing engine module is required for -ReuseEngineModule.'}
    # Local candidates do not rebuild or package an installer before acceptance.
    if($ModuleOnly) {
        Get-FileHash -LiteralPath '.\PrototypeModern.dll' -Algorithm SHA256
        return
    }
    if(-not $ReuseStartupBridge) {
    & (Join-Path (Split-Path -Parent $compiler) 'ml.exe') /nologo /c /coff /safeseh (Join-Path $projectRoot 'tools\repair_socket_stub.asm')
    if($LASTEXITCODE -ne 0) {throw 'Startup socket procedure assembly failed.'}
    & $compiler /nologo /MT /O2 /EHsc /W4 /utf-8 /std:c++17 /D_WIN32_WINNT=0x0A00 /DUNICODE /D_UNICODE `
        (Join-Path $projectRoot 'src\startup_bridge.cpp') repair_socket_stub.obj /Fe:PrototypeModern_Start.exe /link `
        /SUBSYSTEM:WINDOWS /MACHINE:X86 /DYNAMICBASE /NXCOMPAT /INCREMENTAL:NO kernel32.lib ws2_32.lib shell32.lib user32.lib
    if($LASTEXITCODE -ne 0) {throw 'Startup bridge build failed.'}
    } elseif(-not (Test-Path -LiteralPath '.\PrototypeModern_Start.exe')) {throw 'Existing startup bridge is required for -ReuseStartupBridge.'}
    if($StartupBridgeOnly){Get-FileHash -LiteralPath '.\PrototypeModern_Start.exe' -Algorithm SHA256;return}
    $runtimeName='python-3.13.12-embed-amd64.zip'
    if(!$CaptureRuntime) {
        $CaptureRuntime=Join-Path $projectRoot ('vendor\'+$runtimeName)
        if(!(Test-Path -LiteralPath $CaptureRuntime)){$CaptureRuntime=Join-Path $outputRoot $runtimeName}
    }
    if(!(Test-Path -LiteralPath $CaptureRuntime) -or (Get-FileHash -LiteralPath $CaptureRuntime).Hash -ne '76f238f606250c87c6beac75dccd35ee99070a13490555936abb6cb64ecce3d0'){
        throw 'The verified CPython 3.13.12 Windows x64 embeddable ZIP is required. See RELEASE.md.'
    }
    $csc=Join-Path $env:WINDIR 'Microsoft.NET\Framework\v4.0.30319\csc.exe'
    $common=@('/nologo','/target:winexe','/platform:x86','/optimize+','/utf8output','/codepage:65001',
        ('/win32manifest:'+(Join-Path $projectRoot 'installer\app.manifest')),
        '/reference:System.dll','/reference:System.Core.dll','/reference:System.Drawing.dll','/reference:System.IO.Compression.dll',
        '/reference:System.Windows.Forms.dll','/reference:System.Web.Extensions.dll',
        '/resource:PrototypeModern.dll,module','/resource:PrototypeModern_Start.exe,startupBridge',
        ('/resource:'+(Join-Path $projectRoot 'src\profile.json')+',profile'),
        ('/resource:'+$CaptureRuntime+',captureRuntime'),
        ('/resource:'+(Join-Path $projectRoot 'installer\auto_capture.ps1')+',captureBootstrap'),
        ('/resource:'+(Join-Path $projectRoot 'tools\capture_text.py')+',captureText'),
        ('/resource:'+(Join-Path $projectRoot 'tools\capture_engine.py')+',captureEngine'))
    $sources=@(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'installer') -Filter '*.cs' -File | Sort-Object Name | ForEach-Object FullName)
    foreach($language in 'CN','EN') {
        $exe="Prototype_Modern_Compatibility_${language}_Setup.exe"
        $define=if($language -eq 'EN'){'/define:PM_ENGLISH'}else{'/define:PM_CHINESE'}
        & $csc @common $define "/out:$exe" @sources
        if($LASTEXITCODE -ne 0){throw "$language installer build failed."}
        Get-FileHash -LiteralPath $exe
    }
} finally { Pop-Location }
