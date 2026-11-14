add_executable(quickstart quickstart.cpp)
target_link_libraries(quickstart PRIVATE tinysplinecxx)
set_target_properties(quickstart PROPERTIES FOLDER "examples/cpp")

add_executable(time_series time_series.cpp)
target_link_libraries(time_series PRIVATE tinysplinecxx)
set_target_properties(time_series PROPERTIES FOLDER "examples/cpp")

add_executable(periodic_fit_cxx periodic_fit.cpp)
target_link_libraries(periodic_fit_cxx PRIVATE tinysplinecxx)
set_target_properties(periodic_fit_cxx PROPERTIES FOLDER "examples/cpp")

add_subdirectory(fltk)
