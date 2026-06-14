# Third-Party Notices

## miniz 3.1.1

SuperZip vendors miniz release `3.1.1` for standards-oriented ZIP compatibility.

- Upstream: <https://github.com/richgel999/miniz>
- Tag: `3.1.1`
- Commit: `d10b03cc73475af673df40f06e5cefd1d5f940d9`
- License: MIT, preserved at `third_party/miniz/LICENSE`

SuperZip's AMD GPU-native `.szip` archive mode does not rely on miniz. miniz is
used for `.zip` compatibility only.

Local hardening policy: miniz remains based on upstream 3.1.1, but small
source-compatible edits may be carried when they remove static-analysis findings
without changing public ZIP behavior. Keep each such edit minimal and document it
in the commit that introduces it.
