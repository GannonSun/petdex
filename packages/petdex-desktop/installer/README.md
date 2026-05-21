# Windows installer

This directory contains the first Windows installer project for Petdex Desktop.

The installer uses Inno Setup and intentionally keeps the application install
directory separate from the user pet library:

- App files: user-selected install directory, defaulting to `%LOCALAPPDATA%\Programs\Petdex`
- Pet files: `%LOCALAPPDATA%\.petdex\pets`

The default bundled pet is `bao` and is installed to:

```text
%LOCALAPPDATA%\.petdex\pets\bao
```

Users can install future Petdex pets by copying a pet directory into:

```text
%LOCALAPPDATA%\.petdex\pets\<pet-slug>
```

Each pet directory should contain at least:

```text
pet.json
spritesheet.webp
```

## Build

Install Inno Setup 6, then run from `packages/petdex-desktop`:

```powershell
.\scripts\build-windows-installer.ps1 `
  -ZigExe C:\path\to\zig.exe `
  -ZeroNativePath C:\path\to\zero-native
```

The script builds `zig-out\bin\petdex-desktop.exe` first, then writes the
installer to:

```text
dist\installer\
```

