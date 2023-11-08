# Release Notes

This document contains the release notes for `xahaud`, the reference server implementation of the Xahau protocol. To learn more about how to build, run or update a `xahaud` server, visit https://docs.xahau.network/infrastructure/building-xahau

 
Have new ideas? Need help with setting up your node? [Please open an issue here](https://github.com/xrplf/rippled/issues/new/choose).

# Introducing Xahau version 2023.10.30-release+443

Version 2023.10.30-release+443 of `xahaud`, the reference server implementation of the Xahau protocol, is now available.

[Sign Up for Future Release Announcements](https://groups.google.com/g/xahau-server)

<!-- BREAK -->

## Action Required

New amendments are now open for voting according to Xahau's [amendment process](https://docs.xahau.network/features/amendments), which enables protocol changes following five days of >80% support from trusted validators.

If you operate a Xahau server, upgrade to version 2023.10.30-release+443 by November 1 to ensure service continuity. The exact time that protocol changes take effect depends on the voting decisions of the decentralized network.


## Install / Upgrade

On supported platforms, see the [instructions on installing or updating `xahaud`](https://docs.xahau.network/infrastructure/building-xahau).


## New Amendments

- **`Hooks`**: This amendment activates hooks and the hook API in the Xahau network, allowing custom logic to be executed on the ledger in response to transactions.

- **`BalanceRewards`**: This amendment enables `ClaimReward`` and `GenesisMint`` transactions, facilitating balance rewards to be paid in XAH, the Xahau network's native currency.

- **`PaychanAndEscrowForTokens`**: This amendment allows the use of IOU tokens for PaymentChannels and Escrow transactions, enhancing flexibility and functionality.

- **`URIToken`**: This amendment activates URITokens, which are non-fungible, hook-friendly tokens in the Xahau network.

- **`Import`**: This amendment enables the Import transaction for B2M xpop processing, allowing transactions to be imported from another network or system.

- **`XahauGenesis`**: This amendment activates the genesis amendment for the initial distribution of XAH and the establishment of the governance game.

- **`HooksUpdate1`**: This amendment extends the hooks API to include Xpop functionality, enabling more complex transactions involving Xpop.

## Changelog

### New Features and Improvements

- **Server Definitions**: The new feature introduces a `server_definitions` endpoint. This endpoint is designed to return a JSON object that contains the definitions of various types, fields, and transaction results used in the Xahau protocol.

This feature enhances the functionality of the system by providing an efficient way to fetch and verify the current definitions used in the Ripple protocol.

### GitHub

The public source code repository for `xahaud` is hosted on GitHub at <https://github.com/xahau/xahaud>.

We welcome all contributions and invite everyone to join the community of Xahau developers to help build the Internet of Value.

### Credits

The following people contributed directly to this release:

- Nikolaos D. Bougalis <nikb@bougalis.net>
- Wietse Wind <wietse@xrpl-labs.com>
- Richard Holland <richard@xrpl-labs.com>
- Denis Angell <denis@xrpl-labs.com>

Bug Bounties and Responsible Disclosures:
We welcome reviews of the rippled code and urge researchers to
responsibly disclose any issues they may find.

To report a bug, please send a detailed report to:

    bugs@xrpl.org