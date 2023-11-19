import binascii

name: str = "tsh"
with open(f"build/{name}.wasm", "rb") as file:
        wasm = file.read()

data = wasm.hex().upper()
# print(data)

# Convert hexadecimal data to bytes
binary_data = binascii.unhexlify(data)

# Generate C array
c_array = ', '.join([f"0x{b:02x}U" for b in binary_data])

with open(f"hook/{name}.h", "w") as file:
    file.write(f'static const std::vector<uint8_t> {name.capitalize()}Hook = {"{"}')
    file.write(c_array)
    file.write("};\n")