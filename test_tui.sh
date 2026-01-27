#!/bin/bash
# Test script to verify TUI functionality

# Change to script directory
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

# Create a simple test that runs the program briefly and captures output
echo "Testing TUI startup and rendering..."

# Run the program with a timeout and send quit command
(sleep 0.5; echo 'q'; sleep 0.2; echo 'y') | timeout 2 ./smartproxy 2>&1 | head -50 || true

echo ""
echo "TUI test completed."
