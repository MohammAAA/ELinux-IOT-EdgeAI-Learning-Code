#!/bin/bash
# 03_kmodule_workflow_automation.sh — Complete kmodule build, load, verify, unload cycle

set -e  # Exit on any error

MODULE="01_Minimal_kernel_module"

echo "=== Step 1: Clean build ==="
make clean
make
echo "=== Finished ==="

echo ""
echo "=== Step 2: Verify module metadata ==="
modinfo ${MODULE}.ko

echo ""
echo "=== Step 3: Clear kernel log (dmesg -C) ==="
sudo dmesg -C

echo ""
echo "=== Step 4: Load module (insmod) ==="
sudo insmod ./${MODULE}.ko

echo ""
echo "=== Step 5: Verify loaded (lsmod) ==="
lsmod | grep ${MODULE}

echo ""
echo "=== Step 6: Check kernel log (dmesg) ==="
sudo dmesg

echo ""
echo "=== Step 7: Verify no kernel taint ==="
TAINT=$(cat /proc/sys/kernel/tainted)
if [ "$TAINT" -eq 0 ]; then
    echo "Kernel is clean (tainted=0)"
else
    echo "WARNING: Kernel tainted=$TAINT"
fi

echo ""
echo "=== Step 8: Unload module (rmmod) ==="
sudo rmmod ${MODULE}

echo ""
echo "=== Step 9: Verify unloaded (lsmod) ==="
if lsmod | grep -q ${MODULE}; then
    echo "ERROR: Module still loaded!"
    exit 1
else
    echo "Module successfully unloaded"
fi

echo ""
echo "=== Step 10: Check final kernel log (dmesg) ==="
sudo dmesg

echo ""
echo "=== ALL CHECKS PASSED ==="