# WiX UI Namespace Review

DevSkim `DS137138` identifies the literal WiX v4 UI XML namespace in
`Get-FlitzZipWixUiArgument` in `tools/package_runtime.ps1`. This is an XML
namespace identifier, not a download or connection endpoint. Replacing its
scheme changes the namespace and breaks WiX source compilation.

The function returns documented CPack definitions. It supplies the exact
installed UI extension DLL by absolute path, disables CPack's additional
versionless UI-extension selection, and declares the original UI namespace.
CPack consumes these definitions when generating WiX XML. No HTTP client,
redirect handler or network request consumes the reported string.

The reviewed WiX 7.0.1 extension and CMake 4.4.5 CPack generator use this exact
namespace. The generated MSI qualification checks the compiled installer
contract. CLI identity, environment ownership and failure propagation have
independent regressions in `tools/test_package_runtime.ps1`; installer authoring
uses the same helper and extension selection as production packaging.

This is an individually proven false positive under the maintainer's standing
authorization. Retain the raw report and bind any hosted disposition to its
actual alert identity, exact source/location, scanner and consumer context.
This review grants no disposition to another HTTP literal or network operation.

The [CPack WiX generator reference](https://cmake.org/cmake/help/v4.4/cpack_gen/wix.html)
documents `CPACK_WIX_EXTENSIONS`, `CPACK_WIX_SKIP_WIX_UI_EXTENSION` and
`CPACK_WIX_CUSTOM_XMLNS`. The
[WiX extension loader](https://github.com/wixtoolset/wix/blob/v7.0.1/src/wix/WixToolset.Core/ExtensibilityServices/ExtensionManager.cs)
loads an absolute extension path directly, before any named-cache lookup.
