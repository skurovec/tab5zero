fn main() {
    println!("cargo:rerun-if-changed=native/linux.c");
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("linux") {
        panic!("tab5-host requires Linux fbdev and FunctionFS gadget APIs");
    }
    cc::Build::new()
        .file("native/linux.c")
        .flag("-std=c11")
        .flag("-Wall")
        .flag("-Wextra")
        .compile("tab5_linux");
    println!("cargo:rustc-link-lib=dl");
}
