/*
 * Copyright (c) 2018 OnSign TV Limited
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdlib.h>
#include <sys/time.h>
#include "redismodule.h"

/* rater_limit checks whether a particular key has exceeded a rate limit.
 * burst defines the maximum amount permitted in a single instant while
 * count_per_period / period_in_sec defines the maximum sustained rate. */
static long long rater_limit(long long tat, long long burst,
                             long long count_per_period,
                             long long period_in_sec, long long amount,
                             long long *limited, long long *limit,
                             long long *remaining, long long *retry_after,
                             long long *ttl) {
  *limited = 0;
  *retry_after = -1;
  *limit = burst + 1;
  *remaining = 0;

  /* emission_interval is the time between events in the nominal equally
   * spaced events. If you like leaky buckets, think of it as how frequently
   * the bucket leaks one unit. */
  long long emission_interval =
      (long long)(((double)period_in_sec / count_per_period) * 1000000LL);

  /* delay_variation_tolerance is our flexibility:
   * How far can you deviate from the nominal equally spaced schedule?
   * If you like leaky buckets, think about it as the size of your bucket. */
  long long delay_variation_tolerance = emission_interval * (burst + 1);

  /* current time in microseconds to increase precision */
  struct timeval tv;
  gettimeofday(&tv, NULL);
  long long now = ((long long)tv.tv_sec) * 1000000LL + tv.tv_usec;

  /* tat refers to the theoretical arrival time that would be expected
   * from equally spaced requests at exactly the rate limit. */
  if (tat == 0) {
    tat = now;
  }

  long long increment = emission_interval * amount;
  long long new_tat;
  if (now > tat) {
    new_tat = now + increment;
  } else {
    new_tat = tat + increment;
  }

  /* Block the request if the next permitted time is in the future. */
  long long allow_at = new_tat - delay_variation_tolerance;
  long long diff = now - allow_at;
  if (diff < 0) {
    new_tat = 0;
    *limited = 1;
    *ttl = tat - now;
    if (increment <= delay_variation_tolerance) {
      *retry_after = -diff / 1000000LL;
    }
  } else {
    *ttl = new_tat - now;
  }

  long long next = delay_variation_tolerance - *ttl;
  if (next > -emission_interval) {
    *remaining = next / emission_interval;
  }

  *ttl = *ttl / 1000000LL;

  return new_tat;
}

int RaterLimit_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv,
                            int argc) {
  /* RATER.LIMIT <key> <burst> <count per period> <period> [<quantity>] */
  if (argc < 5 || argc > 6) return RedisModule_WrongArity(ctx);

  /* Parse and validate the arguments, in their order. */
  long long tat = 0;
  RedisModuleKey *key =
      RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
  /* Key must be empty or string. */
  if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_STRING) {
    /* Given how the Redis API works, we first must get a RedisModuleString from
     * the key. */
    size_t len;
    const char *raw_tat_str =
        RedisModule_StringDMA(key, &len, REDISMODULE_READ);
    RedisModuleString *tat_str =
        RedisModule_CreateString(ctx, raw_tat_str, len);

    if (RedisModule_StringToLongLong(tat_str, &tat) != REDISMODULE_OK) {
      RedisModule_FreeString(ctx, tat_str);
      RedisModule_CloseKey(key);
      return RedisModule_ReplyWithError(ctx, "ERR invalid stored rater");
    }
    RedisModule_FreeString(ctx, tat_str);
  } else if (RedisModule_KeyType(key) != REDISMODULE_KEYTYPE_EMPTY) {
    /* If the key is not a string and is not empty it is the wrong type. */
    RedisModule_CloseKey(key);
    return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
  }

  long long burst = 0;
  if (RedisModule_StringToLongLong(argv[2], &burst) != REDISMODULE_OK ||
      burst < 0) {
    RedisModule_CloseKey(key);
    return RedisModule_ReplyWithError(ctx, "ERR invalid burst");
  }

  long long count_per_period = 0;
  if (RedisModule_StringToLongLong(argv[3], &count_per_period) !=
          REDISMODULE_OK ||
      count_per_period <= 0) {
    RedisModule_CloseKey(key);
    return RedisModule_ReplyWithError(ctx, "ERR invalid count_per_period");
  }

  long long period_in_sec;
  if (RedisModule_StringToLongLong(argv[4], &period_in_sec) != REDISMODULE_OK ||
      period_in_sec <= 0) {
    RedisModule_CloseKey(key);
    return RedisModule_ReplyWithError(ctx, "ERR invalid period_in_sec");
  }

  long long amount = 1L;
  if (argc == 6) {
    if (RedisModule_StringToLongLong(argv[5], &amount) != REDISMODULE_OK ||
        amount < 1) {
      RedisModule_CloseKey(key);
      return RedisModule_ReplyWithError(ctx, "ERR invalid amount");
    }
  }

  /* After all that preamble, do the Cell-Rate Limiting calculations. */
  long long limited = 0, limit = 0, remaining = 0, retry_after = 0, ttl = 0;
  long long new_tat =
      rater_limit(tat, burst, count_per_period, period_in_sec, amount, &limited,
                  &limit, &remaining, &retry_after, &ttl);

  /* If there is a new theoretical arrival time, store it back on the key. */
  if (new_tat > 0) {
    RedisModuleString *new_tat_str =
        RedisModule_CreateStringFromLongLong(ctx, new_tat);
    RedisModule_StringSet(key, new_tat_str);
    RedisModule_SetExpire(key, ttl * 1000);
    RedisModule_FreeString(ctx, new_tat_str);
  }

  RedisModule_CloseKey(key);

  RedisModule_ReplyWithArray(ctx, 5);
  /* Limited is 0 if not limited, 1 if limited. */
  RedisModule_ReplyWithLongLong(ctx, limited);
  /* Limit is burst + 1 */
  RedisModule_ReplyWithLongLong(ctx, limit);
  /* Remaining count ranges from zero to limit withing a period. */
  RedisModule_ReplyWithLongLong(ctx, remaining);
  /* Retry after this many of seconds to get through or -1 if not limited. */
  RedisModule_ReplyWithLongLong(ctx, retry_after);
  /* Amount of seconds to wait until both the burst and the rate restarts. */
  RedisModule_ReplyWithLongLong(ctx, ttl);

  return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv,
                       int argc) {
  REDISMODULE_NOT_USED(argv);
  REDISMODULE_NOT_USED(argc);
  if (RedisModule_Init(ctx, "rater", 1, REDISMODULE_APIVER_1) ==
      REDISMODULE_ERR) {
    return REDISMODULE_ERR;
  }

  if (RedisModule_CreateCommand(ctx, "rater.limit", RaterLimit_RedisCommand,
                                "write deny-oom random", 0, 0,
                                0) == REDISMODULE_ERR) {
    return REDISMODULE_ERR;
  }

  return REDISMODULE_OK;
}
