kazi throttle fn=null, delay=0 {
  kama (ainaya fn sisawa "kazi" au 
        ainaya delay sisawa "namba"
        ) {
    rudisha Makosa(`Throttle expects first argument to be a function(kazi/lambda), and a second optional delay(default 0ms) in ms, \n Yours got ${ainaya fn} and ${ainaya delay}`)
  }
  data last = 0;
  rudisha (...args) => {
    data thabiti now = muda();
    kama now - last >= delay {
      last = now;
      fn(...args)
    }
  }
}
ruhusu throttle