tumia "async"
kazi debounce fn=null,delay=0 {
  kama (ainaya fn sisawa "kazi" au 
        ainaya delay sisawa "namba"
        ) {
    rudisha Makosa(`Debounce expects first argument to be a function(kazi/lambda), and a second optional delay(default 0ms) in ms, \n Yours got ${ainaya fn} and ${ainaya delay}`)
  }
  data timer;
  rudisha (...args) => {
    kama timer {clearTimeout(timer)}
    timer = setTimeout(() => {
      fn(...args)
    }, delay)
  }
}
ruhusu debounce