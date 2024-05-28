
 const M_REPEAT_10 = (X) => X.repeat(10);
 const M_REPEAT_100 = (X) => M_REPEAT_10(X).repeat(10);
 const M_REPEAT_1000 = (X) => M_REPEAT_100(X).repeat(10);
 const Hook = (arg) => {
 const ret = M_REPEAT_1000("abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz01234567890123");
 return accept(ret, 0);
 }
 /*end*/
