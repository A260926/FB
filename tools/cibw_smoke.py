"""Smoke test used by cibuildwheel after each wheel is built."""
import pygel3d
from pygel3d import hmesh

hmesh.Manifold()
print("pygel3d", pygel3d.__version__)
