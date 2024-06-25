# sdk
SDK Components

![GCC builds](https://github.com/GremSnoort/sdk/actions/workflows/ubuntu_gcc.yaml/badge.svg)
![Clang builds](https://github.com/GremSnoort/sdk/actions/workflows/ubuntu_clang.yaml/badge.svg)
![WindowsServer builds](https://github.com/GremSnoort/sdk/actions/workflows/windows_server.yaml/badge.svg)

## Build

### Windows MSVC

Git Bash:

```bash
export BUILD_TYPE=Release
export RUNTIME=MD

export BUILD_TYPE=Debug
export RUNTIME=MDd

export FOLDER="./out/build/x64-${BUILD_TYPE}/"

conan install ./conanfile.txt \
	-s:b build_type=${BUILD_TYPE} \
	-s:h build_type=${BUILD_TYPE} \
	--output-folder=${FOLDER} \
	--install-folder=${FOLDER} \
	--generator CMakeToolchain \
	--build=missing \
	-c:b tools.cmake.cmaketoolchain:generator=Ninja \
	-c:h tools.cmake.cmaketoolchain:generator=Ninja \
	-s:b compiler.runtime=${RUNTIME} \
	-s:h compiler.runtime=${RUNTIME}
```
