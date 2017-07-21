/*
 * Pseudo-random number generator
 * Not suitable for cryptographic stuff
 *
 * Copyright (c) 2001, 2002 Jeff Brown
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice this list of conditions, and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Absolutely no warranty of function or purpose is made by the author.
 *
 * $Jab-id: prng.h,v 1.3 2002/05/31 01:30:45 jabrown Exp $
 */

#ifndef PRNG_H
#define PRNG_H

#ifdef __cplusplus
extern "C" {
#endif

#define PRNG_MAX        2147483647

/* Don't mess with these fields at all; they're here for convenience */
struct PRNGState { unsigned short s0, s1, s2; };
typedef struct PRNGState PRNGState;

/*
 * prng_reset() initializes the PRNG with the low 32 bits of "seed" as a seed.
 *  Two PRNGs given identical seed values will produce identical streams of
 *  random numbers.
 * prng_reset_autoseed() initializes the PRNG, deriving a seed on its own.
 */
extern void prng_reset(PRNGState *prng, long seed);
extern void prng_reset_autoseed(PRNGState *prng);

/*
 * prng_next_long() gets the next value from the PRNG as a long int, returning
 *  a value from 0...PRNG_MAX (defined above).
 * prng_next_double() gets the next value from the PRNG as a double, returning
 *  a value on [0,1).
 */
extern long prng_next_long(PRNGState *prng);
extern double prng_next_double(PRNGState *prng);

#ifdef __cplusplus
}
#endif

#endif  /* PRNG_H */
