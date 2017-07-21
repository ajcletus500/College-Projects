//
// Long-address C-specific support code
//
// Jeff Brown
// $Id: long-addr-c.c,v 1.1.2.1 2009/07/29 10:52:51 jbrown Exp $
//

const char RCSid_1248839785[] = 
"$Id: long-addr-c.c,v 1.1.2.1 2009/07/29 10:52:51 jbrown Exp $";

#include <stdio.h>

#include "sys-types.h"
#include "long-addr.h"


#if !defined(__cplusplus) || defined(MAKE_DEPEND)
    int SizeOfLongAddrFromC = sizeof(LongAddr);
#else
    #error "this variable definition must be compiled as C"
#endif
