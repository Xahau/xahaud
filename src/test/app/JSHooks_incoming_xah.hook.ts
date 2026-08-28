const MAX_INCOMING_DROPS = 100_000_000n;

/**
 * Accept bounded native-XAH Payments into this Hook account.
 *
 * Non-Payment transactions do not participate in this policy. Payment fields
 * are read from one certified local originating-transaction snapshot; field
 * access performs no additional host calls.
 */
export function main(): never {
  const transactionType = rollback.onFail(
    otxn.type(),
    "cannot classify originating transaction",
    1,
  );
  accept.when(
    transactionType !== TransactionType.Payment,
    "not an incoming Payment",
    0,
  );

  const transaction = otxn.object();
  const source = rollback.requirePresent(
    transaction.get(Field.Account),
    "Payment has no Account",
    10,
  );
  const destination = rollback.requirePresent(
    transaction.get(Field.Destination),
    "Payment has no Destination",
    11,
  );
  const amount = rollback.requirePresent(
    transaction.get(Field.Amount),
    "Payment has no Amount",
    12,
  );

  const hookAccount = hook.account();
  rollback.when(
    !destination.equals(hookAccount),
    "Payment is not addressed to this Hook account",
    20,
  );
  rollback.when(
    source.equals(hookAccount),
    "self-Payments are outside incoming-payment policy",
    21,
  );

  const nativeAmount = rollback.requirePresent(
    amount.asNative(),
    "Payment Amount must be native XAH",
    22,
  );
  rollback.when(
    nativeAmount.drops <= 0n || nativeAmount.drops > MAX_INCOMING_DROPS,
    "Payment Amount is outside the accepted range",
    23,
  );

  accept("incoming native XAH accepted", 0);
}
