// Build procedure is based on esp-idf-sys: https://github.com/esp-rs/esp-idf-sys/blob/155299bde700905fd2ddb040d7a13fb73559ac68/build/native/cargo_driver.rs

use embuild::build::LinkArgsBuilder;
use embuild::cargo;
use embuild::cmake::file_api::{ObjKind, Query};
use embuild::cmake::Config;
use std::{env, path::PathBuf};

fn arm_toolchain_bin() -> PathBuf {
    if let Some(path) = env::var_os("PICO_ARM_TOOLCHAIN_BIN") {
        return path.into();
    }

    PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap())
        .join("../.tools/arm-gnu-toolchain/bin")
}

fn main() {
    println!("cargo:rerun-if-env-changed=WIFI_CONFIG");
    println!("cargo:rerun-if-changed=c");

    let toolchain_bin = arm_toolchain_bin();
    let gcc = toolchain_bin.join("arm-none-eabi-gcc");
    if !gcc.is_file() {
        panic!(
            "complete Arm GNU toolchain not found at {} (set PICO_ARM_TOOLCHAIN_BIN to its bin directory)",
            toolchain_bin.display()
        );
    }

    // Homebrew's compiler-only arm-none-eabi-gcc package has no Newlib. Put
    // the complete local toolchain first for CMake's compiler discovery.
    let path = env::join_paths(
        std::iter::once(toolchain_bin.clone())
            .chain(env::split_paths(&env::var_os("PATH").unwrap_or_default())),
    )
    .unwrap();
    env::set_var("PATH", path);

    let cmake_build_dir = cargo::out_dir().join("build");

    // Set CMake to output API files https://cmake.org/cmake/help/git-stage/manual/cmake-file-api.7.html
    let query = Query::new(&cmake_build_dir, "cargo", &[ObjKind::Codemodel]).unwrap();

    // Build C part
    Config::new("c")
        .target("thumbv8m.main-none-eabi")
        .define("CMAKE_SYSTEM_NAME", "")
        .generator("Ninja")
        .build_target("exe")
        .build();

    // Retrieve information from CMake API files
    let replies = query.get_replies().unwrap();
    let codemodel = replies.get_codemodel().unwrap();
    let exe_target = codemodel
        .into_first_conf()
        .get_target("exe")
        .unwrap()
        .unwrap();
    let link = exe_target.link.unwrap();

    let mut link_args_builder = LinkArgsBuilder::try_from(&link).unwrap();
    link_args_builder
        .linkflags
        .push("-Wl,--wrap=main".to_owned());
    let link_args = link_args_builder
        .force_ldproxy(true)
        .linker(gcc.to_str().unwrap())
        .working_directory(&cmake_build_dir)
        .build()
        .unwrap();
    link_args.output();
    link_args.propagate();
}
