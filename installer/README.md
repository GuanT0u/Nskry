# Nskry Setup

`NskrySetup.iss` creates a per-user installer with Inno Setup 6. It installs
the executable under `%LOCALAPPDATA%\\Programs\\Nskry`; runtime data remains in
`%LOCALAPPDATA%\\Nskry` and is not removed by uninstall.

## Release build

1. From the repository root, configure and build with CMake/Ninja in Release
   mode. This builds `..\\build\\Nskry.exe` and generates the official bundled
   packages under `installer\\bundled` (currently `nskry-scroll.nskryplugin` and
   `nskry-ocr.nskryplugin`).
2. Optionally place additional official signed/hashed `.nskryplugin` files in
   `installer\\bundled` using the names in the script.
3. Run `ISCC.exe installer\\NskrySetup.iss` only after the CMake build has
   completed.

The optional packages are not copied directly into the plugin directory.
Setup invokes `Nskry.exe --install-plugin ... --silent --source official`, so
Setup, Settings, drag/drop, and updates share exactly one package-validation
and staging pipeline.

The installer now ships LICENSE, the full GPLv3 COPYING, PLUGIN_EXCEPTION and the third-party
plugin policy. This does not by itself satisfy all corresponding-source or
model-weight attribution obligations: follow the release checklist in
`docs/governance/distribution_and_brand_protection.md`.
`--source official` is installation provenance, not cryptographic certification.
Do not advertise signature verification until the official catalog trust-root
and release-signing work is implemented. Crop/annotation are Core, not an
optional prerequisite plugin. When building in `build/verify`, do not compile
Setup against an old `build/Nskry.exe`; align its source with the tested binary.

For that build directory, pass the tested executable explicitly:

```powershell
$coreExe = (Resolve-Path build/verify/Nskry.exe).Path
ISCC.exe "/DCoreExePath=$coreExe" installer/NskrySetup.iss
```
