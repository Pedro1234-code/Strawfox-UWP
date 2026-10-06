#!/bin/bash
# Build the Rust std library from source for the selected UWP target and install the
# produced rlibs into the toolchain's sysroot so rustc finds std directly (the
# target ships no prebuilt std).
#
# The toolchain is the one `rustc` resolves to -- the one the engine build will
# use -- unless GECKO_W10M_RUST_TOOLCHAIN names another. It must be a nightly
# with the rust-src component (rustup component add rust-src). Run this again
# whenever that nightly changes: std built by one rustc cannot be linked by
# another.
#
# The x64 target is built into rustc. This script still builds its standard
# library from rust-src because the Gecko build uses build-std and needs the
# resulting rlibs available in the selected nightly's sysroot.
set -e
source "$(dirname "$0")/env.sh"
export CC_x86_64_uwp_windows_msvc="clang-cl"
export CFLAGS_x86_64_uwp_windows_msvc="--target=x86_64-pc-windows-msvc"
export RUSTFLAGS="-Cembed-bitcode=yes -Cpanic=abort -Zunstable-options"
TARGET="${GECKO_W10M_RUST_TARGET:-x86_64-uwp-windows-msvc}"
TC=()
[ -n "$GECKO_W10M_RUST_TOOLCHAIN" ] && TC=("+$GECKO_W10M_RUST_TOOLCHAIN")
SYSROOT_DIR="$(cygpath -u "$(rustc "${TC[@]}" --print sysroot)")/lib/rustlib/$TARGET"
echo "toolchain: $(rustc "${TC[@]}" -V)"

# Install the target description first. rustc looks for a custom target in
# RUST_TARGET_PATH and in <sysroot>/lib/rustlib/<target>/target.json, and only
# the second one survives the trip into the build: make exports reach cargo, but
# msys2 rewrites any variable whose name ends in PATH, and the build never sees
# the value that was set. The sysroot needs no environment at all.
mkdir -p "$SYSROOT_DIR"
if [ -f "$GECKO_W10M_ROOT/mozconfig/rust-targets/$TARGET.json" ]; then
  cp "$GECKO_W10M_ROOT/mozconfig/rust-targets/$TARGET.json" "$SYSROOT_DIR/target.json"
fi

WORK="$(mktemp -d -p "$TMPDIR")"; cd "$WORK"
cargo "${TC[@]}" new --bin stdbuild > /dev/null 2>&1; cd stdbuild
printf '[profile.dev]\npanic = "abort"\n' >> Cargo.toml
cargo "${TC[@]}" build --release -Z build-std=std,panic_abort --target "$TARGET" || true
SYSROOT="$SYSROOT_DIR/lib"
mkdir -p "$SYSROOT"
cp target/"$TARGET"/release/deps/*.rlib "$SYSROOT"/
echo "Installed $(ls "$SYSROOT"/*.rlib | wc -l) std rlibs into $SYSROOT"
