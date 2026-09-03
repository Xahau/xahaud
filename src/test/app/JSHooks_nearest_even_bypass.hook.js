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
    "61D84A8AFA8D4096130000000000000000000000005553440000000000B5F762798A53D543A014CAF8B297CFF8F2F937E8",
  );
  const right = decimal(
    "61D848026D25F9A8760000000000000000000000005553440000000000B5F762798A53D543A014CAF8B297CFF8F2F937E8",
  );

  let failure;
  let returned = false;
  try {
    left.divide(right);
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
      "TypeError: XFLDecimal.divide: arithmetic profile does not implement operation"
  ) {
    rollback("wrong profile backstop", 6063);
  }
  accept("xfl profile backstop", 6061);
}
