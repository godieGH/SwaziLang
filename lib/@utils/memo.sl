tumia JSON kutoka "json"

kazi memo fn, ...o :
  data thabiti cache = {};
  
  # args validations
  kama !fn.nikazi {rudisha Makosa(`memo only allows a function as an argument, you passed ${ainaya fn}`)}
  kama o.urefu() {rudisha Makosa(`memo only take one function arg it has two in this case`)}
  
  rudisha (...args) => {
    data key = JSON.stringify(args)   
    
    kama Object.keys(cache).kuna(key):
      rudisha cache[key];
    
    data result = fn(...args)
    cache[key] = result;
    rudisha result
  }

ruhusu memo