#!/usr/bin/env bash
# CorsixTH-iOS @build 2026-09-06 cross-build every native dependency for iOS with vcpkg
#
# Produces the dependency set declared in the repository's vcpkg.json for one or both iOS
# triplets, then verifies the produced static libraries really are arm64 iOS binaries built
# against the pinned deployment target.
#
# Usage:
#   scripts/build/ios/fetch-deps.sh                 # device only (arm64-ios-min16)
#   scripts/build/ios/fetch-deps.sh simulator       # simulator only
#   scripts/build/ios/fetch-deps.sh device simulator
#   scripts/build/ios/fetch-deps.sh all
#
# Prerequisites: Xcode with the iOS SDK, cmake, ninja, and VCPKG_ROOT pointing at a
# bootstrapped vcpkg checkout.  Nothing is written inside the source tree except
# build/ios-deps/, which is git-ignored.

set -euo pipefail

readonly DEPLOYMENT_TARGET=16.0
readonly DEVICE_TRIPLET=arm64-ios-min16
readonly SIMULATOR_TRIPLET=arm64-ios-simulator-min16

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly REPO_ROOT="$(cd "${script_dir}/../../.." && pwd)"
readonly OVERLAY_TRIPLETS="${REPO_ROOT}/CMake/ios/triplets"
readonly INSTALL_ROOT="${REPO_ROOT}/build/ios-deps"

# vcpkg manifest mode owns its install root exclusively: installing triplet B into a root that
# already holds triplet A removes A.  Each triplet therefore gets its own root, and the
# CMakePresets `VCPKG_INSTALLED_DIR` values must match these.
install_root_for_triplet() {
  case "$1" in
    "${SIMULATOR_TRIPLET}") echo "${INSTALL_ROOT}/simulator" ;;
    *) echo "${INSTALL_ROOT}/device" ;;
  esac
}

if [[ -z "${VCPKG_ROOT:-}" ]]; then
  echo "error: VCPKG_ROOT is not set; point it at a bootstrapped vcpkg checkout" >&2
  exit 1
fi
readonly VCPKG="${VCPKG_ROOT}/vcpkg"
if [[ ! -x "${VCPKG}" ]]; then
  echo "error: ${VCPKG} is missing or not executable; run ${VCPKG_ROOT}/bootstrap-vcpkg.sh" >&2
  exit 1
fi

case "$(uname -m)" in
  arm64) readonly HOST_TRIPLET=arm64-osx ;;
  x86_64) readonly HOST_TRIPLET=x64-osx ;;
  *) echo "error: unsupported host architecture $(uname -m)" >&2; exit 1 ;;
esac

triplets=()
if [[ $# -eq 0 ]]; then
  triplets=("${DEVICE_TRIPLET}")
else
  for arg in "$@"; do
    case "${arg}" in
      device|ios-device|"${DEVICE_TRIPLET}") triplets+=("${DEVICE_TRIPLET}") ;;
      simulator|ios-simulator|"${SIMULATOR_TRIPLET}") triplets+=("${SIMULATOR_TRIPLET}") ;;
      all|both) triplets+=("${DEVICE_TRIPLET}" "${SIMULATOR_TRIPLET}") ;;
      *) echo "error: unknown target '${arg}' (want device|simulator|all)" >&2; exit 1 ;;
    esac
  done
fi

# Expected LC_BUILD_VERSION platform id, as printed by `otool -l`.
# (2 == PLATFORM_IOS, 7 == PLATFORM_IOSSIMULATOR.  `vtool -show-build` prints these by
# name but refuses to read a static archive, so we parse otool instead.)
platform_for_triplet() {
  case "$1" in
    "${SIMULATOR_TRIPLET}") echo "7" ;;
    *) echo "2" ;;
  esac
}

install_triplet() {
  local triplet="$1"
  echo "==> vcpkg install ${triplet} (host ${HOST_TRIPLET})"
  "${VCPKG}" install \
    --triplet "${triplet}" \
    --host-triplet "${HOST_TRIPLET}" \
    --overlay-triplets "${OVERLAY_TRIPLETS}" \
    --x-manifest-root="${REPO_ROOT}" \
    --x-install-root="$(install_root_for_triplet "${triplet}")"
}

# Global Constraint 7: verify the artefacts, not the exit code.  A dependency that silently
# fell back to the host architecture, to macOS, or to the SDK's default deployment target is a
# failure even though vcpkg returned 0.
verify_triplet() {
  local triplet="$1"
  local prefix="$(install_root_for_triplet "${triplet}")/${triplet}"
  local want_platform
  want_platform="$(platform_for_triplet "${triplet}")"
  local failures=0

  echo "==> verifying ${prefix}"
  if [[ ! -d "${prefix}/lib" ]]; then
    echo "  FAIL: ${prefix}/lib does not exist" >&2
    return 1
  fi

  local lib
  for lib in "${prefix}"/lib/*.a; do
    [[ -e "${lib}" ]] || continue
    local arch_line minos platform
    arch_line="$(lipo -info "${lib}")"
    if [[ "${arch_line}" != *arm64* || "${arch_line}" == *x86_64* ]]; then
      echo "  FAIL $(basename "${lib}"): ${arch_line}" >&2
      failures=$((failures + 1))
      continue
    fi
    # One LC_BUILD_VERSION record per archive member; every member must agree.
    local load_commands
    load_commands="$(otool -l "${lib}")"
    platform="$(printf '%s\n' "${load_commands}" | awk '$1 == "platform" { print $2 }' | sort -u | tr '\n' ' ')"
    minos="$(printf '%s\n' "${load_commands}" | awk '$1 == "minos" { print $2 }' | sort -u | tr '\n' ' ')"
    platform="${platform% }"
    minos="${minos% }"
    if [[ "${platform}" != "${want_platform}" || "${minos}" != "${DEPLOYMENT_TARGET}" ]]; then
      echo "  FAIL $(basename "${lib}"): platform='${platform}' minos='${minos}'" \
           "(want '${want_platform}' / '${DEPLOYMENT_TARGET}')" >&2
      failures=$((failures + 1))
      continue
    fi
    printf '  ok   %-24s %s  platform %s  minos %s\n' \
      "$(basename "${lib}")" "${arch_line##*architecture: }" "${platform}" "${minos}"
  done

  # CorsixTH/Src/sdl_core.cpp needs the SDL3 pinch gesture events.
  if grep -q 'SDL_EVENT_PINCH_BEGIN' "${prefix}/include/SDL3/SDL_events.h"; then
    echo "  ok   SDL3/SDL_events.h exports SDL_EVENT_PINCH_BEGIN"
  else
    echo "  FAIL SDL3/SDL_events.h has no SDL_EVENT_PINCH_BEGIN" >&2
    failures=$((failures + 1))
  fi

  local required
  for required in libSDL3.a libSDL3_mixer.a liblua.a liblfs.a liblpeg.a libfreetype.a \
                  libpng.a libz.a; do
    if [[ ! -f "${prefix}/lib/${required}" ]]; then
      echo "  FAIL ${required} was not installed" >&2
      failures=$((failures + 1))
    fi
  done

  if [[ ${failures} -ne 0 ]]; then
    echo "error: ${failures} verification failure(s) for ${triplet}" >&2
    return 1
  fi
  echo "==> ${triplet} verified"
}

for triplet in "${triplets[@]}"; do
  install_triplet "${triplet}"
  verify_triplet "${triplet}"
done

cat <<EOF

Dependencies installed under ${INSTALL_ROOT}.
Next: cmake --preset ios-device   (or ios-simulator)
EOF
