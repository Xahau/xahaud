import sys
import binascii

# Check if the name is provided as a command-line argument
if len(sys.argv) != 2:
    print("Usage: python script.py <name>")
    sys.exit(1)

name: str = sys.argv[1]

try:
    with open(f"build/{name}.wasm", "rb") as file:
        wasm = file.read()
except FileNotFoundError:
    print(f"Error: File build/{name}.wasm not found.")
    sys.exit(1)

data = wasm.hex().upper()

# Convert hexadecimal data to bytes
binary_data = binascii.unhexlify(data)

# Generate C array
c_array = ', '.join([f"0x{b:02X}U" for b in binary_data])

try:
    with open(f"hook/{name}_a.h", "w") as file:
        file.write(f'static const std::vector<uint8_t> {name.capitalize()}Hook = {"{"}')
        file.write(c_array)
        file.write("};\n")
except IOError as e:
    print(f"Error: Unable to write to file {name}_a.h. {e}")
    sys.exit(1)