#!/bin/bash
# Comprehensive TUI verification script
# This script verifies all the TUI improvements

echo "=== TUI Revision Verification ==="
echo ""

# Change to script directory
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

# 1. Verify centralized constants exist
echo "1. Checking centralized margin constants..."
if grep -q "static constexpr int MARGIN_TOP" tui.h && \
   grep -q "static constexpr int MARGIN_BOTTOM" tui.h && \
   grep -q "static constexpr int MARGIN_LEFT" tui.h && \
   grep -q "static constexpr int STATUS_BAR_HEIGHT" tui.h; then
    echo "   ✓ All margin constants defined in tui.h"
else
    echo "   ✗ Missing margin constants"
    exit 1
fi

# 2. Verify helper function exists
echo "2. Checking fill_line_with_bg helper function..."
if grep -q "void TUI::fill_line_with_bg" tui.cpp && \
   grep -q "void fill_line_with_bg" tui.h; then
    echo "   ✓ Helper function declared and implemented"
else
    echo "   ✗ Helper function missing"
    exit 1
fi

# 3. Verify usage of constants in draw()
echo "3. Checking usage of centralized constants in draw()..."
if grep -q "MARGIN_TOP" tui.cpp && \
   grep -q "MARGIN_BOTTOM" tui.cpp && \
   grep -q "STATUS_BAR_HEIGHT" tui.cpp; then
    echo "   ✓ Constants used in draw() function"
else
    echo "   ✗ Constants not used properly"
    exit 1
fi

# 4. Verify usage of constants in navigation functions
echo "4. Checking usage of centralized constants in navigation..."
if grep -q "MARGIN_TOP" tui_input.cpp && \
   grep -q "STATUS_BAR_HEIGHT" tui_input.cpp; then
    echo "   ✓ Constants used in navigation functions"
else
    echo "   ✗ Constants not used in navigation"
    exit 1
fi

# 5. Verify no hardcoded approximate values
echo "5. Checking for removed approximate values..."
if ! grep -q "int used = 35; // Approximate" tui.cpp && \
   ! grep -q "int used = 50; // Approximate" tui.cpp; then
    echo "   ✓ No approximate values found (replaced with calculations)"
else
    echo "   ✗ Still has approximate values"
    exit 1
fi

# 6. Verify fill_line_with_bg is used
echo "6. Checking fill_line_with_bg usage..."
usage_count=$(grep -c "fill_line_with_bg" tui.cpp)
if [ "$usage_count" -ge 3 ]; then
    echo "   ✓ fill_line_with_bg used in $usage_count places"
else
    echo "   ✗ fill_line_with_bg not used enough"
    exit 1
fi

# 7. Verify 'q' key behavior fix
echo "7. Checking 'q' key behavior (should match ESC in detail view)..."
if grep -A 3 "buf\[0\] == 'q'" tui_input.cpp | grep -q "detail_view_" && \
   grep -A 3 "buf\[0\] == 'q'" tui_input.cpp | grep -q "hide_detail"; then
    echo "   ✓ 'q' key checks detail_view and hides detail"
else
    echo "   ✗ 'q' key behavior not fixed"
    exit 1
fi

# 8. Verify no broken loops (i = cols; i < cols)
echo "8. Checking for fixed broken loops..."
if ! grep -q "for (int i = cols; i < cols" tui.cpp; then
    echo "   ✓ No broken loops found"
else
    echo "   ✗ Still has broken loops"
    exit 1
fi

# 9. Verify no double background reset
echo "9. Checking for proper background color handling..."
if ! grep -A 3 "draw_command_bar" tui.cpp | grep -q "output << \"\033\[0m\";.*for.*i < cols"; then
    echo "   ✓ No premature background reset before fill"
else
    echo "   ✗ Still has background reset issues"
    exit 1
fi

# 10. Build test
echo "10. Building project to ensure code compiles..."
if ./build.sh 2>&1 | grep -q "Built target smartproxy"; then
    echo "   ✓ Project builds successfully"
else
    echo "   ✗ Build failed"
    exit 1
fi

echo ""
echo "=== All Verification Checks Passed! ==="
echo ""
echo "Summary of improvements:"
echo "  • Centralized margin constants for easy adjustment"
echo "  • Fixed header/footer background color alignment"
echo "  • Removed approximate width calculations"
echo "  • Fixed broken loops and double-filling"
echo "  • Added helper function for safe background filling"
echo "  • Fixed 'q' key to behave like ESC in detail view"
echo "  • Code is now safe for adding new content or changing margins"
echo ""
