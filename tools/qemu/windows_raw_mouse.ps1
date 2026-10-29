param(
    [string]$Endpoint = "",
    [Parameter(Mandatory=$true)][string]$LogPath,
    [switch]$SelfTest
)
$ErrorActionPreference = "Stop"
Add-Type -Path (Join-Path $PSScriptRoot "windows_raw_mouse.cs") -ReferencedAssemblies System.Windows.Forms,System.Drawing,System.Web.Extensions
if ($SelfTest) { [QemuRawMouse]::LayoutTest(); exit }
if ($Endpoint -eq "") { $Endpoint = $null }
[QemuRawMouse]::Run($Endpoint, $LogPath)
