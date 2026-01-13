#!/bin/bash

# Test script for ACL functionality with roles
echo "Starting ACL tests with roles..."

# Start the server in the background
echo "Starting FileEngine server..."
cd /home/telendry/code/file_projects/file_engine_core/build
./core/fileengine_server &
SERVER_PID=$!

# Wait a moment for the server to start
sleep 3

# Test 1: Create a resource and grant permissions to a role
echo "Test 1: Creating a directory and granting permissions to a role..."
RESULT=$(timeout 10s ./cli/fileengine_cli -u test_user -r users touch test_dir test_file 2>&1)
if [[ $? -eq 0 ]]; then
    echo "✓ Created test file successfully"
else
    echo "✗ Failed to create test file: $RESULT"
fi

# Test 2: Try to grant permission to a role
echo "Test 2: Granting READ permission to 'users' role..."
# We need to create a directory first to have a resource UID
DIR_UID=$(timeout 10s ./cli/fileengine_cli -u test_user mkdir root test_dir 2>&1 | grep -o '[a-f0-9-]*-[a-f0-9-]*')
if [[ -n "$DIR_UID" ]]; then
    echo "Using directory UID: $DIR_UID"
    RESULT=$(timeout 10s ./cli/fileengine_cli -u admin_user -r administrators grant $DIR_UID users r 2>&1)
    if [[ $? -eq 0 ]]; then
        echo "✓ Granted READ permission to 'users' role on directory"
    else
        echo "✗ Failed to grant permission: $RESULT"
    fi
else
    echo "✗ Failed to create directory for testing"
fi

# Test 3: Check permission for a user with the role
echo "Test 3: Checking permissions for user with 'users' role..."
if [[ -n "$DIR_UID" ]]; then
    RESULT=$(timeout 10s ./cli/fileengine_cli -u test_user -r users check $DIR_UID test_user r 2>&1)
    if [[ $? -eq 0 ]]; then
        echo "✓ Permission check successful for user with role"
    else
        echo "✗ Permission check failed: $RESULT"
    fi
else
    echo "✗ Skipping permission check - no directory UID available"
fi

# Test 4: Create a role
echo "Test 4: Creating a role..."
RESULT=$(timeout 10s ./cli/fileengine_cli -u admin_user -r administrators create_role editors 2>&1)
if [[ $? -eq 0 ]]; then
    echo "✓ Created 'editors' role"
else
    echo "✗ Failed to create role: $RESULT"
fi

# Test 5: List all roles
echo "Test 5: Listing all roles..."
RESULT=$(timeout 10s ./cli/fileengine_cli -u admin_user -r administrators list_all_roles 2>&1)
if [[ $? -eq 0 ]]; then
    echo "✓ Listed all roles"
    echo "$RESULT"
else
    echo "✗ Failed to list roles: $RESULT"
fi

# Kill the server
kill $SERVER_PID

echo "ACL tests completed."