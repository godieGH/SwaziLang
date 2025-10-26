
# this module imports async and make subiri and nap available

tumia "async"

# Robust setTime (single-shot)
kazi setTime cb, ms {
   data stopped = sikweli
   data start = muda("ms")
   data id = {
      stop: () => { stopped = kweli }
   }

   subiri(() => {
      wakati kweli {
         kama stopped { simama }                # cancel requested
         data now = muda("ms")
         kama (now - start) >= ms {
            cb()
            simama
         }
         nap(10)                                # yield a bit to avoid tight spin (optional)
      }
   })

   rudisha id
}

# Robust interval (repeating)
kazi interval cb, ms {
   data stopped = sikweli
   data last = muda("ms")
   data id = {
      stop: () => { stopped = kweli }
   }

   subiri(() => {
      wakati kweli {
         kama stopped { simama }
         data now = muda("ms")
         kama (now - last) >= ms {
            # compute how many whole intervals passed to catch up
            data times = (now - last) / ms
            kwa kila i katika Orodha(times) {
               cb()
            }
            last = last + times * ms
         }
         nap(10)
      }
   })

   rudisha id
}

ruhusu {
   setTime, 
   interval
}


# the problem is the interval runs without even giving a gap for other setTime to execute like from the main i want to clear it