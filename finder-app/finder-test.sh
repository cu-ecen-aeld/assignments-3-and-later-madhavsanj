#!/bin/sh
# Tester script for assignment 1 and assignment 2
# Author: Siddhant Jajoo
# Editor: Madhav Appanaboyina
# Modified for Assignment 4 Buildroot execution:

set -u

NUMFILES=10
WRITESTR=AELD_IS_FUN
WRITEDIR=/tmp/aeld-data

CONF_DIR=/etc/finder-app/conf
RESULT_FILE=/tmp/assignment4-result.txt

# Ensure required executables are in PATH
command -v writer >/dev/null 2>&1 || { echo "writer not found in PATH"; exit 1; }
command -v finder.sh >/dev/null 2>&1 || { echo "finder.sh not found in PATH"; exit 1; }

# Read username from target config location
username=$(cat "${CONF_DIR}/username.txt")

if [ $# -lt 3 ]
then
	echo "Using default value ${WRITESTR} for string to write"
	if [ $# -lt 1 ]
	then
		echo "Using default value ${NUMFILES} for number of files to write"
	else
		NUMFILES=$1
	fi
else
	NUMFILES=$1
	WRITESTR=$2
	WRITEDIR=/tmp/aeld-data/$3
fi

MATCHSTR="The number of files are ${NUMFILES} and the number of matching lines are ${NUMFILES}"

echo "Writing ${NUMFILES} files containing string ${WRITESTR} to ${WRITEDIR}"

rm -rf "${WRITEDIR}"

# Create $WRITEDIR if not assignment1
assignment=$(cat "${CONF_DIR}/assignment.txt")

if [ "${assignment}" != "assignment1" ]
then
	mkdir -p "${WRITEDIR}"

	if [ -d "${WRITEDIR}" ]
	then
		echo "${WRITEDIR} created"
	else
		exit 1
	fi
fi

for i in $(seq 1 ${NUMFILES})
do
	writer "${WRITEDIR}/${username}${i}.txt" "${WRITESTR}"
done

# Run finder using PATH; capture output and write required result file
OUTPUTSTRING=$(finder.sh "${WRITEDIR}" "${WRITESTR}")
echo "${OUTPUTSTRING}" > "${RESULT_FILE}"

set +e
echo "${OUTPUTSTRING}" | grep "${MATCHSTR}"
if [ $? -eq 0 ]; then
	echo "success"
	exit 0
else
	echo "failed: expected ${MATCHSTR} in ${OUTPUTSTRING} but instead found"
	exit 1
fi
