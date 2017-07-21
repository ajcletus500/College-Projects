/*
 * Pseudo-random number generator
 * Not suitable for cryptographic stuff
 * (This is based on the "rand48" functions from FreeBSD; see copyright below.)
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
 * $Jab-id: prng.c,v 1.5 2003/12/12 01:08:52 jabrown Exp $
 */

/*
 * The "rand48" source from FreeBSD carried the following notice:
 *
 * Copyright (c) 1993 Martin Birgmeier
 * All rights reserved.
 *
 * You may redistribute unmodified or modified versions of this source
 * code provided that the above copyright notice and this and the
 * following conditions are retained.
 *
 * This software is provided ``as is'', and comes with no warranties
 * of any kind. I shall in no event be liable for anything that happens
 * to anyone/anything when using this software.
 */

const char RCSid_5437846[] = "$Jab-id: prng.c,v 1.5 2003/12/12 01:08:52 jabrown Exp $";

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>

/* OS-dependent stuff to get a better seed */
#ifndef FORCE_ANSI
#       include <fcntl.h>
#       include <unistd.h>
#       include <sys/time.h>
#endif

#include "prng.h"


#define RAND48_SEED_0   (0x330e)
#define RAND48_MULT_0   (0xe66d)
#define RAND48_MULT_1   (0xdeec)
#define RAND48_MULT_2   (0x0005)
#define RAND48_ADD      (0x000b)


static const char RandomDev[] = "/dev/urandom";


static void do_rand48(PRNGState *prng)
{
    unsigned long accu;
    unsigned short temp0, temp1;

    accu = (unsigned long) RAND48_MULT_0 * (unsigned long) prng->s0 +
        (unsigned long) RAND48_ADD;
    temp0 = (unsigned short) (accu & 0xffff);        /* lower 16 bits */
    accu >>= 16;                                /* shift 16 bits */
    accu += (unsigned long) RAND48_MULT_0 * (unsigned long) prng->s1 +
        (unsigned long) RAND48_MULT_1 * (unsigned long) prng->s0;
    temp1 = (unsigned short) (accu & 0xffff);        /* middle 16 bits */
    accu >>= 16;                                /* shift 16 bits */
    accu += RAND48_MULT_0 * prng->s2 + RAND48_MULT_1 * prng->s1 +
        RAND48_MULT_2 * prng->s0;
    prng->s0 = temp0;
    prng->s1 = temp1;
    prng->s2 = (unsigned short) (accu & 0xffff);
}


void prng_reset(PRNGState *prng, long seed)
{
    prng->s0 = RAND48_SEED_0;
    prng->s1 = (unsigned short) (seed & 0xffff);
    prng->s2 = (unsigned short) ((seed >> 16) & 0xffff);
}


void prng_reset_autoseed(PRNGState *prng)
{
#ifdef FORCE_ANSI

    prng_reset(prng, time(NULL));

#else
    int rfile;
    unsigned char seed[6];
    unsigned got_seed;

    got_seed = 0;

    rfile = open(RandomDev, O_RDONLY);
    if ((rfile != -1) && (read(rfile, seed, 6) == 6)) {
        prng->s0 = seed[0] | (seed[1] << 8);
        prng->s1 = seed[2] | (seed[3] << 8);
        prng->s2 = seed[4] | (seed[5] << 8);
        got_seed = 1;
    }

    if (!got_seed) {
        struct timeval time_now;
        unsigned pid;

        gettimeofday(&time_now, NULL);
        pid = getpid();
        prng->s0 = RAND48_SEED_0 ^ (time_now.tv_usec & 0xffff);
        prng->s1 = (time_now.tv_usec >> 16) ^ (time_now.tv_sec >> 16) ^ pid;
        prng->s2 = time_now.tv_sec & 0xffff;
        got_seed = 1;
    }

    if (rfile != -1)
        close(rfile);
    assert(got_seed);
#endif /* FORCE_ANSI */
}


long prng_next_long(PRNGState *prng)
{
    do_rand48(prng);
    return ((long) prng->s2 << 15) | ((long) prng->s1 >> 1);
    
}


double prng_next_double(PRNGState *prng)
{
    do_rand48(prng);
    return (ldexp((double) prng->s0, -48) +
        ldexp((double) prng->s1, -32) +
        ldexp((double) prng->s2, -16));
}
