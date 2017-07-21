//
// Hack to dump some sort of stack backtrace
//
// Copyright (c) 2005 Jeff Brown
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// 1. Redistributions of source code must retain the above copyright
//    notice this list of conditions, and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
// 3. Absolutely no warranty of function or purpose is made by the author.
//
// $JAB-Id: stack-trace.h,v 1.2 2005/06/09 08:37:56 jabrown Exp $
//

#ifndef STACK_TRACE_H
#define STACK_TRACE_H

#ifdef __cplusplus
extern "C" {
#endif

// Query whether this hack is actually implemented in the current binary.
// (This is not even close to being portable.)
int stack_trace_implemented(void);

// Print some kind of stack backtrace to the given file handle.  "exe_name"
// must be the path of the executable file.  "full_trace" is a flag requesting
// that a more detailed trace be generated.
int stack_trace_dump(void *c_FILE_out, const char *exe_name, int full_trace);

#ifdef __cplusplus
}
#endif

#endif  /* STACK_TRACE_H */
