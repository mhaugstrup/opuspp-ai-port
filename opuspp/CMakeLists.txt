cmake_minimum_required(VERSION 3.21)
project(opuspp VERSION 1.6.1 LANGUAGES CXX)

# Header-only library.
add_library(opuspp INTERFACE)
add_library(opuspp::opuspp ALIAS opuspp)
target_include_directories(opuspp INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(opuspp INTERFACE cxx_std_23)

install(DIRECTORY include/ DESTINATION include)
install(TARGETS opuspp EXPORT opusppTargets)
install(EXPORT opusppTargets NAMESPACE opuspp:: DESTINATION lib/cmake/opuspp)

# Differential tests against the reference libopus 1.6.1 sources (hosted).
# They live outside this project, by default next to it.
option(OPUSPP_BUILD_TESTS "Build the comparison tests against reference libopus" OFF)
set(OPUSPP_LIBOPUS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../opus-1.6.1" CACHE PATH
    "libopus 1.6.1 source tree (tests, benchmarks and tools only)")
if(OPUSPP_BUILD_TESTS)
  if(NOT EXISTS ${OPUSPP_LIBOPUS_DIR}/CMakeLists.txt)
    message(FATAL_ERROR "${OPUSPP_LIBOPUS_DIR} is missing: run tools/fetch_deps.sh first")
  endif()
  enable_language(C)
  enable_testing()
  # The x86 reference dispatches an AVX2+FMA pitch kernel at run time, which
  # changes float rounding in the PLC; keep the SSE baseline so the comparison
  # can be bit-exact.
  set(OPUS_X86_MAY_HAVE_AVX2 OFF CACHE BOOL "" FORCE)
  set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  add_subdirectory(${OPUSPP_LIBOPUS_DIR} opus-1.6.1 EXCLUDE_FROM_ALL)

  foreach(t compare fuzz)
    add_executable(opuspp_${t} tests/${t}.cpp)
    target_link_libraries(opuspp_${t} PRIVATE opuspp opus)
    add_test(NAME ${t} COMMAND opuspp_${t})
  endforeach()

  # Freestanding math vs host libm (float-rounded results must be identical).
  add_executable(opuspp_math_check tests/math_check.cpp)
  target_link_libraries(opuspp_math_check PRIVATE opuspp)
  add_test(NAME math_check COMMAND opuspp_math_check)

  # Further libopus configurations, built from the same sources as separate
  # external projects: opuspp_reference(<name> <libopus cmake options...>)
  # creates the imported static library <name>.
  include(ExternalProject)
  function(opuspp_reference name)
    set(dir ${CMAKE_BINARY_DIR}/${name})
    set(lib ${dir}/${CMAKE_STATIC_LIBRARY_PREFIX}opus${CMAKE_STATIC_LIBRARY_SUFFIX})
    ExternalProject_Add(${name}_build
      SOURCE_DIR ${OPUSPP_LIBOPUS_DIR}
      BINARY_DIR ${dir}
      CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
                 -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF ${ARGN}
      BUILD_COMMAND ${CMAKE_COMMAND} --build ${dir} --target opus --config Release
      INSTALL_COMMAND ""
      BUILD_BYPRODUCTS ${lib})
    add_library(${name} STATIC IMPORTED GLOBAL)
    set_target_properties(${name} PROPERTIES IMPORTED_LOCATION ${lib}
      INTERFACE_INCLUDE_DIRECTORIES ${OPUSPP_LIBOPUS_DIR}/include)
    if(UNIX)
      set_property(TARGET ${name} APPEND PROPERTY INTERFACE_LINK_LIBRARIES m)
    endif()
    add_dependencies(${name} ${name}_build)
  endfunction()

  if(MSVC)
    set(OPUSPP_FAST_MATH_FLAG /fp:fast)
  else()
    set(OPUSPP_FAST_MATH_FLAG -ffast-math)
  endif()

  # OPUSPP_FLOAT_APPROX must be bit-exact with libopus built with FLOAT_APPROX
  # (SSE baseline, no fast-math).
  opuspp_reference(opus_ref_approx -DOPUS_FLOAT_APPROX=ON -DOPUS_X86_MAY_HAVE_AVX2=OFF)
  add_executable(opuspp_compare_approx tests/compare.cpp)
  target_compile_definitions(opuspp_compare_approx PRIVATE OPUSPP_FLOAT_APPROX)
  target_link_libraries(opuspp_compare_approx PRIVATE opuspp opus_ref_approx)
  add_test(NAME compare_approx COMMAND opuspp_compare_approx)

  # Decoder throughput vs libopus (not tests; run manually on a Release build).
  # opuspp_bench: both decoders with fast math and FLOAT_APPROX, libopus
  # without its hardening/stack protector/fortify defaults.
  opuspp_reference(opus_ref_fast -DOPUS_FLOAT_APPROX=ON -DOPUS_FAST_MATH=ON
    -DOPUS_HARDENING=OFF -DOPUS_STACK_PROTECTOR=OFF -DOPUS_FORTIFY_SOURCE=OFF)
  add_executable(opuspp_bench tests/bench.cpp)
  target_compile_definitions(opuspp_bench PRIVATE OPUSPP_FLOAT_APPROX)
  target_compile_options(opuspp_bench PRIVATE ${OPUSPP_FAST_MATH_FLAG})
  target_link_libraries(opuspp_bench PRIVATE opuspp opus_ref_fast)
  # opuspp_bench_ieee: both decoders with strict IEEE float, libopus without
  # its hardening/stack protector/fortify defaults.
  opuspp_reference(opus_ref_perf -DOPUS_HARDENING=OFF -DOPUS_STACK_PROTECTOR=OFF -DOPUS_FORTIFY_SOURCE=OFF)
  add_executable(opuspp_bench_ieee tests/bench.cpp)
  target_link_libraries(opuspp_bench_ieee PRIVATE opuspp opus_ref_perf)

  # Profiling driver: `perf record opuspp_profile [ref|pp] [bitrate] [passes] [segments]`.
  add_executable(opuspp_profile tests/profile510.cpp)
  target_link_libraries(opuspp_profile PRIVATE opuspp opus_ref_perf)

  # Peak stack use of opuspp and libopus per decoding path: `opuspp_stack_usage`.
  find_package(Threads REQUIRED)
  add_executable(opuspp_stack_usage tests/stack_usage.cpp)
  target_link_libraries(opuspp_stack_usage PRIVATE opuspp opus_ref_perf Threads::Threads)

  # Listening tools: opuspp_encode encodes the benchmark music clip with
  # libopus into .opk packet files (plus the original as WAV); opuspp_decode
  # decodes one with opuspp (fast math) to a float WAV. See tools/opus_wav/.
  add_executable(opuspp_encode tools/opus_wav/encode.cpp)
  target_include_directories(opuspp_encode PRIVATE tests)
  target_link_libraries(opuspp_encode PRIVATE opus_ref_fast)
  add_executable(opuspp_decode tools/opus_wav/decode.cpp)
  target_compile_definitions(opuspp_decode PRIVATE OPUSPP_FLOAT_APPROX)
  target_compile_options(opuspp_decode PRIVATE ${OPUSPP_FAST_MATH_FLAG})
  target_link_libraries(opuspp_decode PRIVATE opuspp)

  # Real-music segment of the benchmark input (tests/data/README.md).
  foreach(t opuspp_bench opuspp_bench_ieee opuspp_profile opuspp_encode)
    target_compile_definitions(${t} PRIVATE
      OPUSPP_BENCH_AUDIO="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/figaro_overture.s16le")
  endforeach()
  # bench_stream.hpp (aligned packet storage) uses the .opk reader.
  foreach(t opuspp_bench opuspp_bench_ieee opuspp_profile)
    target_include_directories(${t} PRIVATE tools/opus_wav)
  endforeach()

  # Checks that the library builds freestanding without exceptions or RTTI
  # and that the object references no external symbols.
  add_library(opuspp_freestanding OBJECT tests/freestanding.cpp)
  target_link_libraries(opuspp_freestanding PRIVATE opuspp)
  target_compile_options(opuspp_freestanding PRIVATE
    -ffreestanding -fno-exceptions -fno-rtti -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -Werror)
endif()
