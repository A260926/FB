The baseline command is `bash task_support/run.sh baseline` from the source root.
It compiles the retained C99 production source with the system C compiler and libm,
then checks ordinary depth, framebuffer isolation, render-list replay, and textured
Phong lighting. It does not implement or validate the requested voxel feature.
No assets, network connection, window toolkit, or GPU are used.
