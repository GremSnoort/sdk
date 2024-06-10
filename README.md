# sdk
SDK Components

## Build

### Windows MSVC

Git Bash:

```bash
export BUILD_TYPE=Release
export FOLDER="./out/build/x64-${BUILD_TYPE}/"
export RUNTIME=MD

conan install ./conanfile.txt \
	-s build_type=${BUILD_TYPE} \
	--output-folder=${FOLDER} \
	--install-folder=${FOLDER} \
	--generator CMakeToolchain \
	--build=missing \
	-c:b tools.cmake.cmaketoolchain:generator=Ninja \
	-c:h tools.cmake.cmaketoolchain:generator=Ninja \
	-s compiler.runtime=${RUNTIME}
```
