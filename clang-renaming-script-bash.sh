#!/bin/bash

# --- Configuration ---

# Path to your compilation database. Assumes it's in a 'build' directory
COMPILE_DB_PATH="build"

# --- Script Logic ---

echo "Starting Clang-Tidy style application and modernization..."
echo "Using compile database at: ${COMPILE_DB_PATH}"

# Check if the compile_commands.json directory exists
if [ ! -d "${COMPILE_DB_PATH}" ]; then
    echo "Error: Compilation database directory not found at ${COMPILE_DB_PATH}/compile_commands.json"
    echo "Please ensure your project is built and compile_commands.json is generated."
    exit 1
fi

# Run clang-tidy with the --fix option to apply changes
# The checks are implicitly read from your .clang-tidy file.
# The -p flag points to the directory containing compile_commands.json.
# The HeaderFilterRegex in your .clang-tidy will ensure only your files are modified.
# Assuming 'run-clang-tidy.py' is in your system's PATH.
run-clang-tidy -p "${COMPILE_DB_PATH}" --fix

# --- Post-execution instructions ---

echo "Clang-Tidy application complete."
echo ""
echo "#################################################################"
echo "# IMPORTANT: Review the changes carefully!                      #"
echo "# Use 'git diff' to inspect all modifications made by clang-tidy."
echo "# If you are satisfied, commit the changes: 'git add . && git commit -m \"Apply clang-tidy style\"'"
echo "# If not, you can revert them: 'git restore .'"
echo "#################################################################"
