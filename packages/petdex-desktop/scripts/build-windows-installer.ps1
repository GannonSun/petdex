param(
  [string]$ZigExe,
  [string]$ZeroNativePath,
  [string]$InnoSetupCompiler
)

$ErrorActionPreference = "Stop"

$PackageRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
Push-Location $PackageRoot
try {
  if (-not $ZigExe) {
    $zigCommand = Get-Command zig -ErrorAction SilentlyContinue
    if ($zigCommand) {
      $ZigExe = $zigCommand.Source
    } else {
      throw "Zig was not found. Pass -ZigExe C:\path\to\zig.exe."
    }
  }

  if (-not $ZeroNativePath) {
    if ($env:ZERO_NATIVE_PATH) {
      $ZeroNativePath = $env:ZERO_NATIVE_PATH
    } else {
      $candidate = Resolve-Path "..\..\zero-native" -ErrorAction SilentlyContinue
      if ($candidate) {
        $ZeroNativePath = $candidate.Path
      } else {
        throw "zero-native was not found. Pass -ZeroNativePath C:\path\to\zero-native or set ZERO_NATIVE_PATH."
      }
    }
  }

  if (-not $InnoSetupCompiler) {
    $candidates = @(
      (Join-Path ${env:ProgramFiles} "Inno Setup 6\ISCC.exe"),
      (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe")
    )
    foreach ($candidate in $candidates) {
      if ($candidate -and (Test-Path $candidate)) {
        $InnoSetupCompiler = $candidate
        break
      }
    }
    if (-not $InnoSetupCompiler) {
      $isccCommand = Get-Command iscc -ErrorAction SilentlyContinue
      if ($isccCommand) {
        $InnoSetupCompiler = $isccCommand.Source
      }
    }
    if (-not $InnoSetupCompiler) {
      throw "Inno Setup compiler was not found. Install Inno Setup 6 or pass -InnoSetupCompiler C:\path\to\ISCC.exe."
    }
  }

  $defaultPet = Join-Path $PackageRoot "installer\default-pets\bao"
  foreach ($required in @("pet.json", "spritesheet.webp")) {
    $path = Join-Path $defaultPet $required
    if (-not (Test-Path $path)) {
      throw "Missing default pet asset: $path"
    }
  }

  & $ZigExe build -Dplatform=win32 -Dzero-native-path="$ZeroNativePath"
  if ($LASTEXITCODE -ne 0) {
    throw "zig build failed with exit code $LASTEXITCODE"
  }

  $exe = Join-Path $PackageRoot "zig-out\bin\petdex-desktop.exe"
  if (-not (Test-Path $exe)) {
    throw "Expected desktop executable was not produced: $exe"
  }

  $iss = Join-Path $PackageRoot "installer\petdex-desktop.iss"
  & $InnoSetupCompiler $iss
  if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup failed with exit code $LASTEXITCODE"
  }

  $dist = Join-Path $PackageRoot "dist\installer"
  Write-Host "Installer output:"
  Get-ChildItem $dist -Filter "*.exe" | Sort-Object LastWriteTime -Descending | Select-Object -First 5 FullName, Length, LastWriteTime
} finally {
  Pop-Location
}

