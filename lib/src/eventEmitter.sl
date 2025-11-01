
/*

 ----- This src was written by the favor of swazi team to provide a way for for swazi developers to use the event emitter utility ----- 
 
*/

muundo Bus:
  &listeners = {}
  &cache = []
  &tabia emit(evt, ...payload) {
    kama !$.listeners[evt] :
      $.cache.ongeza({
        evt,
        payload
      })
      
      rudisha;
    kwa cb katika $.listeners[evt]:
      cb(...payload)
  }
  &tabia on(evt, cb):
    $.listeners[evt] = []
    $.listeners[evt].ongeza(cb)
    
    kama $.cache.idadi > 0 {
      kwa e katika $.cache:
        kama e.evt === evt =>> cb(...e.payload)
    }
  
  &tabia off(evt, cb) {
    // logic to put listeners off
    
    
    
    
  }
    
  
ruhusu {
  Bus
}

/* 
   
   
   ------ Code after here wont execute ----

*/


data bus = unda Bus;

// usage

bus.emit("evt", "ddd")
bus.emit("evt", "ddd")

bus.on("evt", (p) => {chapisha p})

