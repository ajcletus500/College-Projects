//
// Loader: loads programs and sets up memory maps.
//
// Jeff Brown
// $Id: loader.h,v 1.1.2.3 2006/09/22 22:11:57 jbrown Exp $
//

#ifndef LOADER_H
#define LOADER_H

#ifdef __cplusplus
extern "C" {
#endif

// Defined elsewhere
struct AppState;

int loader_read_auto(AppState *dest, const char *filename);

int loader_share_memory(struct AppState *dest, struct AppState *src);

void loader_init_main_entry(struct AppState *astate, int argc,
    char * const *argv, char * const *env);
void loader_init_shared_entry(struct AppState *astate, struct AppState *src,
    int argc, char * const *argv, char * const *env);

#ifdef __cplusplus
}
#endif


#endif  /* LOADER_H */
