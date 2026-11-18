// Project open (.spectrapack). The target itself is Rust,
// desktop/src-tauri/fuzz/project_archive.rs; this unit brings in the libFuzzer
// driver and the AddressSanitizer runtime it is linked with.
extern "C" int LLVMFuzzerInitialize(int*, char***) { return 0; }
