# F4 Online launcher: gets the newest F4Online.dll from GitHub Releases, then starts the game through F4SE.
# Lives in the Fallout 4 folder; the "Fallout 4 (F4SE)" desktop shortcut runs it. Any problem -> the game starts anyway.
$Repo    = 'fentmaxxer694201-byte/F4Online'
$GameDir = $PSScriptRoot
$Plugins = Join-Path $GameDir 'Data\F4SE\Plugins'
$Dll     = Join-Path $Plugins 'F4Online.dll'
$VerFile = Join-Path $Plugins 'F4Online.version'

try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $rel  = Invoke-RestMethod "https://api.github.com/repos/$Repo/releases/latest" -Headers @{ 'User-Agent' = 'F4Online-Updater' } -TimeoutSec 10
    $have = if (Test-Path $VerFile) { (Get-Content $VerFile -Raw).Trim() } else { '' }
    if ($rel.tag_name -eq $have) {
        Write-Host "F4 Online $have is up to date."
    } elseif (Get-Process Fallout4 -ErrorAction SilentlyContinue) {
        Write-Host "F4 Online $($rel.tag_name) is out, but Fallout 4 is already running. Close it and start again to update."
    } else {
        $asset = $rel.assets | Where-Object name -eq 'F4Online.dll'
        if (-not $asset) { throw "release $($rel.tag_name) has no F4Online.dll" }
        Write-Host "Downloading F4 Online $($rel.tag_name)..."
        $tmp = "$Dll.download"
        Invoke-WebRequest $asset.browser_download_url -OutFile $tmp -UseBasicParsing -TimeoutSec 120
        # GitHub publishes each file's SHA-256; a mismatch means a broken download, so keep the old version.
        $hash = 'sha256:' + (Get-FileHash $tmp -Algorithm SHA256).Hash.ToLower()
        if ($asset.digest -and $asset.digest -ne $hash) { Remove-Item $tmp; throw 'the download was damaged' }
        if (Test-Path $Dll) { Copy-Item $Dll "$Dll.old" -Force }  # one step back, by hand if ever needed
        Move-Item $tmp $Dll -Force
        Set-Content $VerFile $rel.tag_name
        Write-Host "Updated F4 Online to $($rel.tag_name)."
    }
} catch {
    Write-Host "Update check skipped ($($_.Exception.Message)). Starting the game as it is."
    Start-Sleep 3
}

Start-Process (Join-Path $GameDir 'f4se_loader.exe') -WorkingDirectory $GameDir
