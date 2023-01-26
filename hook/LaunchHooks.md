# Xahau Launch Hooks

## Savings Hook
When it comes to managing money, it is a common practice to maintain separate spending and savings accounts.
Suppose you receive an income or salary to your on-ledger account several times a month. Each time you receive funds above a certain threshold you may wish to move a predefined percentage to a savings account where you will not accidentally spend them. The savings hook does exactly this.
### Hook Parameters
  1. Account to install on
  2. Account to send savings to
  3. The percentage to send
  4. The threshold at which it is activated
  5. Whether it applies to incoming payments only, outgoing payments only or both.
### xApp Features
  1. Display the send-to account
  2. Display the percentage
  3. Display the total amount sent
  4. Display the conditions of sending (threshold + incoming/outgoing payments)
  5. Allow the hook to be uninstalled

## Firewall Hook
The ledger is a messy place full of unwanted transactions and spam. To avoid being spammed with low value transactions containing unsolicitied memos you may install a Firewall hook on your account.
### Hook Parameters
 1. Types of transactions to allow into and out of your account (Payment, Escrow, PayChannel) etc.
 2. Allow a minimum number of drops for an incoming txn to be allowed.
 3. Allow a minimum amount to be specified for each of the trustline assets on the account as well.
 4. Allow any txn with a memo larger than X bytes to be blocked regardless of other rules.
### xApp Features
 1. Display the current settings of the hook. Allow the settings to be changed.
 2. Allow the hook to be uninstalled.

## Blocklist Hook
Filter outgoing and incoming payments against a known list of scam accounts maintained by a third party. This acts as a guard against accidentally sending to a scam, or being sent tainted funds by a scammer.
### Hook Parameters
 1. The blocklist (account) to listen to.
### xApp Features
 1. Number of times a transaction was blocked.
 2. The current blocklist (account) being listened to.
 3. Allow the hook to be uninstalled.

## Direct Debit Hook
Allow trusted third parties to pull funds from your account up to a limit you set. For example your power company can bill you and your account can automatically pay that bill.
### Hook Parameters
 1. One or more accounts to provide direct deposit authorization to.
 2. A currency and a limit for each of these.
## xApp Features
 1. See who you've authorized.
 2. See how much they're authorized for.
 3. See how much they've drawn down this month.
 4. Allow authorization to be removed.
 5. Allow authorization limit to be changed.
 6. Allow additional authorizations to be created.
 7. Allow the hook to be uninstalled.
 8. Show a list of recent direct debit transactions.


## High-Value Payment Hook
When sending high value transactions out of your account, require first a notification that a high valued payment will be made, followed by a time delay, followed by the high value transaction itself. This prevents accidental high value sends, adding an additional layer of security to your account.
### Hook Parameters
 1. Select currencies for which the hook will act.
 2. Select the thresholds for which the hook will be triggered.
### xApp Features
 1. See current pending outgoing high value transactions.
 2. State that the hook is active and for which currencies and thresholds.
 3. Allow the hook to be uninstalled.
 4. If installed, and a high value transaction is made from Xumm, it is redirected into the xApp.
 5. The xApp then generates a notification transaction (ttInvoke) which is sent to the hook.
 6. The xApp will then remind the user with an event at a later time that the transaction proper still needs to be sent.
 7. Sending the transaction proper again will result in successful send.

