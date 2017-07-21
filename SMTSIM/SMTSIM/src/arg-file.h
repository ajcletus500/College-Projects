//
// Argument file parser: argument files detail how benchmarks should be
// invoked in the simulator, what command line arguments to pass, etc.
// Note that this does not include simulation configuration info (cache size,
// etc.).
//
// Jeff Brown
// $Id: arg-file.h,v 1.1.2.2.2.1 2008/04/30 22:17:44 jbrown Exp $
//

#ifndef ARG_FILE_H
#define ARG_FILE_H

#ifdef __cplusplus
extern "C" {
#endif


// This struct should have -only- the info that was parsed from argument files.

typedef struct ArgFileInfo {
    i64 ff_dist;

    int num_threads;
    char **argv;

    struct {
        char *in, *out;
    } redir_name;
} ArgFileInfo;


ArgFileInfo *argfile_load(const char *filename);
void argfile_destroy(ArgFileInfo *argfile);

void argfile_dump(void *FILE_out, const ArgFileInfo *af, const char *prefix);


#ifdef __cplusplus
}
#endif

#endif  // ARG_FILE_H
