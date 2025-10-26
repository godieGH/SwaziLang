tumia regex kutoka "regex"
tumia JSON kutoka "json"

# namba_sahihi
kazi namba_sahihi n :
   kama n.nineno :
      kama regex.match(n, "\\D") { 
         rudisha sikweli
      }
      rudisha Namba(n).ninamba na !Namba(n).siSahihi
   vinginevyo:
      fanya {
         jaribu:
            rudisha n.ninamba na !n.siSahihi
         makosa err:
            rudisha sikweli
      }


# fn memoize tool
kazi memoize fn :
  data thabiti cache = {};
  
  rudisha (...args) => {
    data key = JSON.stringify(args)   
    
    kama Object.keys(cache).kuna(key):
      rudisha cache[key];
    
    data result = fn(...args)
    cache[key] = result;
    rudisha result
  }



ruhusu {
   namba_sahihi,
   memoize
}