##########################################
# ec_voxellib
##########################################

add_library(
  ec_voxelib
  ec_voxelib.cc
)

target_include_directories(ec_voxelib PRIVATE ../almostequals)

target_compile_options(ec_voxelib PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)
