//
// Miscellaneous C++ utility functions used in SMTSIM
//
// Jeff Brown
// $Id: utils-cc.cc,v 1.1.2.4 2009/07/29 10:52:56 jbrown Exp $
//

const char RCSid_1227003972[] = 
"$Id: utils-cc.cc,v 1.1.2.4 2009/07/29 10:52:56 jbrown Exp $";

#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <fstream>
#include <iostream>

#include "sys-types.h"
#include "sim-assert.h"
#include "utils-cc.h"
#include "gzstream.h"
// Try to keep simulation-specific #includes (e.g. main.h, context.h) out of
// here; this module is for mostly stand-alone utility code.


std::istream *
open_istream_auto_decomp(const char *filename_c)
{
    std::string filename(filename_c);
    std::istream *result;
    bool use_gz = (filename.size() > 3) && 
        (filename.substr(filename.size() - 3) == ".gz");

    if (use_gz) {
        result = new gzstream::igzstream(filename.c_str());
    } else {
        result = new std::ifstream(filename.c_str());
    }

    // force a buffer fill, to help check for initial errors
    result->peek();

    if (!(*result)) {
        delete result;
        result = NULL;
    }
    return result;
}


std::ostream *
open_ostream_auto_comp(const char *filename_c)
{
    std::string filename(filename_c);
    std::ostream *result;
    bool use_gz = (filename.size() > 3) && 
        (filename.substr(filename.size() - 3) == ".gz");

    if (use_gz) {
        result = new gzstream::ogzstream(filename.c_str());
    } else {
        result = new std::ofstream(filename.c_str());
    }

    if (!(*result)) {
        delete result;
        result = NULL;
    }
    return result;
}
