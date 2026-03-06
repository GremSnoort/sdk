#!/usr/bin/env bash
set -euo pipefail

# Usage:
#   ./build.sh Release
#   ./build.sh Debug
#   BUILD_TYPE=Release ./build.sh

required_tools=(conan cmake ninja nproc)
for tool in "${required_tools[@]}"; do
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "ERROR: required tool '${tool}' is not found in PATH."
    exit 1
  fi
done

input_build_type="${1:-${BUILD_TYPE:-}}"

if [[ -z "${input_build_type}" ]]; then
  echo "ERROR: build type is not set."
  echo "Pass it as argument or environment variable BUILD_TYPE."
  echo "Examples: ./build.sh Release | BUILD_TYPE=Debug ./build.sh"
  exit 1
fi

case "${input_build_type,,}" in
  release)
    BUILD_TYPE="Release"
    CONAN_PROFILE="gcc_release"
    BUILD_DIR="build/Release"
    CONAN_DEPS_BUILD_TYPE="Release"
    ;;
  debug)
    BUILD_TYPE="Debug"
    CONAN_PROFILE="gcc_debug"
    BUILD_DIR="build/Debug"
    # ffmpeg/7.0.1 from ConanCenter does not provide Debug package variants.
    # Keep app build in Debug, but resolve Conan dependencies as Release.
    CONAN_DEPS_BUILD_TYPE="Release"
    ;;
  *)
    echo "ERROR: unsupported build type '${input_build_type}'. Use Release or Debug."
    exit 1
    ;;
esac

conan_version="$(conan --version)"
echo "${conan_version}"
if [[ "${conan_version}" != Conan\ version\ 1.* ]]; then
  echo "ERROR: this script expects Conan 1.x (recommended 1.66.0)."
  exit 1
fi

echo ">>> Preparing Conan profiles"

conan profile new ${CONAN_PROFILE} --detect --force
conan profile update settings.compiler.libcxx=libstdc++ ${CONAN_PROFILE}
conan profile update settings.build_type=${CONAN_DEPS_BUILD_TYPE} ${CONAN_PROFILE}

echo ">>> Using build type: ${BUILD_TYPE}"
echo ">>> Using profile: ${CONAN_PROFILE}"
echo ">>> Conan dependencies build type: ${CONAN_DEPS_BUILD_TYPE}"
echo ">>> Conan install folder: ${BUILD_DIR}"

if [[ "${BUILD_TYPE}" == "Debug" && "${CONAN_DEPS_BUILD_TYPE}" == "Release" ]]; then
  echo ">>> NOTE: building project in Debug with Release Conan dependencies (ffmpeg limitation)."
fi

conan install ./conanfile.txt \
  --profile "${CONAN_PROFILE}" \
  -s:b build_type="${CONAN_DEPS_BUILD_TYPE}" \
  -s:h build_type="${CONAN_DEPS_BUILD_TYPE}" \
  --output-folder="${BUILD_DIR}" \
  --install-folder="${BUILD_DIR}" \
  --generator CMakeToolchain \
  --generator CMakeDeps \
  --build=missing \
  -c:b tools.cmake.cmaketoolchain:generator=Ninja \
  -c:h tools.cmake.cmaketoolchain:generator=Ninja

toolchain_candidates=(
  "${BUILD_DIR}/generators/conan_toolchain.cmake"
  "${BUILD_DIR}/conan_toolchain.cmake"
  "${BUILD_DIR}/build/${BUILD_TYPE}/generators/conan_toolchain.cmake"
  "${BUILD_DIR}/build/${CONAN_DEPS_BUILD_TYPE}/generators/conan_toolchain.cmake"
)

toolchain_file=""
for candidate in "${toolchain_candidates[@]}"; do
  if [[ -f "${candidate}" ]]; then
    toolchain_file="${candidate}"
    break
  fi
done

if [[ -z "${toolchain_file}" ]]; then
  toolchain_file="$(find "${BUILD_DIR}" -type f -name conan_toolchain.cmake 2>/dev/null | head -n 1 || true)"
fi

if [[ -z "${toolchain_file}" || ! -f "${toolchain_file}" ]]; then
  echo "ERROR: conan toolchain file not found in '${BUILD_DIR}'."
  exit 1
fi

echo ">>> Conan toolchain file: ${toolchain_file}"

cmake_config_map_args=()
if [[ "${BUILD_TYPE}" == "Debug" && "${CONAN_DEPS_BUILD_TYPE}" == "Release" ]]; then
  cmake_config_map_args+=(
    -DCMAKE_MAP_IMPORTED_CONFIG_DEBUG=Release
    -DCMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO=Release
    -DCMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL=Release
  )
fi

echo ">>> Configuring CMake"
cmake -S . -B "${BUILD_DIR}" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DCMAKE_TOOLCHAIN_FILE="${toolchain_file}" \
  "${cmake_config_map_args[@]}"

echo ">>> Building"
cmake --build "${BUILD_DIR}" -j"$(nproc)"

echo ">>> Done: ${BUILD_TYPE} build is available in '${BUILD_DIR}'"
