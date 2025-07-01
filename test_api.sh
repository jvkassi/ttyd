#!/bin/bash

# Test the ttyd HTTP API

echo "Testing ttyd HTTP API..."
echo

# Test simple command
echo "Test 1: Simple command (ls)"
curl -s "http://localhost:12000/api/ls"
echo -e "\n"

# Test command with arguments
echo "Test 2: Command with arguments (ls -la)"
curl -s "http://localhost:12000/api/ls%20-la"
echo -e "\n"

# Test command with special characters
echo "Test 3: Command with special characters (echo 'Hello World!')"
curl -s "http://localhost:12000/api/echo%20%27Hello%20World%21%27"
echo -e "\n"

# Test command with pipes (using bash -c)
echo "Test 4: Command with pipes (ls | grep CMake)"
curl -s "http://localhost:12000/api/bash%20-c%20%22ls%20%7C%20grep%20CMake%22"
echo -e "\n"

# Test command that produces error
echo "Test 5: Command that produces error (ls /nonexistent)"
curl -s "http://localhost:12000/api/ls%20/nonexistent"
echo -e "\n"

echo "All tests completed."