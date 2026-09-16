param(
    [Parameter(Mandatory=$true)]
    [string]$Ip
)

$ErrorActionPreference = "Stop"
Invoke-RestMethod -Method Post -Uri "http://$Ip/api/audio/asr/config" | Format-List
