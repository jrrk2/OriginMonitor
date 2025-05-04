#!/bin/bash
# ci_scripts/ci_post_clone.sh

echo "=== Post-Clone: Setting up Qt macOS environment for Xcode Cloud ==="

# Show all environment variables (helpful for debugging)
echo "Environment variables:"
env | sort > env_vars.log

# Check the Xcode configuration
echo "=== Xcode Configuration ==="
xcode-select -print-path
xcrun -find moc || echo "MOC not found in PATH"

# Check if Qt is installed or available
echo "=== Qt Installation Check ==="
find /Applications -name "Qt*.app" -type d | tee qt_paths.log
find /Users -name "Qt*.app" -type d | tee -a qt_paths.log

# Find Qt frameworks
echo "=== Qt Frameworks Check ==="
find /Library/Frameworks -name "Qt*" | tee qt_frameworks.log

# Save build environment info for review
mkdir -p debug_logs
echo "=== MOC Executable Search ==="
find / -name "moc" -type f 2>/dev/null | tee debug_logs/moc_paths.log

exit 0
