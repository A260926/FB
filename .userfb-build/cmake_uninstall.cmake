# -----------------------------------------------
# File that provides "make uninstall" target
#  We use the file 'install_manifest.txt'
# -----------------------------------------------
if(NOT EXISTS "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/.userfb-build/install_manifest.txt")
  message(FATAL_ERROR "Cannot find install manifest: \"/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/.userfb-build/install_manifest.txt\"")
endif(NOT EXISTS "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/.userfb-build/install_manifest.txt")

file(READ "/Users/solo/pro/UserFB/workspace/Z1230/attempts/20261111T163631180204Z/repo/.userfb-build/install_manifest.txt" files)
string(REGEX REPLACE "\n" ";" files "${files}")
foreach(file ${files})
  message(STATUS "Uninstalling \"$ENV{DESTDIR}${file}\"")
	execute_process(COMMAND rm $ENV{DESTDIR}${file})
endforeach(file)


