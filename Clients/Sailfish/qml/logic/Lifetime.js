// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
.pragma library
// Guards for asynchronous callbacks that may arrive after their page is destroyed (PLT-06).
// A page creates a token on completion, releases it on destruction, and wraps every callback
// it hands to a model or a timer. A released token turns the callback into a no-op, so a late
// answer never touches a dead page or acts for the wrong one. Tokens are plain JS objects:
// they outlive the page safely. Pure ES5 (NFR-TST-04).

var nextId = 1

function create(name) {
    return { id: nextId++, name: name || "", alive: true, generation: 0 }
}

function release(token) {
    if (token) token.alive = false
}

// Starts a new request generation; answers wrapped for an older generation are dropped.
// Use it when a page re-issues a request (new query, new video id).
function renew(token) {
    if (!token) return 0
    token.generation += 1
    return token.generation
}

function isCurrent(token, generation) {
    return !!token && token.alive && (generation === undefined || generation === token.generation)
}

// Wraps fn so it runs only while the token is alive (and, when bound to a generation, only
// for that generation). Returns whatever fn returns, or undefined when dropped.
function wrap(token, fn, bindGeneration) {
    var generation = bindGeneration ? token.generation : undefined
    return function () {
        if (!isCurrent(token, generation)) return undefined
        return fn.apply(null, arguments)
    }
}
