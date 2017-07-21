// -*- C++ -*-
//
// Loader private declarations, etc
//
// Jeff Brown
// $Id: loader-private.h,v 1.1.2.2.2.1 2008/04/30 22:17:52 jbrown Exp $
//

#ifndef LOADER_PRIVATE_H
#define LOADER_PRIVATE_H

#include <string>

// Defined elsewhere
struct AppState;

class AppExecFileLoader {
public:
    virtual ~AppExecFileLoader() { };

    // Return some sort of diagnostic-meaningful string to identify the
    // actual loader implementation
    virtual std::string name() const = 0;

    // Query a loader: can you load the excutable image from this file?
    virtual bool file_type_match(const char *filename) const = 0;

    // Load an executable image from the named file into the given AppState.
    // Populates dest->prog_mem and dest->seg_info.  (Note: this doesn't
    // create a stack segment, common code in the caller creates that.)
    //
    // Prints to stderr and returns <0 on error.
    virtual int load_file(struct AppState *dest, const char *filename) 
        const = 0;
};


// These return new object instances, or NULL if unimplemented
extern "C" {
AppExecFileLoader *new_exec_loader_aout(void);
AppExecFileLoader *new_exec_loader_elf(void);
}


#ifdef LOADER_PRIVATE_MAIN_MODULE       // Only loader.cc should define this!

// Placing this storage definition here, instead of inside the single source
// file which should #include this with the conditional defined, is kinda
// shady.  The idea is that this array is the only place where the named
// functions are referenced, and it's nice to only have one file where the
// entire list needs to be maintained; however, we want the function
// prototypes themselves visible to the other components, for type checking.

typedef class AppExecFileLoader *(*LoaderCreatorFunc)(void);
static LoaderCreatorFunc LoaderCreators[] = {
    new_exec_loader_elf,
    new_exec_loader_aout
};
#endif // LOADER_PRIVATE_MAIN_MODULE



#endif  /* LOADER_PRIVATE_H */
