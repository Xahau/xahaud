# Xahau

[Xahau](https://xahau.network/) is a decentralized cryptographic ledger powered by a network of peer-to-peer nodes. Xahau uses the xrp ledger Byzantine Fault Tolerant consensus algorithm to settle and record transactions in a secure distributed database without a central operator.

## XAH
XAH is the public, counterparty-free asset native to Xahau and functions primarily as network gas. Transactions submitted to the Xahau network must supply an appropriate amount of XAH, to be burnt by the network as a fee, in order to be successfully included in a validated ledger. In addition, XAH also acts as a bridge currency within the Xahau DEX. XAH is traded on the open-market and is available for anyone to access. Xahau was created in 2023 with a finite supply of 600 million units of XAH.

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
* [Hooks Documentation](https://xrpl-hooks.readme.io/)
* [Setup and Installation](https://docs.xahau.network/infrastructure/building-xahau)