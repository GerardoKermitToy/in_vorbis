[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)][string]$WinampDirectory,
    [string]$PluginPath = (Join-Path $PSScriptRoot '../dist/Plugins/in_vorbis.dll')
)
$ErrorActionPreference = 'Stop'
$Source = (Resolve-Path -LiteralPath $PluginPath).Path
if (-not (Test-Path -LiteralPath (Join-Path $WinampDirectory 'winamp.exe') -PathType Leaf)) {
    throw 'The selected directory does not contain winamp.exe.'
}
if (Get-Process -Name winamp -ErrorAction SilentlyContinue) { throw 'Close Winamp before installing. Nothing was changed.' }
$Bytes = [IO.File]::ReadAllBytes($Source)
if ($Bytes.Length -lt 64 -or $Bytes[0] -ne 0x4d -or $Bytes[1] -ne 0x5a) { throw 'Not a Windows PE DLL.' }
$Pe = [BitConverter]::ToInt32($Bytes, 0x3c)
if ($Pe -lt 0 -or $Pe -gt $Bytes.Length - 24 -or [BitConverter]::ToUInt32($Bytes, $Pe) -ne 0x4550 -or [BitConverter]::ToUInt16($Bytes, $Pe + 4) -ne 0x14c) { throw 'A 32-bit x86 DLL is required.' }
$Target = Join-Path $WinampDirectory 'Plugins/in_vorbis.dll'
if ([IO.Path]::GetFullPath($Source) -eq [IO.Path]::GetFullPath($Target)) { throw 'Source and destination are identical.' }
$Hash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
if ($PSCmdlet.ShouldProcess($Target, 'Install independent Ogg plugin, backing up the original Vorbis plugin')) {
    $Backup = $null
    if (Test-Path -LiteralPath $Target) {
        $OldHash = (Get-FileHash -LiteralPath $Target -Algorithm SHA256).Hash
        $Backup = "$Target.backup-$([Guid]::NewGuid().ToString('N'))"
        Copy-Item -LiteralPath $Target -Destination $Backup
        if ((Get-FileHash -LiteralPath $Backup -Algorithm SHA256).Hash -ne $OldHash) { throw 'Backup verification failed; original was not changed.' }
    }
    try {
        Copy-Item -LiteralPath $Source -Destination $Target -Force
        if ((Get-FileHash -LiteralPath $Target -Algorithm SHA256).Hash -ne $Hash) { throw 'Installation checksum mismatch.' }
    } catch {
        if ($Backup) { Copy-Item -LiteralPath $Backup -Destination $Target -Force }
        throw
    }
    Write-Host "Installed: $Target"
    if ($Backup) { Write-Host "Backup: $Backup" }
    Write-Host "SHA256: $Hash"
}
