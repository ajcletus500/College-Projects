// -*- C++ -*-
//
// Key-value tree, recursive operations on paths
//
// Jeff Brown
// $JAB-Id: kv-tree-path.h,v 1.5 2003/02/07 20:38:23 jabrown Exp $
//

#ifndef KV_TREE_PATH_H
#define KV_TREE_PATH_H

#include <string>
#include <vector>

class KVTree;
class KVTreeVal;
class KVPathParser;

// KV-tree, with path parsing built-in, recursive ops, and a stack to ease
// working in multiple tree locations at once.
class KVTreePath {
public:
    typedef std::string string;
    typedef std::vector<KVTree *> TreeStack;

    // Exceptions
    struct BadPath {
        string path;
        string reason;
        BadPath(const string& path_, const string& reason_) 
            : path(path_), reason(reason_) { }
    };

private:
    KVPathParser *path_parser;
    KVTree *tree_root;
    KVTree *tree_curr;
    TreeStack tree_stack;

public:
    KVTreePath(const KVPathParser& path_parser_);
    KVTreePath(const KVPathParser& path_parser_, KVTree *tree_root_);
    ~KVTreePath();
    const KVPathParser *get_parser() const { return path_parser; }
    KVTreePath *copy_top() const;

    // Tree-stack manipulation routines
    void ts_clear();
    int ts_size() const { return 1 + tree_stack.size(); }
    void ts_push(const string& path);
    void ts_push_top();
    void ts_pop();
    KVTree *ts_top() const { return tree_curr; }
    KVTree *ts_get(int depth) const;
    KVTree *root() const { return tree_root; };

    void walk_root();
    void walk_const(const string& path);
    void walk_create(const string& path);
    string full_path(const string& rel_path) const;
    const KVTreeVal *get(const string& path) const;
    const KVTreeVal *get_ifexist(const string& path) const;
    void set(const string& path, KVTreeVal *new_val, bool weakly);
    void del(const string& path);
    void overlay(const string& tree_path, KVTree *src, bool weakly);
};


#endif  /* KV_TREE_PATH_H */
