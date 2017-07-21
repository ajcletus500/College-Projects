//
// Trivial program to test standalone-linkability of SMTSIM utility modules
//
// Jeff Brown
// $Id: linktest-utils.cc,v 1.1.2.1 2009/07/29 20:55:33 jbrown Exp $
//

const char RCSid_1248896773[] = 
"$Id: linktest-utils.cc,v 1.1.2.1 2009/07/29 20:55:33 jbrown Exp $";

#include <stdio.h>

#include "sys-types.h"
#include "sim-assert.h"
#include "utils.h"
#include "utils-cc.h"
#include "online-stats.h"


int
main(int argc, char *argv[])
{
    BasicStat_I64 some_stats;
    BiConfusionMatrix conf_matrix;

    install_signal_handlers(argv[0], NULL, NULL);
    systypes_init();

    printf("linktest-utils: OK\n");

    return 0;
}
