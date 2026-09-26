# Releasing BiasedDoom

This checklist is for maintainers preparing public builds for users who do not compile from source.

## Release Artifacts

The `Release` GitHub Actions workflow builds from the release tag and publishes:

- `BiasedDoom-<version>-Linux-x86_64.AppImage`
- `BiasedDoom-<version>-Windows-x64.zip`
- `BiasedDoom-<version>-Windows-x64-MinGW.zip`
- `BiasedDoom-<version>-macOS.tar.gz`
- one `.sha256` checksum file beside each artifact

The Linux AppImage is built on Ubuntu 22.04 (Jammy) and bundles its SDL, display-protocol, codec, C++ runtime, OpenAL, GTK, and CPython extension dependencies. `tools/check-appimage-deps.sh` validates the dependency closure, and `tools/smoke-appimage.sh` verifies startup in clean Ubuntu 22.04 and 20.04 containers before the asset can be published.

The native Windows zip is built on Windows with Visual Studio 2022. The MinGW zip is built on Linux with `x86_64-w64-mingw32-g++`. Both Windows packages contain `biaseddoom.exe`, PK3 resources, DLLs copied by the build, soundfonts, FM banks, and a short Windows readme.

## Version Prep

1. Update `CHANGELOG.md`.
2. Bump the version in `src/version.h`.
3. Commit the version/changelog changes.
4. Create and push the matching tag, for example `v4.15.2`.

The workflow verifies that `src/version.h` at the tag matches the release version. A manual workflow run must point at a tag that already exists.

## Recommended Flow

Use the release helper from a clean branch:

```bash
./tools/release.sh --set 4.15.2 --draft
```

Useful variants:

```bash
./tools/release.sh --patch --draft
./tools/release.sh --minor --prerelease
./tools/release.sh --set 4.15.2 --no-workflow
```

If `--no-workflow` is used, run the `Release` workflow manually in GitHub Actions with the same version number.

## Local All-In-One Release

If the remote release workflow is broken, use the local release script from an Ubuntu 22.04 (Jammy) machine, VM, or container. The Jammy host requirement keeps the compiled binary and bundled AppImage SONAMEs in lockstep. The script builds the Linux AppImage, builds the Windows x64 MinGW zip, writes `.sha256` files, validates package contents, can smoke-test with an IWAD, and can publish the GitHub Release directly through `gh`.

Install the local build and publishing tools:

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build git wget curl \
  libsdl2-dev libvpx-dev libwebp-dev libgtk-3-dev \
  mingw-w64 g++-mingw-w64 gcc-mingw-w64 nasm \
  unzip wine gh
gh auth login
```

Build and verify local release artifacts:

```bash
./tools/release-local.sh --version 4.15.2 --clean --iwad /path/to/doom2.wad
```

Publish the verified artifacts as a real non-draft GitHub Release:

```bash
./tools/release-local.sh --version 4.15.2 --skip-build --publish
```

For a one-shot tag push, build, verify, and publish:

```bash
./tools/release-local.sh --version 4.15.2 --create-tag --push-tag --clean --iwad /path/to/doom2.wad --publish
```

By default, `--publish` creates or updates a non-draft, non-prerelease release and marks it as latest. Use `--draft`, `--prerelease`, or `--no-latest` only when that is intentional.

The local script publishes:

- `BiasedDoom-<version>-Linux-x86_64.AppImage`
- `BiasedDoom-<version>-Linux-x86_64.AppImage.sha256`
- `BiasedDoom-<version>-Linux-x86_64.tar.gz`
- `BiasedDoom-<version>-Linux-x86_64.tar.gz.sha256`
- `BiasedDoom-<version>-Windows-x64-MinGW.zip`
- `BiasedDoom-<version>-Windows-x64-MinGW.zip.sha256`

The Linux tarball is an extra portable convenience artifact. The AppImage remains the primary Linux user download.

## Workflow Behavior

The release workflow:

1. Resolves the release version from the tag or manual input.
2. Checks out the exact release tag for every build job.
3. Ensures a usable vcpkg checkout, cloning the repository gitlink commit when needed.
4. Configures CMake with the vcpkg toolchain and glTF enabled.
5. Builds Release on Linux, Windows, and macOS.
6. Cross-compiles a second Windows x64 package on Linux with MinGW-w64.
7. Packages consumer-facing assets and SHA256 checksums.
8. Validates that every non-system AppImage dependency resolves inside the AppDir and smoke-tests startup in clean Ubuntu containers.
9. Publishes a GitHub Release using changelog content when available.

## Local Windows Package

Windows maintainers can make the same style of local zip without opening Visual Studio:

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-windows.ps1 -Configuration Release -Clean -Package
```

The zip is written to `artifacts\BiasedDoom-Windows-x64-Release.zip`.

Linux maintainers can also produce a Windows `.exe` package with MinGW-w64:

```bash
sudo apt install mingw-w64 g++-mingw-w64 gcc-mingw-w64 nasm
./tools/build-windows-mingw.sh --clean --package
```

The zip is written to `artifacts/BiasedDoom-<version>-Windows-x64-MinGW.zip`.

The MinGW helper validates vcpkg's `libvpx.a` before linking. If vcpkg generated a host Linux ELF archive for the MinGW triplet, the helper rebuilds libvpx with `x86_64-w64-mingw32-` tools so the Windows package still keeps VP8/VP9 movie support.

For Wine smoke tests, include `-stdout` so the Windows GUI executable prints startup errors to the terminal. Use `+quit` for an automated launch-and-exit check:

```bash
wine biaseddoom.exe -stdout -iwad doom2.wad +quit
```

Avoid using bare `wine biaseddoom.exe` as the only test because GUI-subsystem Windows failures can appear silent. The `-norun` diagnostic path intentionally pauses before closing; under Wine its `1337` diagnostic exit can appear as shell exit code `57`.

## Sanity Checks

Before publishing a release as non-draft:

- Confirm the Linux asset is an `.AppImage`, not a tarball containing one.
- Run `tools/check-appimage-deps.sh <release>.AppImage`; it must report dependency closure OK.
- Run `tools/smoke-appimage.sh <release>.AppImage ubuntu:22.04` and repeat with `ubuntu:20.04`; both must reach the normal no-IWAD startup message without loader errors.
- Confirm both Windows assets are `.zip` files and contain `biaseddoom.exe` plus PK3 files.
- Confirm every published artifact has a `.sha256` file.
- Download the Windows zip on a clean machine or VM, extract it, and run `biaseddoom.exe -iwad <path-to-iwad>`.
- Download the MinGW Windows zip on a clean machine or VM and run the same smoke test.
- On Linux with Wine, run `wine biaseddoom.exe -stdout -iwad doom2.wad +quit` from inside the extracted Windows package.
- Download the AppImage, run `chmod +x`, and launch it with an IWAD.
