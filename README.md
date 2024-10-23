# Xahau 

**Note:** Throughout this README, references to "we" or "our" pertain to the community and contributors involved in the Xahau network. It does not imply a legal entity or a specific collection of individuals.

[Xahau](https://xahau.network/) is a decentralized cryptographic ledger that builds upon the robust foundation of the XRP Ledger. It inherits the XRP Ledger's Byzantine Fault Tolerant consensus algorithm and enhances it with additional features and functionalities. Developers and users familiar with the XRP Ledger will find that most documentation and tutorials available on [xrpl.org](https://xrpl.org) are relevant and applicable to Xahau, including those related to running validators and managing validator keys. For Xahau specific documentation you can visit our [documentation](https://docs.xahau.network/)

## XAH
XAH is the public, counterparty-free asset native to Xahau and functions primarily as network gas. Transactions submitted to the Xahau network must supply an appropriate amount of XAH, to be burnt by the network as a fee, in order to be successfully included in a validated ledger. In addition, XAH also acts as a bridge currency within the Xahau DEX. XAH is traded on the open-market and is available for anyone to access. Xahau was created in 2023 with a supply of 600 million units of XAH.

## xahaud
The server software that powers Xahau is called `xahaud` and is available in this repository under the permissive [ISC open-source license](LICENSE.md). The `xahaud` server software is written primarily in C++ and runs on a variety of platforms. The `xahaud` server software can run in several modes depending on its configuration.

### Build from Source

* [Read the build instructions in our documentation](https://docs.xahau.network/infrastructure/building-xahau)
* If you encounter any issues, please [open an issue](https://github.com/xahau/xahaud/issues)

## Highlights of Xahau

1. **Hooks**: Hooks are small, efficient WebAssembly modules designed specifically for Xahau. They add a robust smart contract functionality to Xahau, allowing you to construct and deploy applications with bespoke functionalities. Hooks can block or allow transactions to and from the account, change and keep track of the hook’s internal state and logic, and autonomously initiate new transactions on the account’s behalf. They can be written in any language that can be compiled into WebAssembly.

2. **Balance Rewards**: Xahau offers a Balance Rewards feature that provides a 4% per annum reward. This feature encourages users to maintain a balance in their accounts and rewards them for doing so.

3. **URIToken**: The URIToken is a feature in Xahau that allows for the creation and management of non fungible tokens within the network. This feature can be used for a variety of purposes and specific use cases.

4. **Import/B2M**: The Import/B2M feature in Xahau allows for the importation of assets into the network. This feature can be used to bring external assets into the Xahau network, expanding the range of assets that can be managed and traded within the network.

5. **Governance Game**: The Governance Game is a feature in Xahau that allows for the decentralized governance of the network. This feature allows users to participate in the decision-making process of the network, ensuring that the network remains democratic and responsive to the needs of its users.

## Binary Releases and Versioning System

Xahau provides pre-compiled binary releases of its software, which are ready-to-run versions that users can download and execute without compiling the source code themselves. These binaries are built automatically using GitHub Actions whenever a new commit is pushed or a pull request is merged.

The versioning system for Xahau binaries is based on the date of the build, the branch name, and a build number, following the format `YYYY.MM.DD-branch+buildnumber`. For example, `2023.10.30-release+443` indicates a binary built on October 30, 2023, from the `release` branch, and it is the 443rd build from that branch.

Users can access these binaries on the [Xahau Build Server](https://build.xahau.tech/), which provides an organized list of releases along with release notes for each version. This system simplifies the deployment process for users and ensures they can easily identify and download the appropriate version for their needs.

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

## Resources

- **Documentation**: Documentation for XRPL, Xahau and Hooks.
  - [Xrpl Documentation](https://xrpl.org)
  - [Xahau Documentation](https://docs.xahau.network/)
  - [Hooks Technical Documentation](https://xrpl-hooks.readme.io/)
- **Explorers**: Explore the Xahau ledger using various explorers:
  - [xahauexplorer.com](https://xahauexplorer.com)
  - [xahscan.com](https://xahscan.com)
  - [xahau.xrpl.org](https://xahau.xrpl.org)
  - [explorer.xahau.network](https://explorer.xahau.network)
- **Testnet & Faucet**: Test applications and obtain test XAH at [xahau-test.net](https://xahau-test.net) and use the testnet explorer at [explorer.xahau.network](https://explorer.xahau.network).
- **Supporting Wallets**: A list of wallets that support XAH and Xahau-based assets.
  - [Xumm](https://xumm.app)
  - [Crossmark](https://crossmark.io)
