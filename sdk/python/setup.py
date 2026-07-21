"""setup.py — builds the native _rcp extension from the C++ SDK via pybind11."""
import os
from setuptools import setup
from pybind11.setup_helpers import Pybind11Extension, build_ext

# The C++ SDK headers live in ../cpp/include (single source of truth).
HERE = os.path.dirname(os.path.abspath(__file__))
CPP_INCLUDE = os.path.normpath(os.path.join(HERE, "..", "cpp", "include"))

ext_modules = [
    Pybind11Extension(
        "rcp._rcp",
        ["src/bindings.cpp"],
        include_dirs=[CPP_INCLUDE],
        cxx_std=23,
    ),
]

setup(
    packages=["rcp"],
    ext_modules=ext_modules,
    cmdclass={"build_ext": build_ext},
)
