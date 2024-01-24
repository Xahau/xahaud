import re
import sys

# Check if the file name is provided as a command-line argument
if len(sys.argv) < 2:
    print("Usage: python script.py <filename>")
    sys.exit(1)

# Read the .h file content
file_name = sys.argv[1]  # Get the file name from command-line arguments
try:
    with open(file_name, "r") as file:
        content = file.read()

    # Find the C array using a regular expression
    match = re.search(r'\{(.+?)\};', content, re.DOTALL)
    if match:
        c_array = match.group(1)

        # Extract the hexadecimal values and remove the 'U' suffix
        hex_values = re.findall(r'0x([0-9A-F]+)U', c_array)

        # Convert the list of hex values to a byte array
        byte_array = bytearray.fromhex(''.join(hex_values))

        # Convert the byte array to a hex string
        hex_string = byte_array.hex().upper()

        print(hex_string)
    else:
        print("C array not found in the file.")
except FileNotFoundError:
    print(f"File not found: {file_name}")
except Exception as e:
    print(f"An error occurred: {e}")
