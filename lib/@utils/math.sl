
/*
- This version is written and supported by swazi version v2.7.0^
- It is the alias of the Hesabu built object but polyfilled and refined
- To use this you need to import the module this module is an embeded runtime module 
- so no need for install or dowload it comes pre-installed within the swazi interpreter

- It will be modified, tools added and etc.
*/

data Math = {
  ...Hesabu
}

kazi Math_sqr(num) {
  kama ainaya num sisawa "namba" =>> rudisha null;
  kama num.isNaN na num.isInf =>> rudisha null;
  rudisha num.root()
}
Math.sqr = Math_sqr

kazi Math_pow(num, a=null) {
  kama ainaya num sisawa "namba" =>> rudisha null;
  kama num.isNaN na num.isInf =>> rudisha null;
  kama a {
    kama !a.ninamba au a.isNaN au a.isInf =>> rudisha null;
    rudisha num.pow(a)
  }
  rudisha num.pow()
}
Math.pow = Math_pow

Math.__proto__.freeze();
ruhusu Math