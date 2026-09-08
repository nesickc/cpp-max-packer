# CPU dependencies

The Windows CPU build uses the locked `x64-windows-static` graph in [dependencies.lock.json](../cmake/dependencies.lock.json), with toolchain profiles in [toolchains.lock.json](../cmake/toolchains.lock.json). Build and staging instructions are in [BUILDING.md](BUILDING.md). The vcpkg baseline, port-tree hashes and source archive SHA512 values are the reproducibility record. Port-tree identities include the port's patches; upstream source refs are listed below.

| Port | Version | Upstream ref | License | Role |
| --- | --- | --- | --- | --- |
| manifold | 3.2.1#1 | `elalish/manifold` `v3.2.1` | Apache-2.0 | geometry/booleans |
| fcl | 0.7.0#4 | `flexible-collision-library/fcl` `0.7.0` | BSD (upstream notice) | collision queries |
| meshoptimizer | 0.25 | `zeux/meshoptimizer` `v0.25` | MIT | mesh processing |
| pocketfft | 2023-09-25 | `mreineck/pocketfft` `9efd4da52cf8d28d14531d14e43ad9d913807546` | BSD-3-Clause | CPU FFT |
| nlohmann-json | 3.12.0#1 | `nlohmann/json` `v3.12.0` | MIT | JSON contracts |
| clipper2 | 1.5.4 | `AngusJohnson/Clipper2` `Clipper2_1.5.4` | BSL-1.0 | Manifold dependency |
| tbb | 2022.2.0 | `oneapi-src/oneTBB` `v2022.2.0` | Apache-2.0 | Manifold dependency |
| ccd | 2.1#4 | `danfis/libccd` `v2.1` | BSD-3-Clause | FCL dependency |
| eigen3 | 3.4.1#1 | `libeigen/eigen` `3.4.1` | MPL-2.0 | FCL dependency |
| octomap | 1.10.0 | `OctoMap/octomap` `v1.10.0` | BSD-3-Clause | FCL dependency |
| catch2 | 3.11.0 | `catchorg/Catch2` `v3.11.0` | BSL-1.0 | test framework |

The staged diagnostic preserves original upstream notices at `<install-prefix>/share/licenses/<port>/copyright`; they are the license record. FCL's port manifest has no SPDX license field, but its installed notice contains the upstream BSD license. The 11 target library notices above are separate from the host-only build utilities `vcpkg-cmake` (2025-08-07) and `vcpkg-cmake-config` (2024-05-23), which do not ship as runtime libraries.
