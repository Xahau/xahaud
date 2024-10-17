import os

def replace_in_file(file_path):
    """Replace occurrences of 'from xrpl' with 'from xahau' in the given file."""
    try:
        with open(file_path, "r") as file:
            content = file.read()

        # Replace the text
        new_content = (
            content.replace("xrpld/app/", "ripple/app/")
            .replace("xrpld/core/", "ripple/core/")
            .replace("xrpld/nodestore/", "ripple/nodestore/")
            .replace("xrpl/basics/", "ripple/basics/")
            .replace("xrpl/protocol/", "ripple/protocol/")
            .replace("xrpl/json/", "ripple/json/")
            .replace("xrpld/overlay/", "ripple/overlay/")
            .replace("xrpl/resource/", "ripple/resource/")
            .replace("xrpl/crypto/", "ripple/crypto/")
            .replace("xrpl/beast/", "ripple/beast/")
            .replace("xrpld/shamap/", "ripple/shamap/")
            .replace("xrpld/rpc/", "ripple/rpc/")
            .replace("xrpld/perflog/", "ripple/perflog/")
            .replace("xrpld/nodestore/detail/", "ripple/nodestore/impl/")
            .replace("xrpld/ledger/", "ripple/ledger/")
            .replace("xrpld/app/misc/detail/AccountTxPaging.h", "ripple/app/misc/impl/AccountTxPaging.h")
            .replace("xrpld/perflog/PerfLog.h", "ripple/basics/PerfLog.h")
            .replace("xrpld/rpc/detail/RPCHelpers.h", "ripple/rpc/impl/RPCHelpers.h")
            .replace("xrpld/protocol/RPCErr.h", "ripple/net/RPCErr.h")
        )

        # Write the changes back to the file
        with open(file_path, "w") as file:
            file.write(new_content)
        print(f"Updated: {file_path}")

    except Exception as e:
        print(f"Error processing file {file_path}: {e}")


def search_and_replace_in_folders(folder_paths):
    """Search for Python files in the given list of folders and replace text."""
    for folder_path in folder_paths:
        for root, dirs, files in os.walk(folder_path):
            for file in files:
                if file.endswith(".cpp") or file.endswith(".h"):
                    file_path = os.path.join(root, file)
                    replace_in_file(file_path)


if __name__ == "__main__":
    folder_list = ["src/ripple", "src/test"]
    search_and_replace_in_folders(folder_list)