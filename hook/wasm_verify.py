import re
import sys

# Function to extract hexadecimal values from a C array in a .h file
def extract_hex_values(file_name):
    try:
        with open(file_name, "r") as file:
            content = file.read()
    except FileNotFoundError:
        print(f"File {file_name} not found.")
        return None

    # Find the C array using a regular expression
    match = re.search(r'\{(.+?)\};', content, re.DOTALL)
    if match:
        c_array = match.group(1)

        # Extract the hexadecimal values and remove the 'U' suffix
        hex_values = re.findall(r'0x([0-9A-F]+)U', c_array)
        return hex_values
    else:
        print(f"C array not found in the file {file_name}.")
        return None

# Check if the correct number of arguments are provided
if len(sys.argv) != 3:
    print("Usage: python script.py <file1.h> <file2.h>")
    sys.exit(1)

# File names of the .h files to compare
file_name1 = sys.argv[1]
file_name2 = sys.argv[2]

# Extract hexadecimal values from both files
hex_values1 = extract_hex_values(file_name1)
hex_values2 = extract_hex_values(file_name2)

print(len(hex_values1))
print(len(hex_values2))

# Compare the hexadecimal values if both files have been read successfully
if hex_values1 is not None and hex_values2 is not None:
    if hex_values1 == hex_values2:
        print("The hexadecimal values in both files match.")
    else:
        print("The hexadecimal values in both files do not match.")