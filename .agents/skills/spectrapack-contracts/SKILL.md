---
name: spectrapack-contracts
description: Implement or review SpectraPack schemas, CLI/stdio service, desktop integration, portable projects, checkpoints, transforms in the viewer, or exports.
---

# Contracts and desktop work

Read selected parts of [spec §8](../../../doc/spectrapack-spec.md#8-interfaces-persistence-and-export) using `./tools/Read-Spec.ps1 -Section 8.1,8.3` from the repository root. Add §7 for user flows, §5.2 for transforms, and relevant DATA/UI rows and AT-13–AT-16 with `-Id`.

Agree schemas and ownership before independent UI/backend edits. C++ owns geometry/feasibility, Rust owns the fixed sidecar and scoped file access, and React presents engine state. Generate TypeScript types from versioned JSON Schemas; schema success does not replace semantic checks.

- Protocol (§8.1): UTF-8 NDJSON; stdout is protocol-only, logs go to stderr. Records carry protocol version and request/event identity; maximum record size is 1 MiB. Transfer large meshes through scoped assets. Deduplicate request IDs; only one active solver job per process.
- State (§8.2): stop acknowledges promptly and retains the last validated incumbent. Publish immutable result revisions; ignore stale job/revision events. Backend/version changes can reset search state without losing valid poses.
- Results (§8.3): count equals placement length; use `best_found`, including valid empty results. Seeds are decimal uint64 strings. Quaternion/translation is canonical and matrices must agree; preserve source mapping through viewer and export.
- Persistence (§8.4): content hashes, portable paths, lossless accepted geometry, atomic archive/checkpoint replacement, and restore validation. Reject traversal, tampering, unsupported versions, and truncation without losing the last complete checkpoint.
- Export (§8.5): stream full-resolution accepted geometry and validate actual quantized STL coordinates per copy. Keep valid JSON/project if STL quantization fails. Never alter poses/clearance to make export pass or treat touching copies as one manifold union.

Use targeted schema, lifecycle, transform round-trip, and recovery cases. For UI-only work, verify the affected interaction visually; screenshots do not prove solid validity. Contract/semantic changes need the §11.3 decision record and compatibility assessment.
