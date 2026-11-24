file(REMOVE_RECURSE
  "libkvazaar.a"
  "libkvazaar.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang C CXX)
  include(CMakeFiles/kvazaar.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
