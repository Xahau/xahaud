# Release Notes

This document contains the release notes for `xahaud`, the reference server implementation of the XAH Ledger protocol. To learn more about how to build, run or update a `xahaud` server, visit https://docs.xahau.network/infrastructure/building-xahau

 
Have new ideas? Need help with setting up your node? [Please open an issue here](https://github.com/xrplf/rippled/issues/new/choose).

# Introducing XAH Ledger version 2023.10.30-release+443

Version 2023.10.30-release+443 of `xahaud`, the reference server implementation of the XAH Ledger protocol, is now available.

[Sign Up for Future Release Announcements](https://groups.google.com/g/xahau-server)

<!-- BREAK -->

## Action Required

New amendments are now open for voting according to the XAH Ledger's [amendment process](https://docs.xahau.network/features/amendments), which enables protocol changes following 5 days of >80% support from trusted validators.

If you operate an XAH Ledger server, upgrade to version 2023.10.30-release+443 by November 1 to ensure service continuity. The exact time that protocol changes take effect depends on the voting decisions of the decentralized network.


## Install / Upgrade

On supported platforms, see the [instructions on installing or updating `xahaud`](https://docs.xahau.network/infrastructure/building-xahau).


## New Amendments

- **`Hooks`**: Enables hooks and the hook api.
- **`BalanceRewards`**: Enables ClaimReward and GenesisMint transactions to allow for balance rewards paid in XAH.
- **`PaychanAndEscrowForTokens`**: Enables IOUs for PaymentChannels and Escrow.
- **`URIToken`**: Enables URITokens, Xahau's non fungible hook friendly tokens.
- **`Import`**: Enables the Import transaction for B2M xpop processing.
- **`XahauGenesis`**: Enables the genesis amendment for the initial distribution of XAH and the governance game.
- **`HooksUpdate1`**: Adds Xpop functionality to the hooks api.

## Changelog

### New Features and Improvements

- **Server Definitions**: Adds `server_definitions` endpoint which returns the definitions.json in an rpc endpoint.

### GitHub

The public source code repository for `xahaud` is hosted on GitHub at <https://github.com/xahau/xahaud>.

We welcome all contributions and invite everyone to join the community of XAH Ledger developers to help build the Internet of Value.

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