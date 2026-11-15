file(REMOVE_RECURSE
  "libcapstone.a"
  "libcapstone.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang C)
  include(CMakeFiles/capstone_static.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
