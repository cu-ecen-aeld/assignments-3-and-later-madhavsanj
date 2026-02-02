#!/bin/sh

set -u

if [ $# -lt 2 ]; then
    echo "Error: missing arguments."
    exit 1
fi

writefile="$1"
writestr="$2"


writedir=$(dirname "$writefile")

mkdir -p "$writedir"
if [ $? -ne 0 ]; then
    echo "Error: could not create directory path '$writedir'"
    exit 1
fi

echo "$writestr" > "$writefile"
if [ $? -ne 0 ]; then
    echo "Error: could not write to file '$writefile'"
    exit 1
fi

exit 0