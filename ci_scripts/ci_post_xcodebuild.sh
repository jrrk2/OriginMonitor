#!/bin/bash
# ci_scripts/ci_post_xcodebuild.sh

echo "=== Post-Build: Analyzing Qt build results ==="

# Create directory for build logs if it doesn't exist
mkdir -p build_logs

# Collect build logs
if [ -d "${CI_DERIVED_DATA_PATH}" ]; then
    echo "Collecting build logs from DerivedData"
    find "${CI_DERIVED_DATA_PATH}" -name "*.log" -exec cp {} build_logs/ \;
fi

# Analyze build errors
echo "=== Build Error Analysis ==="
grep -r "error:" build_logs/ || echo "No explicit errors found in logs"
grep -r "PhaseScriptExecution failed" build_logs/ || echo "No PhaseScriptExecution failures found"
grep -r "moc" build_logs/ || echo "No MOC references found in logs"

echo "Post-build analysis complete"
exit 0
