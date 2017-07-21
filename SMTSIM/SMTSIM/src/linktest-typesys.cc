//
// Trivial program to test standalone-linkability of SMTSIM type-system library
//
// Jeff Brown
// $Id: linktest-typesys.cc,v 1.1.2.2 2009/07/29 20:02:47 jbrown Exp $
//

const char RCSid_1248893252[] = 
"$Id: linktest-typesys.cc,v 1.1.2.2 2009/07/29 20:02:47 jbrown Exp $";

#include <stdio.h>

#include "sys-types.h"
#include "sim-assert.h"
#include "hash-map.h"           // not used here, just testing it


int
main(int argc, char *argv[])
{
    install_signal_handlers(argv[0], NULL, NULL);
    systypes_init();

    printf("A big 64-bit number: %s\n", fmt_i64(I64_MAX));
    printf("linktest-typesys: OK\n");
    printf("I'm going to assert-fail on purpose now.  Goodbye!\n\n");

    sim_assert(0);

    return 0;
}
