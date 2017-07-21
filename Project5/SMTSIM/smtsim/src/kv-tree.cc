//
// Key-value tree -- a tree-structured collection of key/value pairs, with 
// text keys.
//
// Jeff Brown
// $JAB-Id: kv-tree.cc,v 1.6 2005/03/18 07:23:34 jabrown Exp $
//

const char RCSid_1039559241[] = "$JAB-Id: kv-tree.cc,v 1.6 2005/03/18 07:23:34 jabrown Exp $";

#include <assert.h>
#include <stdlib.h>
#include <map>
#include <string>
#include <vector>

#include "kv-tree.h"


using std::make_pair;
using std::string;
using std::vector;


typedef std::map<string, KVTreeVal *> KVNameMap;


// Representation of KVTree nodes, with primitive operations
class KVTreeRep {
    KVTree *parent;
    KVNameMap children;
    KVNameMap::const_iterator child_iter, child_iter_end;

public:
    KVTreeRep() : parent(0) { }
    KVTreeRep(KVTree *holder, const KVTreeRep& to_copy);
    ~KVTreeRep();

    KVTree *get_parent() const { return parent; }
    KVTreeVal *get_child(const string& name) const;
    const string *get_child_name(const KVTreeVal *val) const;
    void set_child(KVTree *holder, const string& name, KVTreeVal *new_val,
                   bool weakly);
    bool del_child(const string& name);

    void iter_reset();
    bool iter_next(string& next_key_ret, KVTreeVal *&next_val_ret);

    void overlay(KVTree *holder, bool do_changes, KVTree *src, bool weakly);
};


KVTreeRep::KVTreeRep(KVTree* holder, const KVTreeRep& to_copy)
    : parent(0)
{
    KVNameMap::const_iterator children_end = to_copy.children.end();

    for (KVNameMap::const_iterator child = to_copy.children.begin();
         child != children_end; ++child) {
        KVTreeVal *new_val = child->second->copy();
        children.insert(make_pair(child->first, new_val));
        if (KVTree *val_tree = dynamic_cast<KVTree *>(new_val)) {
            val_tree->tree_rep->parent = holder;
        }
    }
}


KVTreeRep::~KVTreeRep()
{
    for (KVNameMap::iterator iter = children.begin(); iter != children.end();
         ++iter) {
        delete iter->second;
    }
}


KVTreeVal *
KVTreeRep::get_child(const string& name) const
{
    KVNameMap::const_iterator search = children.find(name);
    KVTreeVal *result = (search != children.end()) ? search->second : 0;
    return result;
}


const string *
KVTreeRep::get_child_name(const KVTreeVal *val) const
{
    const string *result = 0;
    KVNameMap::const_iterator child_end = children.end();
    for (KVNameMap::const_iterator child = children.begin();
         child != child_end; ++child) {
        if (child->second == val) {
            result = &(child->first);
            break;
        }
    }
    return result;
}


void 
KVTreeRep::set_child(KVTree *holder, const string& name, KVTreeVal *new_val,
                     bool weakly)
{
    KVNameMap::iterator search = children.find(name);
    if (search != children.end()) {
        if (weakly) {
            delete new_val;
            new_val = 0;
        } else {
            KVTreeVal *victim = search->second;
            search->second = new_val;
            delete victim;
        }
    } else {
        children.insert(make_pair(name, new_val));
    }

    // new_val will be NULL if already deleted
    if (KVTree *val_tree = dynamic_cast<KVTree *>(new_val)) {
        val_tree->tree_rep->parent = holder;
    }
}


bool 
KVTreeRep::del_child(const string& name)
{
    bool success;
    KVNameMap::iterator search = children.find(name);
    if (search != children.end()) {
        KVTreeVal *victim = search->second;
        children.erase(search);
        delete victim;
        success = true;
    } else {
        success = false;
    }
    return success;
}


void
KVTreeRep::iter_reset() 
{
    child_iter = children.begin(); 
    child_iter_end = children.end();
}


bool 
KVTreeRep::iter_next(string& next_key_ret, KVTreeVal *&next_val_ret)
{
    bool valid = (child_iter != child_iter_end);
    if (valid) {
        next_key_ret = child_iter->first;
        next_val_ret = child_iter->second;
        child_iter++;
    }
    return valid;
}


void 
KVTreeRep::overlay(KVTree *holder, bool do_changes, KVTree *src, bool weakly)
{
    KVNameMap::const_iterator src_iter = src->tree_rep->children.begin(), 
        src_end = src->tree_rep->children.end();
    for (; src_iter != src_end; ++src_iter) {
        const string& name = src_iter->first;
        KVTreeVal *new_val = src_iter->second;

        KVNameMap::iterator dst_find = children.find(name);
        if (dst_find != children.end()) {
            // Incoming name exists in old tree
            KVTreeVal *old_val = dst_find->second;
            KVTree *old_tree = dynamic_cast<KVTree *>(old_val),
                *new_tree = dynamic_cast<KVTree *>(new_val);

            if (new_tree) {
                // Incoming value is a tree
                if (!old_tree) {
                    assert(!do_changes);
                    throw KVTree::OverlayConflict(
                        holder, src, name, "can't overlay tree onto non-tree");
                }
                // Recurse into children
                old_tree->tree_rep->overlay(old_tree, do_changes,
                                            new_tree, weakly);
                if (do_changes)
                    delete new_val;
            } else {
                // Incoming value is a leaf
                if (old_tree) {
                    // Trying to overlay a non-tree value on a tree value; fail
                    assert(!do_changes);
                    throw KVTree::OverlayConflict(
                        holder, src, name, "can't overlay non-tree onto tree");
                }
                if (do_changes) {
                    if (weakly) {
                        delete new_val;
                    } else {
                        dst_find->second = new_val;
                        delete old_val;
                    }
                }
            }
        } else {
            // Incoming name doesn't exist in tree
            if (do_changes) {
                children.insert(make_pair(name, new_val));
                if (KVTree *val_tree = dynamic_cast<KVTree *>(new_val)) {
                    val_tree->tree_rep->parent = holder;
                }
            }
        }
    }
    // Caller will delete "src"; we've already disposed of all the values
    // linked from src->tree_rep->children (either transfering them to "this"
    // or deleteing them), so we'll clear that map to keep src's destructor
    // from monkeying with them.
    if (do_changes)
        src->tree_rep->children.clear();
}


KVTree::KVTree()
    : tree_rep(0)
{
    tree_rep = new KVTreeRep();
}


KVTree::KVTree(const KVTree& to_copy)
    : tree_rep(0)
{
    tree_rep = new KVTreeRep(this, *to_copy.tree_rep);
}


KVTree::~KVTree()
{
    delete tree_rep;
}


KVTree *
KVTree::get_parent() const
{
    return tree_rep->get_parent();
}


KVTreeVal *
KVTree::get_child(const std::string& name) const
{
    return tree_rep->get_child(name);
}


const std::string *
KVTree::get_child_name(const KVTreeVal *val) const
{
    return tree_rep->get_child_name(val);
}


void 
KVTree::set_child(const std::string& name, KVTreeVal *new_val, bool weakly)
{
    tree_rep->set_child(this, name, new_val, weakly);
}


bool 
KVTree::del_child(const std::string& name)
{
    return tree_rep->del_child(name);
}


void 
KVTree::iter_reset()
{
    tree_rep->iter_reset();
}


bool 
KVTree::iter_next(std::string& next_key_ret, KVTreeVal *&next_val_ret)
{
    return tree_rep->iter_next(next_key_ret, next_val_ret);
}


void 
KVTree::overlay(KVTree *src, bool weakly)
{
    // Check that the structures match w/o any changes, then perform changes
    // in a second pass
    tree_rep->overlay(this, false, src, weakly);
    try {
        tree_rep->overlay(this, true, src, weakly);
        delete src;
    } catch (OverlayConflict& conf) {
        // If the first-pass check succeeded, this shouldn't be possible!
        abort();
    }
}
