#!/bin/bash

# Test script for FileEngine permissions system

echo "Starting FileEngine permissions system test..."

# Create a test file to upload
echo "Creating test file..."
echo "This is a test file for permissions testing" > /tmp/test_file.txt

# First, let's create a directory as root user (root should have full access)
echo "Step 1: Creating a directory as root user"
cd /home/telendry/code/file_projects/file_engine_core/build

# Create a directory as root user
TEST_DIR_UID=$(./cli/fileengine_cli -u root touch "" test_permissions_dir | grep -oE '[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}' | head -1)

if [ -z "$TEST_DIR_UID" ]; then
    # If touch doesn't work for directory, try mkdir
    TEST_DIR_UID=$(./cli/fileengine_cli -u root mkdir "" test_permissions_dir | grep -oE '[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}' | head -1)
    if [ -z "$TEST_DIR_UID" ]; then
        echo "Failed to create test directory as root user"
        exit 1
    fi
fi

echo "Created test directory with UID: $TEST_DIR_UID"

# Create a test file in that directory
TEST_FILE_UID=$(./cli/fileengine_cli -u root touch $TEST_DIR_UID test_file.txt | grep -oE '[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}' | head -1)

if [ -z "$TEST_FILE_UID" ]; then
    echo "Failed to create test file as root user"
    exit 1
fi

echo "Created test file with UID: $TEST_FILE_UID"

# Upload content to the test file as root
echo "Uploading content to test file as root user..."
./cli/fileengine_cli -u root upload $TEST_DIR_UID test_file.txt /tmp/test_file.txt

# Now let's grant specific permissions to another user
echo "Step 2: Granting permissions to test_user"

# Grant read permission to test_user on the test file
./cli/fileengine_cli -u root grant $TEST_FILE_UID test_user r

# Grant write permission to test_user on the test file
./cli/fileengine_cli -u root grant $TEST_FILE_UID test_user w

# Grant read permission to test_user on the directory
./cli/fileengine_cli -u root grant $TEST_DIR_UID test_user r

echo "Granted read and write permissions to test_user on test file and directory"

# Now let's test access with test_user
echo "Step 3: Testing access with test_user"

# Check if test_user can list the directory
echo "Checking if test_user can list directory..."
./cli/fileengine_cli -u test_user ls $TEST_DIR_UID

# Check if test_user can read the file
echo "Checking if test_user can read file..."
./cli/fileengine_cli -u test_user get $TEST_FILE_UID /tmp/downloaded_test_file.txt

# Check if test_user can check their permissions
echo "Checking if test_user has read permission..."
./cli/fileengine_cli -u test_user check $TEST_FILE_UID test_user r

echo "Checking if test_user has write permission..."
./cli/fileengine_cli -u test_user check $TEST_FILE_UID test_user w

# Now let's test with a user that has no permissions
echo "Step 4: Testing access with unauthorized_user"

# Check if unauthorized_user can list the directory (should fail)
echo "Checking if unauthorized_user can list directory (should fail)..."
./cli/fileengine_cli -u unauthorized_user ls $TEST_DIR_UID

# Check if unauthorized_user can read the file (should fail)
echo "Checking if unauthorized_user can read file (should fail)..."
./cli/fileengine_cli -u unauthorized_user get $TEST_FILE_UID /tmp/unauthorized_download.txt

# Check if unauthorized_user has read permission (should fail)
echo "Checking if unauthorized_user has read permission (should fail)..."
./cli/fileengine_cli -u unauthorized_user check $TEST_FILE_UID unauthorized_user r

# Now let's test with roles
echo "Step 5: Testing with roles"

# Grant permissions to a role
./cli/fileengine_cli -u root grant $TEST_FILE_UID "role:editor" r
./cli/fileengine_cli -u root grant $TEST_FILE_UID "role:editor" w

# Test access with a user that has the editor role
echo "Checking access for user with editor role..."
./cli/fileengine_cli -u editor_user -r editor ls $TEST_DIR_UID
./cli/fileengine_cli -u editor_user -r editor get $TEST_FILE_UID /tmp/editor_download.txt

echo "Step 6: Testing permission revocation"

# Revoke write permission from test_user
./cli/fileengine_cli -u root revoke $TEST_FILE_UID test_user w

# Check if test_user still has write permission (should fail now)
echo "Checking if test_user still has write permission after revocation (should fail)..."
./cli/fileengine_cli -u test_user check $TEST_FILE_UID test_user w

# Check if test_user still has read permission (should still work)
echo "Checking if test_user still has read permission after write revocation (should work)..."
./cli/fileengine_cli -u test_user check $TEST_FILE_UID test_user r

echo "Permissions test completed!"

# Clean up test files
echo "Cleaning up test files..."
./cli/fileengine_cli -u root rm $TEST_FILE_UID
./cli/fileengine_cli -u root rm $TEST_DIR_UID