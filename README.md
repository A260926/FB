This source subset contains MLT's unmodified C framework and its CPU luma/composite
implementation. Optional modules, bindings and device/media dependencies are not
part of this build. See COPYING and the copyright/license headers in each source.

Build and run the current CPU baseline with:

    bash task_support/run.sh baseline

After implementing strict video export and adding src/tests/task_strict_video.c:

    bash task_support/run.sh target

The target runs fixed calls into the production frame/tractor/luma/consumer path,
then runs the image, graph and export task_cases using TASK_CHECK. It also runs
the normal baseline; only baseline mode checks the original fallback defect.
The fixed probes and the checks runner are support code, not a replacement for
the new regression cases. No Python, package download or external media is used.
