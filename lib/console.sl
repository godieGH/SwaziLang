
kazi print(...txt):
  kama txt.urefu() === 1:
    chapisha txt[0];
    rudisha;
  
  # if multiple args
  kwa kila t katika txt:
    andika t + " "
  andika "\n"
  rudisha;

ruhusu {
  print
}