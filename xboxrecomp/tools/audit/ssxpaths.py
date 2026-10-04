"""Where things live, for the build passes in this folder.

    import ssxpaths
    ssxpaths.GEN, ssxpaths.XBE, ...
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))

PORT = os.path.join(ROOT, "port")
SRC = os.path.join(PORT, "src")
RECOMP = os.path.join(SRC, "recomp")
GEN = os.path.join(RECOMP, "gen")
BUILD = os.path.join(PORT, "build")
GAME_EXE = os.path.join(BUILD, os.environ.get("SSX_GAME_EXE", "SSX Tricky.exe"))

GAME_FILES = os.path.join(ROOT, "game_files")
XBE = os.path.join(GAME_FILES, "default.xbe")
