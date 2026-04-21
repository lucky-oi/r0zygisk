# Changelog

All notable changes to this project will be documented in this file.

## v1.0.3

### Notes

+ r0zygisk first release

## v1.0.1

### Added

+ Add GitHub Actions CI workflow for automated build and artifact upload

### Changed

+ Align CI with the current repository layout and `main` branch
+ Keep release/debug package naming consistent with `r0zygisk`

## v1.0.0

### Added

+ Initial public open source release of `r0zygisk`
+ English and Simplified Chinese README documents
+ Build instructions, installation guide, FAQ, acknowledgements and third-party dependency notes
+ Vendored `lsplt` source in the repository for direct builds without submodules

### Changed

+ Clean repository layout for public release
+ Remove local build artifacts, machine-specific files and embedded git metadata from vendored source
+ Make the project buildable in a clean environment with JDK 17, Android SDK/NDK and Rust nightly

### Notes

+ Release packages can be built with `./gradlew zipRelease`
+ If `module/private_key` and `module/public_key` are absent, generated packages are unsigned by design
