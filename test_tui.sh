#!/bin/bash
# Test script to verify TUI functionality

# Change to script directory
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

# Configurable timeouts for different system speeds
INITIAL_WAIT=${INITIAL_WAIT:-1}  # Wait for startup
INPUT_DELAY=${INPUT_DELAY:-0.5}  # Delay before sending 'y'
TOTAL_TIMEOUT=${TOTAL_TIMEOUT:-5}  # Overall timeout

# Create a simple test that runs the program briefly and captures output
echo "Testing TUI startup and rendering..."

# Run the program with a timeout and send quit command
(sleep "$INITIAL_WAIT"; echo 'q'; sleep "$INPUT_DELAY"; echo 'y') | timeout "$TOTAL_TIMEOUT" ./smartproxy 2>&1 | head -50 || true

echo ""
echo "TUI test completed."
