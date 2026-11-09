# Comparator Integrity Metadata Review

The maintainer authorized individually proven false-positive dispositions while
requiring demonstrated defects to be repaired. This review covers only the
`DS173237` matches listed here. These values identify public files or revisions;
they do not authenticate a user or grant access to a service.

The [metadata ledger](../../.github/scanner-metadata-reviews.csv) binds the JSON
values to their exact source-file SHA-256, public value and scanner rule. The
[source ledger](../../.github/scanner-false-positive-reviews.csv) additionally
binds the seven Python literals to their individual alerts, producer, locations
and caller context. Changed source expires those bindings. Raw scanner results
remain retained; another diagnostic never inherits approval by filename or rule.

## Official 7-Zip Binaries

The official [26.06 Extra archive](https://github.com/ip7z/7zip/releases/download/26.06/7z2606-extra.7z)
has publisher SHA-256
`396c180f12d2247c24b0b1fcd482c6fc2a2700f61211ffede1061dd971f5e104`.
The complete download matched that digest and the publisher's byte length.
Independent extraction and hashing established these three values in
`tools/benchmark_permissions.json`:

| Binary | SHA-256 |
| --- | --- |
| `7za.exe` | `dd3757f31c97e33776f2590317d4fb12f2da075db3dbb1e3f94d0f2a01d84ffa` |
| `7za.dll` | `2fcf9cea76d55f16c8c9113b63ffb82c66e96bc679b22459436031c252916551` |
| `7zxa.dll` | `25b19512a0482bca7db4b3e000f41ebe25bed40225dee4ccb86417b7e35a4f57` |

These fields are consumed only as exact binary integrity expectations before
tool execution. They are publicly reproducible hashes of the original files,
not credentials. The original license text is installed and verified separately.

## Unchanged Public Values

The following nine values retain their previously reviewed identities. The
source comparison proved the complete Silesia `EXPECTED` mapping unchanged and
every permission subject other than 7-Zip structurally unchanged. Their source
bindings are renewed because the files now reference the current tool releases.

| Source | Exact field or corpus member | Public identifier |
| --- | --- | --- |
| `tools/benchmark_permissions.json` | Zstandard executable SHA-256 | `8076aae03feac7c66b319579e82172eed168deed2a3f25e5e2d3c60f55e84111` |
| `tools/benchmark_permissions.json` | Public Pythia model revision | `94f7c35d5e9f2e9bac8ca839329f505b4d007d5d` |
| `tools/run_archive_comparison.py` | Silesia `dickens` SHA-256 | `b24c37886142e11d0ee687db6ab06f936207aa7f2ea1fd1d9a36763c7a507e6a` |
| `tools/run_archive_comparison.py` | Silesia `mozilla` SHA-256 | `657fc3764b0c75ac9de9623125705831ebbfbe08fed248df73bc2dc66e2a963b` |
| `tools/run_archive_comparison.py` | Silesia `nci` SHA-256 | `fc63a31770947b8c2062d3b19ca94c00485a232bb91b502021948fee983e1635` |
| `tools/run_archive_comparison.py` | Silesia `ooffice` SHA-256 | `e7ee013880d34dd5208283d0d3d91b07f442e067454276095ded14f322a656eb` |
| `tools/run_archive_comparison.py` | Silesia `samba` SHA-256 | `93ba07bc44d8267789c1d911992f40b089ffa2140b4a160fac11ccae9a40e7b2` |
| `tools/run_archive_comparison.py` | Silesia `xml` SHA-256 | `0e82e54e695c1938e4193448022543845b33020c8be6bf3bf3ead2224903e08c` |
| `tools/run_archive_comparison.py` | Silesia `x-ray` SHA-256 | `7de9fce1405dc44ae5e6813ed21cd5751e761bd4265655a005d39b9685d1c9ad` |

The Zstandard value remains the pinned official standalone executable's hash.
The model revision identifies the public upstream snapshot used by the corpus
controller. The seven Silesia values verify fixed corpus bytes before use; their
associated input lengths are also unchanged. No corpus download, model execution
or performance measurement is required to establish this unchanged-source proof.

### Alert 1450

`DS173237` at `tools/run_archive_comparison.py:41` reports the `x-ray` hash above.
The unchanged tuple verifies public corpus length and bytes before use.

### Alert 1451

`DS173237` at `tools/run_archive_comparison.py:40` reports the `xml` hash above.
The unchanged tuple verifies public corpus length and bytes before use.

### Alert 1452

`DS173237` at `tools/run_archive_comparison.py:39` reports the `samba` hash above.
The unchanged tuple verifies public corpus length and bytes before use.

### Alert 1453

`DS173237` at `tools/run_archive_comparison.py:38` reports the `ooffice` hash above.
The unchanged tuple verifies public corpus length and bytes before use.

### Alert 1454

`DS173237` at `tools/run_archive_comparison.py:37` reports the `nci` hash above.
The unchanged tuple verifies public corpus length and bytes before use.

### Alert 1455

`DS173237` at `tools/run_archive_comparison.py:36` reports the `mozilla` hash above.
The unchanged tuple verifies public corpus length and bytes before use.

### Alert 1456

`DS173237` at `tools/run_archive_comparison.py:35` reports the `dickens` hash above.
The unchanged tuple verifies public corpus length and bytes before use.
