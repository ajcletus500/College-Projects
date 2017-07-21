//
// Key-value tree, recursive operations on paths
//
// Jeff Brown
// $JAB-Id: kv-tree-path.cc,v 1.5 2003/02/12 07:06:41 jabrown Exp $
//

const char RCSid_1041383346[] = "$JAB-Id: kv-tree-path.cc,v 1.5 2003/02/12 07:06:41 jabrown Exp $";

#include <assert.h>

#include <string>
#include <vector>

#include "kv-tree-pparse.h"
#include "kv-tree-path.h"


using std::string;
using std::vector;


namespace {
KVTreePath::BadPath badpath_conv(const KVParsedPath::BadPath& ppbp, 
                                 const KVTreePath& tree)
{
    string path = tree.get_parser()->to_str(ppbp.path);
    return KVTreePath::BadPath(path, ppbp.reason);
}
}


KVTreePath::KVTreePath(const KVPathParser& path_parser_)
    : path_parser(0), tree_root(0)
{
    path_parser = new KVPathParser(path_parser_);
    tree_root = new KVTree();
    tree_curr = tree_root;
}


KVTreePath::KVTreePath(const KVPathParser& path_parser_, KVTree *tree_root_)
    : path_parser(0), tree_root(tree_root_)
{
    path_parser = new KVPathParser(path_parser_);
    tree_curr = tree_root;
}


KVTreePath::~KVTreePath()
{
    if (path_parser)
        delete path_parser;
    if (tree_root)
        delete tree_root;
}


KVTreePath *
KVTreePath::copy_top() const
{
    KVTree *tree_copy = new KVTree(*tree_curr);
    KVTreePath *result = new KVTreePath(*path_parser, tree_copy);
    return result;
}


void 
KVTreePath::ts_clear()
{
    tree_curr = tree_root;
    tree_stack.clear();
}


void 
KVTreePath::ts_push(const string& path)
{
    KVPath p_path;
    path_parser->parse(p_path, path);
    KVTree *new_tree = 0;

    try {
        new_tree = KVParsedPath::walk_const(tree_curr, p_path);
    } catch (KVParsedPath::BadPath& bp) {
        throw badpath_conv(bp, *this);
    }
    assert(tree_curr);
    tree_stack.push_back(tree_curr);
    assert(new_tree);
    tree_curr = new_tree;
}


void
KVTreePath::ts_push_top()
{
    assert(tree_curr);
    tree_stack.push_back(tree_curr);
}


void
KVTreePath::ts_pop()
{
    if (!tree_stack.empty()) {
        tree_curr = tree_stack.back();
        assert(tree_curr);
        tree_stack.pop_back();
    } else {
        tree_curr = tree_root;
    }
}


KVTree *
KVTreePath::ts_get(int depth) const
{
    KVTree *result = 0;
    assert(depth >= 0);
    if (depth == 0) {
        result = tree_curr;
    } else if (static_cast<unsigned>(depth - 1) < tree_stack.size()) {
        result = tree_stack[depth - 1];
    }
    return result;
}


void
KVTreePath::walk_root()
{
    tree_curr = tree_root;
}


void
KVTreePath::walk_const(const string& path)
{
    assert(tree_curr);
    KVPath p_path;
    path_parser->parse(p_path, path);
    try {
        KVTree *new_tree = KVParsedPath::walk_const(tree_curr, p_path);
        assert(new_tree);
        tree_curr = new_tree;
    } catch (KVParsedPath::BadPath& bp) {
        throw badpath_conv(bp, *this);
    }
}


void
KVTreePath::walk_create(const string& path)
{
    assert(tree_curr);
    KVPath p_path;
    path_parser->parse(p_path, path);
    try {
        KVTree *new_tree = KVParsedPath::walk_create(tree_curr, p_path);
        assert(new_tree);
        tree_curr = new_tree;
    } catch (KVParsedPath::BadPath& bp) {
        throw badpath_conv(bp, *this);
    }
}


string 
KVTreePath::full_path(const string& rel_path) const
{
    KVPath p_path = KVParsedPath::full_path(tree_curr);
    KVPath p_rel_path;
    path_parser->parse(p_rel_path, rel_path);
    p_path.append(p_rel_path);
    p_path = path_parser->resolve(p_path, 0);
    string result = path_parser->to_str(p_path);
    return result;
}


const KVTreeVal *
KVTreePath::get(const string& path) const
{
    KVPath p_path;
    path_parser->parse(p_path, path);
    const KVTreeVal *val = 0;
    try {
        val = KVParsedPath::get(tree_curr, p_path);
    } catch (KVParsedPath::BadPath& bp) {
        throw badpath_conv(bp, *this);
    }
    return val;
}


const KVTreeVal *
KVTreePath::get_ifexist(const string& path) const
{
    KVPath p_path;
    path_parser->parse(p_path, path);
    const KVTreeVal *val = KVParsedPath::get_ifexist(tree_curr, p_path);
    return val;
}


void 
KVTreePath::set(const string& path, KVTreeVal *new_val, bool weakly)
{
    KVPath p_path;
    path_parser->parse(p_path, path);
    try {
        KVParsedPath::set(tree_curr, p_path, new_val, weakly);
    } catch (KVParsedPath::BadPath& bp) {
        throw badpath_conv(bp, *this);
    }
}


void 
KVTreePath::del(const string& path)
{
    KVPath p_path;
    path_parser->parse(p_path, path);
    try {
        KVParsedPath::del(tree_curr, p_path);
    } catch (KVParsedPath::BadPath& bp) {
        throw badpath_conv(bp, *this);
    }
}


void 
KVTreePath::overlay(const string& tree_path, KVTree *src, bool weakly)
{
    KVPath p_path;
    path_parser->parse(p_path, tree_path);
    try {
        KVParsedPath::overlay(tree_curr, p_path, src, weakly);
    } catch (KVParsedPath::BadPath& bp) {
        throw badpath_conv(bp, *this);
    }
}
