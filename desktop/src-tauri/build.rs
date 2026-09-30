fn main() {
    #[cfg(feature = "app")]
    {
        let hash = std::fs::read_to_string("engine-resources/engine.sha256")
            .expect("Stage the fixed engine and notices before building the native desktop");
        println!("cargo:rustc-env=SPECTRAPACK_ENGINE_SHA256={}", hash.trim());
        println!("cargo:rerun-if-changed=engine-resources/engine.sha256");
        tauri_build::build();
    }
}
