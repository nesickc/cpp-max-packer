# Pinned vcpkg source assets

`0034c113477f83c28d4380de1ee189c25b1168e6.patch` is the original upstream
patch consumed by `json-schema-validator` `2.3.0#2` at vcpkg baseline
`271a5b8850aa50f9a40269cbf3cf414b36e333d6`.

- Source URL: https://github.com/pboettch/json-schema-validator/commit/0034c113477f83c28d4380de1ee189c25b1168e6.patch
- Expected SHA512: `5c165b50813b0d9937ff0eb4d4a81e2d1e77718ac3b0d02b93931c8eddb4e06e4fae1822c5cc97a5b01c995916a29d0af03fcbcd8f059cb29cfeb0e2371b15e3`
- Provenance: retained from the pinned upstream patch form because the current
  GitHub response changes the abbreviated `index` metadata and therefore its bytes.

`Prepare-VcpkgAssets.ps1` verifies the committed source hash before it creates
or replaces the corresponding vcpkg download-cache entry.
