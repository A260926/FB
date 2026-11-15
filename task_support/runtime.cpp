#include "pbrt.h"
// Standalone CPU library executable does not link the scene-file parser/API main.
Options PbrtOptions;
int line_num = 0;
std::string current_file;
