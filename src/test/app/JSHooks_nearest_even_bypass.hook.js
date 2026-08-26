export const hookConfig = defineHookConfig({
  xflArithmetic: XFLProfile.nearestEvenV1,
});

function decimal(hex) {
  return rollback
    .requirePresent(
      util.decodeObject(STBlob.fromHex(hex)).get(Field.Amount),
      "bypass amount",
    )
    .toXFL();
}

export function main(_reserved) {
  void _reserved;
  const left = decimal(
    "61D8438D7EA4C680010000000000000000000000005553440000000000B5F762798A53D543A014CAF8B297CFF8F2F937E8",
  );
  const right = decimal(
    "61D451C37937E080000000000000000000000000005553440000000000B5F762798A53D543A014CAF8B297CFF8F2F937E8",
  );

  let failure;
  let returned = false;
  try {
    left.add(right);
    returned = true;
  } catch (error) {
    failure = error;
  }

  if (returned || failure === undefined) {
    rollback.onFail(
      state.set("xfl-bypass", "unexpected-return"),
      "bypass sentinel write failed",
    );
    accept("unexpected arithmetic return", 6062);
  }
  if (
    !(failure instanceof TypeError) ||
    String(failure) !==
      "TypeError: XFLDecimal.add: arithmetic profile does not implement operation"
  ) {
    rollback("wrong profile backstop", 6063);
  }
  accept("xfl profile backstop", 6061);
}
