from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

import os
import sys
import subprocess
import driver_config

VERBOSE = 1 #bool(os.environ.get("VERBOSE"))

CXX = test_config.CXX_PATH
ONNF = os.path.join(test_config.ONNF_BUILD_PATH, "bin/onnf")
LLC = os.path.join(test_config.LLVM_PROJ_BUILD_PATH, "bin/llc")

# Make lib folder under build directory visible in PYTHONPATH
RUNTIME_DIR = os.path.join(test_config.ONNF_BUILD_PATH, "lib")
sys.path.append(RUNTIME_DIR)

def execute_command(cmd):
    if (VERBOSE):
        print(" ".join(cmd))
    subprocess.run(cmd, stdout=subprocess.PIPE)

def compile(inputModelPath):
    execute_command([ONNF, inputModelPath])
    # Call llc to generate object file from bitcode.
    FilePath = os.path.splitext(inputModelPath)[0]
    execute_command(
        [LLC, "-filetype=obj", "-relocation-model=pic", FilePath + ".bc"])
    # Generate shared library from object file, linking with c runtime.
    execute_command([
        CXX, "-shared", "-fPIC", FilePath + ".o", "-o", FilePath + ".so",
        "-L" + RUNTIME_DIR, "-lcruntime"
    ])

