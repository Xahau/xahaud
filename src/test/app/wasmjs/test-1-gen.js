
 const ASSERT = (x, code) => {
 if (!x) {
 rollback(x.toString(), code);
 }
 }

 const Hook = (arg) => {
 let acc2 = hook_account();
 trace("acc2", acc2, false);
 ASSERT(acc2.length == 20);
 return accept(acc2, 0);
 }
 /*end*/
