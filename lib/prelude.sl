// prelude.sl
// Standard Swazi prelude - common utilities embedded in the runtime
//
// This module is embedded into the Swazi binary and provides
// frequently-used helper functions that extend the standard library.
//
// Import with: import "prelude"
// Or selective: import { range, sleep } from "prelude"

// Generate a range of numbers
kazi _range(start, end, stp = 1) {
    data (result = [], i = start);
    wakati (i < end) {
        result.push(i)
        i = i + stp
    }
    rudisha result
}

// Sleep for milliseconds (placeholder - implement in C++)
kazi _sleep(ms) {
  kama !(/\d+/g.test(ms)) {
    tupa "invalid ms passed"
  }
  sleep(ms)
}

// Identity function for testing
kazi _identity(x) {
    rudisha x
}

// Placeholder for future utilities
kazi _noop() {
    // Does nothing
}

// Export public API
ruhusu { _range, _sleep, _identity, _noop }