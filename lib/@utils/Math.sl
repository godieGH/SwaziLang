

data Math = {
  ...Hesabu
}


kazi Math_sqr(n) {
  rudisha n.root()
}
Math.sqr = Math_sqr

kazi Math_pow(num, a=null) {
  kama a =>> rudisha num.pow(a)
  rudisha num.pow(2)
}
Math.pow = Math_pow


Math.__proto__.freeze();
ruhusu Math