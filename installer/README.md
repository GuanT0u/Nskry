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
