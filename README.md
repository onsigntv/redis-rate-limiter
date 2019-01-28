# redis-rate-limiter

A [Redis module][redis-modules] that provides rate limiting in Redis as a single command.
Implements the fairly sophisticated [generic cell rate algorithm][gcra]
which provides a rolling time window and doesn't depend on a background drip
process.

This implementation is based on amazing projects such as:

* [throttled](https://github.com/throttled/throttled): An implementation that can be used standalone or easily plugged into a Go HTTP stack.
* [redis-cell](https://github.com/brandur/redis-cell): An implementation in Rust that can be loaded from directly within Redis as a module.
* [redis-gcra](https://github.com/rwz/redis-gcra): An implementation that runs in Lua from within Redis and which provides a Ruby API.

While those projects where designed to be modular and flexible this is a straightforward implementation of the GCR algorithm, in 130 lines of C, without any external dependencies.

## Install

Clone and build this project from source.

```
$ git clone https://github.com/onsigntv/redis-rate-limiter.git
$ cd redis-rate-limiter
$ make
$ cp ratelimit.so /path/to/modules/
```

Run Redis pointing to the newly built module:

```
redis-server --loadmodule /path/to/modules/ratelimit.so
```

Alternatively add the following to a `redis.conf` file:

```
loadmodule /path/to/modules/ratelimit.so
```

## Usage

From Redis (try running `redis-cli`) use the new `RATER.LIMIT` command loaded by
the module. It's used like this:

```
RATER.LIMIT <key> <max_burst> <count per period> <period> [<quantity>]
```

Where `key` is an identifier to rate limit against. Examples might be:

* A user account's unique identifier.
* The origin IP address of an incoming request.
* A static string (e.g. `global`) to limit actions across the entire system.

For example:

```
RATER.LIMIT user123 15 30 60 1
               ▲     ▲  ▲  ▲ ▲
               |     |  |  | └───── apply 1 token (default if omitted)
               |     |  └──┴─────── 30 tokens / 60 seconds
               |     └───────────── 15 max_burst
               └─────────────────── key "user123"
```

### Response

This means that a single token (the `1` in the last parameter) should be
applied against the rate limit of the key `user123`. 30 tokens on the key are
allowed over a 60 second period with a maximum initial burst of 15 tokens. Rate
limiting parameters are provided with every invocation so that limits can
easily be reconfigured on the fly.

The command will respond with an array of integers:

```
127.0.0.1:6379> RATER.LIMIT user123 15 30 60
1) (integer) 0
2) (integer) 16
3) (integer) 15
4) (integer) -1
5) (integer) 2
```

The meaning of each array item is:

1. Whether the action was limited:
    * `0` indicates the action is allowed.
    * `1` indicates that the action was limited/blocked.
2. The total limit of the key (`max_burst` + 1). This is equivalent to the
   common `X-RateLimit-Limit` HTTP header.
3. The remaining limit of the key. Equivalent to `X-RateLimit-Remaining`.
4. The number of seconds until the user should retry, and always `-1` if the
   action was allowed. Equivalent to `Retry-After`.
5. The number of seconds until the limit will reset to its maximum capacity.
   Equivalent to `X-RateLimit-Reset`.

### Multiple Rate Limits

Implement different types of rate limiting by using different key names.

```
RATER.LIMIT user123-read-rate 15 30 60
RATER.LIMIT user123-write-rate 5 10 60
```


### Peeking The Value of a Key

You can use a quantity of `0` to inspect the current limit of a key without modifying it.

```
RATER.LIMIT user123 15 30 60 0
```

## License

This is free software under the terms of MIT the license (see the file
`LICENSE` for details).

[gcra]: https://en.wikipedia.org/wiki/Generic_cell_rate_algorithm
[redis-modules]: https://github.com/antirez/redis/blob/unstable/src/modules/INTRO.md
