#!/usr/bin/env bash
#
# The browser demo's port of the CPU engine (demo/model.js, dsp.js, engine.js)
# against the plugin's own C++.
#
#   demo/tools/check_port.sh [--quick]
#
# check_port.mjs compiles refport.cpp with the ParamID enum, the constructor
# and the whole of ProcessOpenGL cut out of source/Lowband.{h,cpp} at run time,
# against the plugin's own Engine.cpp, Model.cpp, Dsp.cpp, Controls.cpp and
# Clock.cpp (GL recorded, not drawn; the intake handed to its read-back), then
# compares the declarations, the designs, the Gaussian table, the clock, the
# settings, the passes and every float of the deck's uploaded picture with the
# port's, exactly. It says what it covers and what it cannot. Called from
# tools/verify.sh; exits 3 (skip) without node or a C++ compiler.
#
set -uo pipefail
cd "$(dirname "$0")/../.."

command -v node >/dev/null 2>&1 || { echo "skipped: node not installed"; exit 3; }
command -v c++ >/dev/null 2>&1 || { echo "skipped: no C++ compiler"; exit 3; }
node demo/tools/check_port.mjs "$@"
