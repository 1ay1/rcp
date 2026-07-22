"""setup.py — packaging for the native (pure-Python) RCP SDK.

There is no C/C++ extension: the SDK is standard-library-only Python, so this is
a plain source distribution. Metadata lives in pyproject.toml; this shim exists
only for ``pip install -e .`` on older toolchains.
"""
from setuptools import setup

setup(packages=["rcp"])
