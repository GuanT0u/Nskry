# Nskry Setup

`NskrySetup.iss` creates a per-user installer with Inno Setup 6. It installs
the executable under `%LOCALAPPDATA%\\Programs\\Nskry`; runtime data remains in
`%LOCALAPPDATA%\\Nskry` and is not removed by uninstall.

## Release build

1. Build `..\\build\\Nskry.exe` in Release mode.
2. Optionally place official signed/hashed `.nskryplugin` files in
   `installer\\bundled` using the names in the script.
3. Run `ISCC.exe installer\\NskrySetup.iss`.

The optional packages are not copied directly into the plugin directory.
Setup invokes `Nskry.exe --install-plugin ... --silent --source official`, so
Setup, Settings, drag/drop, and updates share exactly one package-validation
and staging pipeline.
