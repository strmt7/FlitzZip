# Settings and upgrade compatibility

This is the implementation contract for persistent preferences and installer
upgrades. It applies to every future release. A supported old release must
upgrade directly to a newer release without installing intermediate versions.
That is a release invariant, verified against retained historical inputs; it is
not a claim that unimplemented future migrations are already correct.

## Storage ownership

The application resolves the current user's Local AppData through Windows
Known Folders. It does not store preferences beside the executable or require
administrator access to save them. MSI owns installed binaries and selected
shortcuts. It does not own user archives, preferences or diagnostic data.

| Data | Location and lifetime |
| --- | --- |
| Current preferences | `FlitzZip/settings-v4.json` under Local AppData; UTF-8 JSON, generation identified in both filename and `schema` |
| Recovery copy | The current profile filename followed by `.bak`; preceding validated bytes, replaced before the next current-profile publication |
| Writer lock | The profile filename followed by `.lock`; an empty persistent coordination file, with exclusivity held by an open OS handle |
| Earlier generations | Retained in the same directory; original `settings.json` and the predecessor product directory are read-only migration inputs |
| Diagnostics | `FlitzZip/flitzzip.log`; diagnostic text, not an application state database |
| Queue, history and drafts | Session state; not a durable user-data format |
| Archives | User-selected paths; never MSI cleanup targets |

The portable and installed applications use the same per-user preferences.
Changing or uninstalling binaries leaves user data intact. Removing the portable
directory does not reset preferences. Windows describes the distinction between
[per-user application data](https://learn.microsoft.com/en-us/windows/apps/design/app-settings/store-and-retrieve-app-data)
and [installer ownership and context](https://learn.microsoft.com/en-us/windows/win32/msi/installation-context).

## Reading and saving preferences

`resources/settings/compatibility.json` registers readable generations,
migration paths, retired field identifiers and coupled preference groups.
The public JSON Schema describes canonical current values. Frozen schemas and
authored profile fixtures live under `tests/fixtures/settings/`; keep published
fixtures unchanged and add new evidence beside them.

Persist semantic identifiers and explicit units: format extensions, block bytes,
retention days and severity names. UI positions are transient. Legacy position
tables remain frozen in the decoder, independent of current dropdown ordering.
Retired names are never reassigned to another meaning. This follows the same
identity discipline as Google's
[schema evolution guidance](https://protobuf.dev/best-practices/dos-donts/);
FlitzZip uses JSON rather than introducing a Protocol Buffers dependency.

The OS JSON parser validates the complete bounded document before fields are
applied. Recognized fields require their documented scalar types; invalid
individual values use explicit safe defaults. An absent file selects defaults.
A malformed document, unsupported generation or I/O error is reported and must
not be overwritten by startup, Apply or Reset. The recovery copy is available
for deliberate recovery; it is not silently substituted for damaged input.

Unknown fields and nested values survive saves. Their original JSON member
bytes preserve large integers and numeric spellings without conversion through
floating-point values. Unknown data remains opaque and never enables behavior.
This is the JSON extension-data pattern described by
[Microsoft](https://learn.microsoft.com/en-us/dotnet/standard/serialization/system-text-json/handle-overflow)
and the [JSON Schema object contract](https://json-schema.org/understanding-json-schema/reference/object).
The decoder retains its 64 KiB and 64-container-depth limits; future schema work
must evaluate these budgets explicitly rather than assuming unlimited storage.

Startup does not rewrite a valid current-generation document. Apply changes only
preferences that differ from the applied UI baseline. Runtime normalization,
such as a temporarily unavailable GPU, must not erase saved intent on an
unrelated edit. Changes to format, compression level, Neutron selection or GPU
requirement publish that entire coupled group consistently. Reset deliberately
resets known current preferences while preserving opaque extension data.

Saving requires an exact expected revision, including explicit absence for a new
profile. A nonblocking OS handle serializes cooperating writers across processes;
a stale or busy writer reports an error and requires reopening the app. The
application validates existing data, publishes its recovery copy, then publishes
the new document through the shared private file transaction. A failed backup
prevents replacement. Staging is closed, flushed through `FlushFileBuffers` and
renamed through a pinned destination; failed saves retain the current document.
See Microsoft's [flush contract](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers).
This does not promise recovery from storage hardware that fails to honor writes.
Do not manually edit the profile while an application instance is running.

## Adding, renaming or removing preferences

1. Define the durable key, scalar type, units, default, bounds and coupled
   relationships in the schema and registry before changing the serializer.
   A new independent optional field can remain in the current generation when
   older readers safely preserve and ignore it.
2. Renaming, deleting, changing meaning, type, bounds, enum values, defaults or
   required fields is a breaking change. Allocate a new profile generation;
   never reuse retired keys. Software version and profile generation are separate.
3. Add an explicit decoder/migration from every readable historical generation,
   or a complete forward-only path to the new generation. Read the newest
   existing supported older profile only when the new profile is absent.
   Preserve old bytes. Never consume a newer unknown profile or rewrite an older
   filename with a new schema. Older binaries therefore retain their own profile.
   Add each preceding versioned filename to `kPreviousSettingsFilenames` in
   `settings_profile.hpp`, newest first. The evolution gate checks this native
   lookup order against every frozen earlier schema; omitted versions fail.
4. Keep frozen fixtures and schema baselines append-only. Add independent examples
   for skipped releases, missing/invalid fields, Unicode, precise opaque values,
   retired identifiers and coupled settings. Test the real serializer/decoder,
   not just schema syntax or a round-trip oracle.
5. Run the compatibility contracts, native persistence tests and hidden-window
   startup/Apply tests. Exercise write, flush, backup and replacement failures,
   simultaneous and stale writers, successful retry and byte-exact old-data
   preservation. Update user recovery instructions and qualify the package.

`settings-evolution-contracts` runs through change-aware verification and hosted
component contracts. It rejects drift in frozen field signatures, historical
fixture edits/deletions, missing migration paths and unregistered serialized
keys. Native behavior tests remain required: a registry is not an implementation.

## Installer identities and version order

Keep the published x64 per-machine scope and UpgradeCode
`8E0D80F6-859D-4FE9-B082-F0D8048A8B57`. ProductCode changes with the version or
explicit build identity; existing deterministic identity tests remain normative.
Do not change the product family, installation scope, feature identifiers or
component ownership casually. Cross-scope or architecture migrations require
their own explicit design and fixtures. Microsoft documents
[major upgrades](https://learn.microsoft.com/en-us/windows/win32/msi/major-upgrades)
and [ProductCode changes](https://learn.microsoft.com/en-us/windows/win32/msi/productcode).

The existing explicit per-user developer package remains available for isolated
non-admin testing. Its compiled scope is checked separately; it must not be
published as the per-machine product release. Both contexts use HKMU shortcut
markers. Do not migrate a product between contexts implicitly.

MSI compares exactly three numeric fields, limited to `255.255.65535`.
Prerelease suffixes cannot express installer ordering; use an increasing numeric
version and the beta release track. Portable developer builds may use SemVer
suffixes. The package entry point and native CPack policy reject ambiguous or
out-of-range MSI versions instead of truncating them. See
[ProductVersion](https://learn.microsoft.com/en-us/windows/win32/msi/productversion).
An authorized same-version replacement uses a distinct ProductCode and retains
the existing explicit release-replacement approval requirement.

Inspect the compiled MSI, not just authoring text. Older/equal versions must be
covered without a lower-version cutoff; newer versions must be detected and
rejected. Feature choices migrate. `RemoveExistingProducts` stays after
`InstallInitialize`, inside the rollback transaction, before installing new
components. Shortcut markers use HKMU so their registry ownership matches the
installation context. References:
[WiX MajorUpgrade](https://docs.firegiant.com/wix/schema/wxs/majorupgrade/),
[RegistryValue](https://docs.firegiant.com/wix/schema/wxs/registryvalue/) and
[Microsoft upgrade table](https://learn.microsoft.com/en-us/windows/win32/msi/upgrade-table).

## Qualifying installer changes

`tools/test_installer_lifecycle.ps1` defaults to non-installing authoring checks.
Change-aware local and hosted component contracts select these checks when
installer authoring or its shared tools change. Missing WiX is provisioned into
the repository's ignored tool cache through the pinned installer.
It creates tiny protocol fixtures using the pinned production CPack/WiX policy,
feature tree and shortcut components, then inspects their compiled MSI tables.
The fixture payloads are labelled test data, not executable FlitzZip packages.
The separate actual-package smoke verifies production binaries and repair.

The existing release workflow also runs:

```powershell
tools/test_installer_lifecycle.ps1 -Full -DisposableMachine
```

Run this mode only on a disposable elevated Windows VM/hosted runner with no
related installed product. Never run it on a development or customer machine.
It performs fresh install, a direct large version jump, sequential upgrades,
downgrade rejection, a deliberately failed deferred upgrade with rollback,
authorized same-version replacement, missing-file repair and uninstall. Assert
exact payloads, obsolete-file removal, one product registration, retained feature
choices and unchanged independent user data after each phase. Every future
release must pass both this protocol matrix and the exact-package smoke before
publication. Retain logs and package/source identities with qualification evidence.
