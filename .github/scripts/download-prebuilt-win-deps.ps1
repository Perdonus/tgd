param(
  [Parameter(Mandatory = $true)]
  [string]$PrepareRoot,
  [string]$Repo = 'Perdonus/astrogram-win-deps',
  [string]$ManifestName = 'astrogram-win-deps-manifest.json'
)

$ErrorActionPreference = 'Stop'

function Set-StepOutput([string]$Name, [string]$Value) {
  if ($env:GITHUB_OUTPUT) {
    "$Name=$Value" | Out-File -FilePath $env:GITHUB_OUTPUT -Encoding utf8 -Append
  }
}

function Use-NoPrebuilt([string]$Reason) {
  if (-not [string]::IsNullOrWhiteSpace($Reason)) {
    Write-Host "[deps] $Reason"
  }
  Set-StepOutput -Name 'ready' -Value 'false'
  exit 0
}

function Download-Asset(
  [hashtable]$Headers,
  [string]$Uri,
  [string]$Destination,
  [string]$Label
) {
  Write-Host "[deps] downloading $Label"
  Invoke-WebRequest -Headers $Headers -Uri $Uri -OutFile $Destination
}

try {
  $headers = @{ Accept = 'application/vnd.github+json' }
  if (-not [string]::IsNullOrWhiteSpace($env:GH_TOKEN)) {
    $headers.Authorization = "Bearer $env:GH_TOKEN"
  }

  try {
    $release = Invoke-RestMethod -Headers $headers -Uri "https://api.github.com/repos/$Repo/releases/latest"
  } catch {
    Use-NoPrebuilt "no published release in $Repo yet"
  }

  $assets = @{}
  foreach ($asset in $release.assets) {
    $assets[$asset.name] = $asset.browser_download_url
  }

  $tmp = Join-Path $env:RUNNER_TEMP 'astrogram-win-deps'
  New-Item -ItemType Directory -Path $tmp -Force | Out-Null

  $libsArchiveName = 'astrogram-win-libs.tar.zst'
  $thirdpartyName = 'astrogram-win-thirdparty.tar.zst'
  $sourcesName = 'astrogram-win-sources.tar.zst'
  $libsPartNames = @()

  if ($assets.ContainsKey($ManifestName)) {
    $manifestPath = Join-Path $tmp $ManifestName
    Download-Asset -Headers $headers -Uri $assets[$ManifestName] -Destination $manifestPath -Label $ManifestName

    $manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    if (-not $manifest.libs) {
      Use-NoPrebuilt 'manifest missing libs metadata'
    }

    if ($manifest.libs.name) {
      $libsArchiveName = [string]$manifest.libs.name
    }
    if ($manifest.thirdparty -and $manifest.thirdparty.name) {
      $thirdpartyName = [string]$manifest.thirdparty.name
    }
    if ($manifest.sources -and $manifest.sources.name) {
      $sourcesName = [string]$manifest.sources.name
    }
    if ($manifest.libs.parts) {
      foreach ($part in @($manifest.libs.parts)) {
        $partName = [string]$part
        if ([string]::IsNullOrWhiteSpace($partName)) {
          Use-NoPrebuilt 'manifest contains an empty libs part name'
        }
        $libsPartNames += $partName
      }
    }
  }

  $required = @($thirdpartyName, $sourcesName)
  if ($libsPartNames.Count -gt 0) {
    $required += $libsPartNames
  } else {
    $required += $libsArchiveName
  }

  $missing = @($required | Where-Object { -not $assets.ContainsKey($_) })
  if ($missing.Count -gt 0) {
    Use-NoPrebuilt "latest release exists but missing assets: $($missing -join ', ')"
  }

  $thirdpartyArchivePath = Join-Path $tmp $thirdpartyName
  $sourcesArchivePath = Join-Path $tmp $sourcesName
  $libsArchivePath = Join-Path $tmp $libsArchiveName

  Download-Asset -Headers $headers -Uri $assets[$thirdpartyName] -Destination $thirdpartyArchivePath -Label $thirdpartyName
  Download-Asset -Headers $headers -Uri $assets[$sourcesName] -Destination $sourcesArchivePath -Label $sourcesName

  if ($libsPartNames.Count -gt 0) {
    if (Test-Path -LiteralPath $libsArchivePath) {
      Remove-Item -LiteralPath $libsArchivePath -Force
    }
    $targetStream = [System.IO.File]::Open($libsArchivePath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    try {
      foreach ($partName in $libsPartNames) {
        $partPath = Join-Path $tmp $partName
        Download-Asset -Headers $headers -Uri $assets[$partName] -Destination $partPath -Label $partName
        $partStream = [System.IO.File]::OpenRead($partPath)
        try {
          $partStream.CopyTo($targetStream)
        } finally {
          $partStream.Dispose()
        }
      }
    } finally {
      $targetStream.Dispose()
    }
  } else {
    Download-Asset -Headers $headers -Uri $assets[$libsArchiveName] -Destination $libsArchivePath -Label $libsArchiveName
  }

  foreach ($archivePath in @($libsArchivePath, $thirdpartyArchivePath, $sourcesArchivePath)) {
    Write-Host "[deps] extracting $([System.IO.Path]::GetFileName($archivePath))"
    & tar --zstd -xf $archivePath -C $PrepareRoot
    if ($LASTEXITCODE -ne 0) {
      throw "failed to extract $archivePath"
    }
  }

  Set-StepOutput -Name 'ready' -Value 'true'
} catch {
  Write-Host "[deps] failed to use prebuilt bundle: $($_.Exception.Message)"
  Set-StepOutput -Name 'ready' -Value 'false'
  exit 0
}
