# Xahau

[Xahau](https://xahau.network/) is a decentralized cryptographic ledger powered by a network of peer-to-peer nodes. The XAH Ledger uses a novel Byzantine Fault Tolerant consensus algorithm to settle and record transactions in a secure distributed database without a central operator.

## XAH
XAH is a public, counterparty-free asset native to Xahau, and is designed to bridge the many different currencies in use worldwide. XAH is traded on the open-market and is available for anyone to access. Xahau was created in 2023 with a finite supply of 600 million units of XAH.

## xahaud
The server software that powers Xahau is called `xahaud` and is available in this repository under the permissive [ISC open-source license](LICENSE.md). The `xahaud` server software is written primarily in C++ and runs on a variety of platforms. The `xahaud` server software can run in several modes depending on its configuration.

### Build from Source

* [Read the build instructions in our documentation](https://docs.xahau.network/infrastructure/building-xahau)
* If you encounter any issues, please [open an issue](https://github.com/xahau/xahaud/issues)

## Key Features of Xahau




## Source Code

Here are some good places to start learning the source code:

- Read the markdown files in the source tree: `src/ripple/**/*.md`.
- Read [the levelization document](./Builds/levelization) to get an idea of the internal dependency graph.
- In the big picture, the `main` function constructs an `ApplicationImp` object, which implements the `Application` virtual interface. Almost every component in the application takes an `Application&` parameter in its constructor, typically named `app` and stored as a member variable `app_`. This allows most components to depend on any other component.

### Repository Contents

| Folder     | Contents                                         |
|:-----------|:-------------------------------------------------|
| `./Builds` | Platform-specific guides for building `xahaud`.  |
| `./cfg`    | Example configuration files.                     |
| `./src`    | Source code.                                     |

Some of the directories under `src` are external repositories included using
git-subtree. See those directories' README files for more details.


## Additional Documentation

* [Xahau Documentation](https://docs.xahau.network/)
* [Setup and Installation](https://docs.xahau.network/infrastructure/building-xahau)