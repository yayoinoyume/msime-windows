#!/usr/bin/env pwsh
# 收集水杉输入法的诊断信息，用于排查「能切输入法但候选窗不出现」这类问题。
#
# 只读：不启动、不停止任何进程，不修改任何文件。把结果贴回对话即可。
#
# 用法：
#   pwsh -File .\collect-diagnostics.ps1
#   pwsh -File .\collect-diagnostics.ps1 -ServerPath 'D:\custom\MetasequoiaImeServer.exe'

[CmdletBinding()]
param(
    [string]$ServerPath
)

$ErrorActionPreference = 'Continue'

function Section([string]$title) {
    Write-Host ''
    Write-Host "=== $title ==="
}

# ---------------------------------------------------------------- 安装位置

Section '数据目录（注册表 DataDir）'
$dataDir = $null
try {
    $key = 'HKLM:\Software\Metasequoia\MetasequoiaIME'
    if (Test-Path $key) {
        $dataDir = (Get-ItemProperty -Path $key -ErrorAction SilentlyContinue).DataDir
        $versionDir = (Get-ItemProperty -Path $key -ErrorAction SilentlyContinue).VersionDir
        Write-Host "DataDir     = $dataDir"
        Write-Host "VersionDir  = $versionDir"
    }
    else {
        Write-Host '注册表键不存在：HKLM\Software\Metasequoia\MetasequoiaIME'
    }
}
catch {
    Write-Host "读取注册表失败：$($_.Exception.Message)"
}
if (-not $dataDir) {
    $fallback = Join-Path $env:LOCALAPPDATA 'metasequoiaime'
    Write-Host "回退默认位置：$fallback"
    $dataDir = $fallback
}

Section '安装目录'
if (-not $ServerPath) {
    $candidates = @(
        (Join-Path $env:ProgramFiles 'metasequoiaime\server\MetasequoiaImeServer.exe'),
        ((Join-Path ${env:ProgramFiles(x86)} 'metasequoiaime\server\MetasequoiaImeServer.exe'))
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
    if ($candidates) { $ServerPath = $candidates[0] }
}
if ($ServerPath -and (Test-Path -LiteralPath $ServerPath)) {
    $item = Get-Item -LiteralPath $ServerPath
    Write-Host "Server      = $($item.FullName)"
    Write-Host "版本        = $($item.VersionInfo.FileVersion)"
    Write-Host "大小        = $($item.Length) 字节"
    Write-Host "修改时间    = $($item.LastWriteTime)"

    # 未签名 + uiAccess="true" 会让进程启动被拒。这里只报数字，不评价。
    $signature = Get-AuthenticodeSignature -LiteralPath $item.FullName
    Write-Host "签名状态    = $($signature.Status)"
    if ($signature.SignerCertificate) {
        Write-Host "签名者      = $($signature.SignerCertificate.Subject)"
    }

    # 从 PE 里找 requestedExecutionLevel 那一行。
    try {
        $bytes = [System.IO.File]::ReadAllBytes($item.FullName)
        $text = [System.Text.Encoding]::UTF8.GetString($bytes)
        $match = [regex]::Match($text, 'requestedExecutionLevel[^>]*')
        if ($match.Success) { Write-Host "清单        = $($match.Value)" }
        else { Write-Host '清单        = 未找到 requestedExecutionLevel' }
    }
    catch {
        Write-Host "读取 PE 失败：$($_.Exception.Message)"
    }
}
else {
    Write-Host '未找到 MetasequoiaImeServer.exe（用 -ServerPath 指定）'
}

Section 'TSF DLL'
foreach ($root in @($env:SystemRoot + '\IME\MetasequoiaIME', $env:SystemRoot + '\SysWOW64\IME\MetasequoiaIME')) {
    if (Test-Path -LiteralPath $root) {
        Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue | ForEach-Object {
            $dll = Join-Path $_.FullName 'MetasequoiaImeTsf.dll'
            if (Test-Path -LiteralPath $dll) {
                $v = (Get-Item -LiteralPath $dll).VersionInfo.FileVersion
                Write-Host "$dll  版本=$v"
            }
        }
    }
    else {
        Write-Host "(不存在) $root"
    }
}

# ---------------------------------------------------------------- 日志

Section '启动探针日志（server-boot.log）'
$bootLogs = @(
    (Join-Path $dataDir 'logs\server-boot.log'),
    (Join-Path $env:LOCALAPPDATA 'metasequoiaime\logs\server-boot.log'),
    (Join-Path $env:TEMP 'server-boot.log')
) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
if ($bootLogs) {
    foreach ($log in $bootLogs) {
        Write-Host "--- $log"
        Get-Content -LiteralPath $log -Tail 80
    }
}
else {
    Write-Host '未找到 server-boot.log。'
    Write-Host '含义：Server 进程从未执行到 WinMain（被 Windows 拒绝启动常见于未签名的 uiAccess 程序），'
    Write-Host '      或本机装的还是不含探针的旧版本。'
}

Section '诊断日志（水杉IME诊断日志.log）'
$diagLogs = @(
    (Join-Path ([Environment]::GetFolderPath('Desktop')) '水杉IME诊断日志.log'),
    (Join-Path $dataDir 'logs\水杉IME诊断日志.log'),
    (Join-Path $env:LOCALAPPDATA 'metasequoiaime\logs\水杉IME诊断日志.log')
) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
if ($diagLogs) {
    foreach ($log in $diagLogs) {
        Write-Host "--- $log"
        Get-Content -LiteralPath $log -Tail 60
    }
}
else {
    Write-Host '未找到诊断日志。'
    Write-Host '含义：diagnostic_log 仍是关闭状态，或 Server 没跑到初始化日志那一步。'
}

# ---------------------------------------------------------------- 进程与事件

Section '当前进程'
foreach ($name in 'MetasequoiaImeServer', 'MetasequoiaImeWatchdog', 'MetasequoiaImeSettings') {
    $p = Get-Process -Name $name -ErrorAction SilentlyContinue
    if ($p) { $p | ForEach-Object { Write-Host "$name pid=$($_.Id) 启动=$($_.StartTime)" } }
    else { Write-Host "$name 未运行" }
}

Section '最近的应用事件（含进程启动失败记录）'
try {
    Get-WinEvent -FilterHashtable @{
        LogName   = 'Application'
        StartTime = (Get-Date).AddHours(-2)
    } -MaxEvents 200 -ErrorAction SilentlyContinue |
        Where-Object { $_.Message -match 'Metasequoia|水杉' } |
        Select-Object -First 10 |
        ForEach-Object { Write-Host "[$($_.TimeCreated)] $($_.LevelDisplayName): $($_.Message)" }
    Write-Host '（以上为空表示近两小时没有相关事件）'
}
catch {
    Write-Host "读取事件日志失败：$($_.Exception.Message)"
}

Write-Host ''
Write-Host '=== 完成：把以上输出整段贴回对话即可 ==='
