#!/bin/bash

# Get the input file name from the command line argument
input_file="$1"
# Generate the hex output
hex_output=$(./qjsc -c -o /dev/stdout "$input_file" | tr -d '\n' | grep -Eo '\{[^}]+\}' | grep -Eo '0x[a-fA-F0-9]+' | sed -e 's/0x//g' | tr -d '\n')
# Convert the hex to a C array format
c_array=""
for ((i=0; i<${#hex_output}; i+=2)); do
    c_array+="0x${hex_output:$i:2}U, "
done
c_array="${c_array%, }"
c_array+=""
# Output the C array to the console
echo "$c_array"