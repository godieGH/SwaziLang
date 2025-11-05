tumia regex kama rg

kazi no_wrap(str):
  # validate str
  kama ainaya str sisawa "neno":
    rudisha Makosa("no_wrap only takes a string(neno) as argument.")
  
  # if str is a string
  rudisha str.badilishaZote("\n", "")


kazi f(str, ...args) {
  kama si str.nineno =>> rudisha null;
  kama !str.kuna("{}") =>> rudisha str;
  
  data { replace } = rg;
  args.kwaKila((a) => {
    str = replace(str, "\\{\\}", a)
  })
  
  rudisha str;
}

ruhusu {
  no_wrap,
  f
}