#!/bin/bash
# TaskXT Safe Load/Unload Helper Script

set -e

TASKXT_DIR="/home/sh4d0w/Workspace/tutorials/machine_learning/taskxtV4"
TASKXT_MODULE="taskxt-6.18.3-arch1-1.ko"
TASKXT_ALT="taskxt-6.18.5-arch1-1.ko"

# Detect which module exists
if [ -f "$TASKXT_DIR/$TASKXT_MODULE" ]; then
    MODULE_FILE="$TASKXT_DIR/$TASKXT_MODULE"
    MODULE_NAME="${TASKXT_MODULE%.ko}"
elif [ -f "$TASKXT_DIR/$TASKXT_ALT" ]; then
    MODULE_FILE="$TASKXT_DIR/$TASKXT_ALT"
    MODULE_NAME="${TASKXT_ALT%.ko}"
else
    echo "ERROR: No module found!"
    echo "Expected: $TASKXT_DIR/$TASKXT_MODULE or $TASKXT_DIR/$TASKXT_ALT"
    exit 1
fi

echo "Module: $MODULE_NAME"
echo "File: $MODULE_FILE"
echo ""

case "$1" in
    load)
        if lsmod | grep -q taskxt; then
            echo "ERROR: taskxt module already loaded"
            echo "Unload it first: $0 unload"
            exit 1
        fi
        
        shift  # Remove 'load' from args
        if [ $# -lt 2 ]; then
            echo "Usage: $0 load path=<path> pname=<processname> [srate=<ms>] [dura=<ms>]"
            echo ""
            echo "Example:"
            echo "  $0 load path=/tmp pname=ls srate=1 dura=5000"
            exit 1
        fi
        
        echo "Loading module with parameters: $@"
        sudo insmod "$MODULE_FILE" "$@"
        echo "✓ Module loaded successfully"
        
        # Verify
        sleep 1
        if lsmod | grep -q taskxt; then
            echo "✓ Verified: Module is loaded"
            echo ""
            echo "sysfs controls available at /sys/kernel/taskxt/"
        else
            echo "ERROR: Module failed to load!"
            exit 1
        fi
        ;;
        
    unload)
        if ! lsmod | grep -q taskxt; then
            echo "INFO: taskxt module not currently loaded"
            exit 0
        fi
        
        echo "Unloading taskxt module..."
        
        # Stop sampling first
        if [ -f /sys/kernel/taskxt/sampling ]; then
            echo "0" | sudo tee /sys/kernel/taskxt/sampling > /dev/null 2>&1 || true
        fi
        
        # Give it a moment
        sleep 1
        
        # Try normal rmmod first
        if sudo rmmod taskxt 2>/dev/null; then
            echo "✓ Module unloaded successfully"
        else
            # Try forced remove if needed
            echo "Normal unload failed, trying forced removal..."
            if sudo rmmod -f taskxt 2>/dev/null; then
                echo "✓ Module force-unloaded (with force flag)"
            else
                echo "ERROR: Could not unload module"
                echo "Current load count: $(lsmod | grep taskxt | awk '{print $3}')"
                exit 1
            fi
        fi
        
        # Verify
        sleep 1
        if lsmod | grep -q taskxt; then
            echo "ERROR: Module is still loaded!"
            exit 1
        else
            echo "✓ Verified: Module unloaded"
        fi
        ;;
        
    status)
        if ! lsmod | grep -q taskxt; then
            echo "taskxt: not loaded"
            exit 0
        fi
        
        echo "=== TaskXT Status ==="
        echo ""
        echo "Module Info:"
        lsmod | grep taskxt
        echo ""
        
        if [ -d /sys/kernel/taskxt ]; then
            echo "sysfs Attributes:"
            for attr in sampling status pname_control srate_control dura_control; do
                if [ -f /sys/kernel/taskxt/$attr ]; then
                    value=$(cat /sys/kernel/taskxt/$attr 2>/dev/null || echo "N/A")
                    printf "  %-20s: %s\n" "$attr" "$value"
                fi
            done
        else
            echo "sysfs attributes not available"
        fi
        ;;
        
    logs)
        echo "=== Recent Kernel Messages ==="
        sudo dmesg | grep TaskXT | tail -20
        ;;
        
    *)
        echo "TaskXT Safe Module Manager"
        echo ""
        echo "Usage: $0 <command> [options]"
        echo ""
        echo "Commands:"
        echo "  load <params>      Load module with parameters"
        echo "                     Example: $0 load path=/tmp pname=ls srate=1 dura=5000"
        echo ""
        echo "  unload             Unload module safely"
        echo ""
        echo "  status             Show module and sysfs status"
        echo ""
        echo "  logs               Show recent kernel messages"
        echo ""
        echo "Examples:"
        echo "  $0 load path=/tmp pname=ls srate=1 dura=5000"
        echo "  $0 status"
        echo "  $0 unload"
        ;;
esac
