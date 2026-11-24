# libFuzzer harnesses (SPEC.md §19; CI job "Fuzz", 60 s each). Configure with
# clang and -DZPROJECT_FUZZ=ON (cmake/Sanitizers.cmake instruments the
# libraries); run e.g.
#   build-fuzz/fuzz/fuzz_link_notation -max_total_time=60 <scratch copy of fuzz/corpus/link_notation>
foreach(_h link_notation dependency_json calendar_json export)
  add_executable(fuzz_${_h} fuzz_${_h}.cpp)
  target_link_libraries(fuzz_${_h} PRIVATE zproject_core)
  target_link_options(fuzz_${_h} PRIVATE -fsanitize=fuzzer)
endforeach()
