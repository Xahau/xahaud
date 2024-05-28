
 const INVALID_ARGUMENT = -7
 const sfAccount = 0x80001

 const ASSERT = (x, code) => {
 if (!x) {
 rollback(x.toString(), code);
 }
 }

 const Hook = (arg) => {
 ASSERT(otxn_field(sfAccount) == 20);
 ASSERT(otxn_field(1) == INVALID_ARGUMENT);
 
 let acc2 = hook_account();
 ASSERT(acc2 == 20);

 for (var i = 0; i < 20; ++i)
 ASSERT(acc[i] == acc2[i]);

 return accept("0", 0);
 }
 /*end*/
