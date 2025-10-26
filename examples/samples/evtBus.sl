
muundo Bus:
  &listeners = {}
  &cache = []
  &tabia emit(evt, ...payload):
    kama !$.listeners[evt] :
      $.cache.ongeza({evt:evt,payload:payload})
      
      rudisha;
    kwa kila cb katika $.listeners[evt]:
      cb(...payload)
  
  &tabia on(evt, cb):
    $.listeners[evt] = []
    $.listeners[evt].ongeza(cb)
    
    kama $.cache.idadi > 0:
      kwa kila e katika $.cache:
        kama e.evt === evt:
          cb(...e.payload)
    
data bus = unda Bus;

// usage

bus.emit("evt", "ddd")
bus.emit("evt", "ddd")

bus.on("evt", (p) => {
  chapisha p
})

