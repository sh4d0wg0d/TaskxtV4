#!/bin/bash
# TaskXT Sysfs Control Test Script
# Tests the runtime parameter change functionality

set -e

TASKXT_SYSFS="/sys/kernel/taskxt"

echo "=== TaskXT Sysfs Control Test ==="
echo ""

# Check if module is loaded
if ! lsmod | grep -q taskxt; then
    echo "ERROR: taskxt module not loaded"
    echo "Load it first: sudo insmod taskxt-6.18.3-arch1-1.ko path=/tmp pname=test srate=1 dura=5000"
    exit 1
fi

echo "✓ Module is loaded"
echo ""

# Check if sysfs attributes exist
if [ ! -d "$TASKXT_SYSFS" ]; then
    echo "ERROR: sysfs directory not found: $TASKXT_SYSFS"
    exit 1
fi

echo "✓ sysfs directory exists"
echo ""

# List all attributes
echo "Available attributes:"
ls -1 "$TASKXT_SYSFS/" | grep -v "^\."
echo ""

# Test reading status
echo "Current Status:"
cat "$TASKXT_SYSFS/status"
echo ""

# Test reading current parameters
echo "Current Parameters:"
echo "  pname: $(cat $TASKXT_SYSFS/pname_control)"
echo "  srate: $(cat $TASKXT_SYSFS/srate_control) ms"
echo "  dura:  $(cat $TASKXT_SYSFS/dura_control) ms"
echo "  sampling: $(cat $TASKXT_SYSFS/sampling)"
echo ""

# Test changing parameters
echo "Testing parameter changes..."
echo ""

echo "1. Changing process name to 'bash'..."
echo "bash" | sudo tee "$TASKXT_SYSFS/pname_control" > /dev/null
echo "   New pname: $(cat $TASKXT_SYSFS/pname_control)"
sleep 1

echo ""
echo "2. Changing sampling rate to 5ms..."
echo "5" | sudo tee "$TASKXT_SYSFS/srate_control" > /dev/null
echo "   New srate: $(cat $TASKXT_SYSFS/srate_control)"
sleep 1

echo ""
echo "3. Changing duration to 10000ms..."
echo "10000" | sudo tee "$TASKXT_SYSFS/dura_control" > /dev/null
echo "   New dura: $(cat $TASKXT_SYSFS/dura_control)"
sleep 1

echo ""
echo "4. Starting sampling..."
echo "1" | sudo tee "$TASKXT_SYSFS/sampling" > /dev/null
echo "   sampling: $(cat $TASKXT_SYSFS/sampling)"
sleep 2

echo ""
echo "5. Stopping sampling..."
echo "0" | sudo tee "$TASKXT_SYSFS/sampling" > /dev/null
echo "   sampling: $(cat $TASKXT_SYSFS/sampling)"
echo ""

echo "Final Status:"
cat "$TASKXT_SYSFS/status"
echo ""

echo "=== Test Complete ==="
echo ""
echo "Check kernel messages: dmesg | grep TaskXT"
