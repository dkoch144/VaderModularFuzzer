# VMF Build System Documentation

## Overview

## Debug build

On linux, in order to build the project in debug mode, execute the following from the project root directory.

```bash
$ cd build
$ cmake -DCMAKE_BUILD_TYPE=Debug ..
$ make install -j8
```

On windows, a different build type can be provided at the command line (or the debug target type can be selected from the drop-down box at the top of the Visual Studio GUI). 

```powershell
cd build
cmake --build . --target INSTALL --config Debug
```

## Alternate Compiler Support
VMF has been tested on linux to build with either g++ or clang++.  To specify the preferred compiler use the CMAKE_CXX_COMPILER compiler flag.

```bash
$ cmake -DCMAKE_CXX_COMPILER=g++ ..
```

```bash
$ cmake -DCMAKE_CXX_COMPILER=clang++ ..
```

On Windows, VMF has only been tested with the MSVC compilers.

## Generate Doxygen Documentation
 To generate class level Doxygen documentation, run the following commands.  (Doxygen must be installed first.)
 ```bash
 cd docs/doxygen
 doxygen Doxyfile.in
 ```
 Open [docs/doxygen/html/index.html] to browse the resulting html documentation.

 To rebuild the PDF version of the documentation, run the following additional commands:
  ```bash
 cd docs/doxygen/latex
 make
 ```

 Open [docs/doxygen/latex/refman.pdf](./docs/doxygen/latex/refman.pdf) to view the resulting PDF documentation.


## Adding New External Projects

Third party source libraries should be added to vmf/dependencies. There are three steps:

1. Modify or create a CMakeLists.txt file for the project. You may need to disable
   the project's install commands to avoid putting unwanted artifacts in our install.

2. Add the license information to vmf/dependencies/licences

3. Put the add_subdirectory() command in vmf/dependecies/CMakeLists.txt. In general,
   build the library as static where possible so it can be linked into a monolithic 
   VMFFramework.so/VMFFramework.dll.
## Optional .NET assembly instrumenter

Initialize the pinned instrumenter with `git submodule update --init submodules/vmf-dotnet-instrumenter`.
Install the .NET 10 SDK and configure VMF with `-DVMF_ENABLE_DOTNET=ON`. Build
the `vmf_dotnet_instrumenter` target to place the tool and coverage runtime
under `build_artifacts/dotnet` in the build directory. Run
`ctest -R ManagedCoverageInstrumenter --output-on-failure` from that build
directory to verify the tool on the managed fixture.

On Linux and Windows, the same option enables `ManagedStdinExecutor`. Set its `sutArgv`
to the .NET host and the staged harness DLL, for example
`[dotnet, /path/to/staged/Harness.dll]`. The executor starts one process per
testcase and delivers raw bytes on stdin. VMF appends two arguments to
`sutArgv`, first the coverage output path and then a readiness marker path.
After setup and immediately before invoking the synchronous testcase callback,
the harness creates an empty file at the readiness path. The harness handles
expected input rejections and writes its bitmap with `EdgeCoverage.WriteTo`
before exiting successfully. For other callback exceptions, report the
exception and return a nonzero exit code from the harness. This lets VMF
classify the failure without waiting for operating-system crash reporting.
Pair the executor with `AFLFeedback`.

A launch that fails before readiness stops the campaign with a setup error.
After readiness, VMF marks a nonzero exit or abnormal termination as `CRASHED`
and a timeout as `HUNG`. The optional `timeoutInMs` defaults to 5000 and applies
to startup and testcase execution together.

Run `ctest -R ManagedStdinExecutorTest --output-on-failure` to exercise this
through VMF using the repository-owned, staged .NET 10 fixture.
With the Visual Studio generator, add `-C Release` or the configuration you
built to the CTest commands.
