#===============================================================================
# Vader Modular Fuzzer (VMF)
# Copyright (c) 2021-2026 The Charles Stark Draper Laboratory, Inc.
# <vmf@draper.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 (only) as 
# published by the Free Software Foundation.
#  
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.
#  
# @license GPL-2.0-only <https://spdx.org/licenses/GPL-2.0-only.html>
#===============================================================================

# required by gtest
find_package(Threads REQUIRED)

add_library(VmfTestLib SHARED
    TestConfigInterface.cpp
    ModuleTestHelper.cpp  
    LogFileUtils.cpp
)
target_include_directories(VmfTestLib PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ../../vmf/src/framework/app/
    ./
)
target_link_libraries(VmfTestLib
  PRIVATE
  gtest 
  gtest_main
  VMFFramework
  CoreModules
  Threads::Threads
  yaml-cpp
)

set(TEST_SRCS
  ../../vmf/src/framework/app/ConfigManager.cpp
  ../../vmf/src/framework/app/ModuleManager.cpp
  TestConfigInterfaceTest.cpp
  TestAll.cpp
  ConfigManagerTest.cpp
  StorageRegistryTest.cpp
  StorageModuleTest.cpp
  StorageModuleTestFloat.cpp
  AFLMutatorTest.cpp
  GramatronMutatorTest.cpp
  GAInputGenTest.cpp
  MOPTInputGeneratorTest.cpp
  DictionaryMutatorTest.cpp
  StackedMutatorTest.cpp
  TrivialSeedInitializationTest.cpp
  OutputSchedulerTest.cpp
  SaveCorpusOutputTest.cpp
  MOPTTest.cpp
)

if(WIN32)
   list(APPEND TEST_SRCS
      windows/FridaExecutorTest.cpp
   )
   #windows-specific unit tests
else()
   #linux-specific unit tests
    list(APPEND TEST_SRCS
      linux/AFLForkserverExecutorTest.cpp
      linux/DictionaryInitializationTest.cpp
      linux/RedPawnInputGeneratorTest.cpp
	  )

    find_program(KLEE_EXEC klee)
    if (KLEE_EXEC)
      list(APPEND TEST_SRCS
        linux/KleeInitializationTest.cpp
      )
    endif()
endif()

add_executable(VmfTest ${TEST_SRCS})
if(VMF_ENABLE_DOTNET AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(managed_fixture_dir "${CMAKE_BINARY_DIR}/dotnet/managed-stdin/fixture")
  add_custom_target(vmf_managed_stdin_fixture
    COMMAND "${VMF_DOTNET_EXECUTABLE}" build
      "${CMAKE_CURRENT_SOURCE_DIR}/inputs/ManagedHarness/ManagedHarness.csproj"
      --artifacts-path "${CMAKE_BINARY_DIR}/dotnet/managed-stdin/artifacts"
      --output "${managed_fixture_dir}"
      "-p:CoverageRuntime=${VMF_BUILD_OUTPUT_DIR}/dotnet/Vmf.ManagedCoverage.dll"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
      "${managed_fixture_dir}" "${managed_fixture_dir}-staged"
    COMMAND "${VMF_DOTNET_EXECUTABLE}" "${VMF_BUILD_OUTPUT_DIR}/dotnet/Instrumenter.dll"
      "${managed_fixture_dir}-staged/ManagedHarness.dll"
    VERBATIM)
  add_dependencies(vmf_managed_stdin_fixture vmf_dotnet_instrumenter)
  add_dependencies(VmfTest vmf_managed_stdin_fixture)
  target_sources(VmfTest PRIVATE linux/ManagedStdinExecutorTest.cpp)
  target_compile_definitions(VmfTest PRIVATE
    VMF_TEST_DOTNET="${VMF_DOTNET_EXECUTABLE}"
    VMF_TEST_MANAGED_HARNESS="${managed_fixture_dir}-staged/ManagedHarness.dll")
endif()
target_compile_definitions(VmfTest PRIVATE
  GRAMATRON_TEST_PDA="${CMAKE_CURRENT_SOURCE_DIR}/inputs/gramatron.json")

set_target_properties(VmfTest PROPERTIES LINKER_LANGUAGE CXX)

target_include_directories(VmfTest PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ../../vmf/src/framework/app/
    ./
)

if(WIN32)
   target_compile_definitions(VmfTest PRIVATE PLOG_IMPORT)
endif()

target_link_libraries(VmfTest
  PRIVATE
  VmfTestLib
  PUBLIC
  gtest 
  gtest_main
  VMFFramework
  CoreModules
  Threads::Threads
  PRIVATE
  yaml-cpp
)

# install public headers + lib

set(PUBLIC_TEST_HEADERS TestConfigInterface.hpp ModuleTestHelper.hpp LogFileUtils.hpp)
install(FILES ${PUBLIC_TEST_HEADERS} DESTINATION ${CMAKE_INSTALL_PREFIX}/include/vmf/test COMPONENT CPP_INCLUDES)
install(TARGETS VmfTestLib LIBRARY DESTINATION ${VMF_INSTALL_LIBDIR})

find_program(AFL_CLANG_FAST afl-clang-fast)
find_program(AFL_CLANG_FAST++ afl-clang-fast++)
if (AFL_CLANG_FAST AND AFL_CLANG_FAST++)
    gtest_discover_tests(VmfTest WORKING_DIRECTORY ${CMAKE_BINARY_DIR}  DISCOVERY_MODE PRE_TEST)
else()
    message(WARNING "Unable to build AFL instrumented SUTs, allowing failure across VmfTest")
    gtest_discover_tests(VmfTest 
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR} 
        PROPERTIES SKIP_RETURN_CODE 77
        DISCOVERY_MODE PRE_TEST)
endif()

