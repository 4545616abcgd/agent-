param(
    [Parameter(Mandatory=$true)]
    [string]$Ip,

    [Parameter(Mandatory=$true)]
    [string]$ApiKey,

    [string]$BaseUrl = "https://api.siliconflow.cn/v1/audio/transcriptions",
    [string]$Model = "FunAudioLLM/SenseVoiceSmall"
)

$ErrorActionPreference = "Stop"

$uri = "http://$Ip/api/audio/asr/config"
$body = @{
    api_key  = $ApiKey
    base_url = $BaseUrl
    model    = $Model
} | ConvertTo-Json -Compress

Write-Host "Configuring ESP-Claw ASR at $uri"
$result = Invoke-RestMethod `
    -Method Post `
    -Uri $uri `
    -ContentType "application/json; charset=utf-8" `
    -Body ([System.Text.Encoding]::UTF8.GetBytes($body))

$result | Format-List

if (-not $result.ok -or -not $result.configured) {
    throw "ASR configuration was not accepted by the board."
}

Write-Host "ASR configuration saved. API key is stored in board NVS and is not returned by the API."
