//reworked version of https://github.com/RichardAH/xrpl-tools/blob/master/validator-address-tool/vatool.js
const rac = require('ripple-address-codec');
const { XrplClient } = require('xrpl-client');
const { sign, derive, XrplDefinitions } = require('xrpl-accountlib');
const ed = require('elliptic').eddsa;
const ec = new ed('ed25519');

if (process.argv.length < 6)
{
    console.log("Rekey validator address tool");
    console.log("Usage: node " + process.argv[1] + " validator_secret_key raddr_to_rekey_to fee_drops network_id [ws://localhost:6005]");
    process.exit(1);
}

const sk = (()=>
{
    return 'ED' + 
        Buffer.from(ec.keyFromSecret(rac.codec._codec.decode(process.argv[2]).slice(1,33)).secret()).
        toString('hex').toUpperCase();
})();


console.log(sk)
const account = derive.privatekey(sk);
console.log(account);

const endpoint = process.argv.length >= 7 ? process.argv[6] : 'ws://localhost:6005';
console.log("endpoint:", endpoint);
const client = new XrplClient(endpoint);

(async () => {
  const liveDefinitions = await client.send({ "command": "server_definitions" })

  const definitions = new XrplDefinitions(liveDefinitions)

  const ledgerInfo = await client.send({
  	command: 'ledger'
  })
  
  // console.log(ledgerInfo)

  const accountInfo = await client.send({
  	command: 'account_info',
    account: account.address,
  })
 
  const txJsonPreSigning = {
    TransactionType: 'SetRegularKey',
    Account: account.address,
    RegularKey: process.argv[3],
    Fee: process.argv[4] + '',
    LastLedgerSequence: Number(ledgerInfo.closed.ledger.seqNum) + 5,
    Sequence: accountInfo.account_data.Sequence,
    NetworkID: process.argv[5]
  };

  const signed = sign(txJsonPreSigning, account, definitions);

  console.log('Tx', signed.id);
  console.log(signed.signedTransaction);

  const submit = await client.send({
    command: 'submit',
    tx_blob: signed.signedTransaction
  });

  console.log(submit);
})();

