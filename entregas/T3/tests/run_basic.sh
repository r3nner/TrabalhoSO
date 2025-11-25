timeout 3s ./main > tests/output.txt 2>&1 || true
#!/bin/bash
set -e
# Test script (basic): build the project and verify the simulator executable exists
ROOT="/home/pao/Área de Trabalho/SO2/TrabalhoSOok"
cd "$ROOT"
mkdir -p tests
echo "Building project..."
if ! make -j2; then
  echo "BUILD FAIL"
  exit 2
fi
echo "Build succeeded. Checking for executable './main'..."
if [ -x ./main ]; then
  echo "TEST PASS: './main' exists and is executable"
  exit 0
else
  echo "TEST FAIL: './main' missing or not executable"
  ls -l ./main || true
  exit 3
fi
