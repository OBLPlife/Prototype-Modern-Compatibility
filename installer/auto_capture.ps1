[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateRange(1,2147483647)][int]$TargetProcessId,
    [Parameter(Mandatory=$true)][long]$TargetStartFileTime,
    [Parameter(Mandatory=$true)][string]$GameDirectory
)
$ErrorActionPreference='Stop'
$logFolder=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'PrototypeModern\Performance'
[IO.Directory]::CreateDirectory($logFolder) | Out-Null
$log=Join-Path $logFolder 'bootstrap.log'
function Write-CaptureStatus([string]$status,[string]$detail) {
    $record=[ordered]@{utc=[DateTime]::UtcNow.ToString('o');pid=$TargetProcessId;creation_filetime=$TargetStartFileTime;status=$status;detail=$detail.Substring(0,[Math]::Min(1000,$detail.Length))}
    $line=($record | ConvertTo-Json -Compress)+[Environment]::NewLine
    $utf8=New-Object Text.UTF8Encoding($false)
    if((Test-Path -LiteralPath $log) -and (Get-Item -LiteralPath $log).Length+$utf8.GetByteCount($line) -gt 8192){[IO.File]::WriteAllText($log,'',$utf8)}
    [IO.File]::AppendAllText($log,$line,$utf8)
}
try {
    $target=Get-Process -Id $TargetProcessId -ErrorAction Stop
    if($target.StartTime.ToUniversalTime().ToFileTimeUtc() -ne $TargetStartFileTime -or
       $target.Path -ne (Join-Path $GameDirectory 'prototypef.exe')){throw 'The original Steam child identity does not match.'}
    Write-CaptureStatus 'AUTO_TEXT_START' 'Passive five-second CPU/GPU and engine samples.'
    $result=& (Join-Path $PSScriptRoot 'python.exe') -B -X utf8 (Join-Path $PSScriptRoot 'capture_text.py') --pid $TargetProcessId --creation $TargetStartFileTime --game $GameDirectory --output (Join-Path $logFolder 'performance.log') 2>&1
    if($LASTEXITCODE -ne 0){throw (($result | Out-String).Trim())}
    Write-CaptureStatus 'AUTO_TEXT_STOPPED' 'Collector finished.'
} catch {
    Write-CaptureStatus 'UNAVAILABLE' $_.Exception.Message
    exit 1
}
