# Necessary dependencies for the build system
# Keep installs compatible with rust-toolchain.toml instead of using new releases.
CARGO_AXPLAT_VERSION := 0.2.4
AXCONFIG_GEN_VERSION := 0.2.1
CARGO_BINUTILS_VERSION := 0.4.0

# Tool to parse information about the target package
ifeq ($(shell cargo axplat --version 2>/dev/null),)
  $(info Installing cargo-axplat...)
  $(shell cargo install cargo-axplat --version $(CARGO_AXPLAT_VERSION) --locked)
endif

# Tool to generate platform configuration files
ifeq ($(shell axconfig-gen --version 2>/dev/null),)
  $(info Installing axconfig-gen...)
  $(shell cargo install axconfig-gen --version $(AXCONFIG_GEN_VERSION) --locked)
endif

# Cargo binutils
ifeq ($(shell cargo install --list | grep cargo-binutils),)
  $(info Installing cargo-binutils...)
  $(shell cargo install cargo-binutils --version $(CARGO_BINUTILS_VERSION) --locked)
endif
