# Supplemental license sources

These small, pinned upstream texts fill gaps in packaged dependency sources.
`supplemental.json` records their package versions and provenance, including
explicit build-generated runtime contributions such as Vite's preload helpers.
They are inputs to `tools/desktop/Generate-ThirdPartyNotices.ps1`, not a manually maintained
inventory of all application dependencies.

Desktop staging generates an ignored `engine-resources/third-party/index.md`
and complete, deduplicated license/NOTICE files. Distribute that directory with
the application. No concatenated notices report is committed to this repository.
