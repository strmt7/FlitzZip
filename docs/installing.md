# Install FlitzZip

FlitzZip targets Windows 11 x64. Published builds are listed on
[GitHub Releases](https://github.com/strmt7/FlitzZip/releases). Choose the
portable ZIP or MSI from the same release and read its known limitations.
If no package is published, the [source build](development/building.md) is the
available development path.

**Prerelease software:** keep original files and verify archives before using
them as your only copy of important data.

## Check requirements

GPU operations require a supported AMD GPU and a compatible driver providing
the HIP runtime expected by the package. Consult the
[GPU targets](release.md#gpu-targets) and AMD's
[Windows compatibility information](https://rocm.docs.amd.com/en/docs-10.1.0/compatibility/compatibility-matrix.html).
The application reports whether the runtime, device and kernel module are usable.
CPU functions remain available when GPU prerequisites are missing; Neutron star
mode requires a compatible GPU.

The ROCm development SDK is needed to build FlitzZip, rather than to use an
already-built package. Runtime installation details are in
[portability and prerequisites](portability.md#runtime-prerequisites).

## Portable ZIP

1. Download the portable ZIP and its SHA-256 sidecar from the chosen release.
2. Calculate the downloaded file's hash in PowerShell, replacing `<version>`
   with the version you downloaded:

   ```powershell
   Get-FileHash -Algorithm SHA256 -LiteralPath ".\FlitzZip-<version>-win64-portable.zip"
   ```

3. Compare the result with the release's sidecar, then extract the complete ZIP
   into a writable folder.
4. Start `bin\FlitzZip.exe`. Keep the complete extracted directory structure,
   including the accompanying libraries and runtime manifest.
   `bin\flitzzip_cli.exe` provides the command-line interface.

A matching checksum detects changed download bytes. Publisher authenticity also
depends on obtaining the package and checksum from the intended release source.

## MSI installer

Download the MSI and its SHA-256 sidecar from the same release. Verify its hash,
then run the MSI. The standard installation uses `Program Files` and may request
Windows administrator approval. A desktop shortcut is optional.

Run the same MSI again to access its maintenance actions. Use Windows
**Settings > Apps** to uninstall the MSI installation.
To upgrade, close FlitzZip and run the newer MSI directly; intermediate releases
are not required. Keep the installation scope unchanged. Setup rejects an older
version when a newer one is installed. Repair uses the matching installed
version's MSI and restores package-owned files.

Preferences and archives survive MSI repair, upgrade and uninstall. Preferences
are shared with the portable application under the current user's Local AppData
`FlitzZip` directory. A new profile generation preserves older profile files;
the application migrates supported older settings when its new profile is
absent. Removing a portable directory leaves those preferences intact.
The portable ZIP and MSI contain the same HIP-enabled application binaries.
For troubleshooting, retain the release version and any installer error before
retrying.

Continue with [creating and extracting archives](using-flitzzip.md).
