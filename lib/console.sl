tumia regex kutoka "regex"

kazi print(...txt):
  kama txt.urefu() === 1:
    chapisha txt[0];
    rudisha;
  
  # if multiple args
  kwa kila t katika txt:
    andika t + " "
  andika "\n"
  rudisha;

kazi printf(str, ...args):
  # check if there is {} in the string if not return the string as is
  kama str.nineno na !str.kuna("{}"):
    chapisha str
    rudisha;
  
  kama str.nineno:
    data { replace } = regex;
    kwa kila arg katika args:
      str = replace(str, "\\{\\}", arg);
  
  chapisha str;
  rudisha;

kazi error(...arg):
  swazi.cerr(...arg)


# export tools
ruhusu {
  print,
  printf,
  error
}