# A source export has no Git working directory. Do not inspect a parent repository.
set(__get_git_revision_description YES)
function(get_git_head_revision refspec hash)
  set(${refspec} "exported-source" PARENT_SCOPE)
  set(${hash} "1fce01f4f8f06e45f1b82f339eb9ab29ee1258dd" PARENT_SCOPE)
endfunction()
