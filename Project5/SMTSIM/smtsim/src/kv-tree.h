// -*- C++ -*-
//
// Key-value tree -- a tree-structured collection of key/value pairs, with 
// text keys.
//
// Jeff Brown
// $JAB-Id: kv-tree.h,v 1.2 2005/03/18 07:23:34 jabrown Exp $
//

#ifndef KV_TREE_H
#define KV_TREE_H

#include <string>

#include "kv-tree-val.h"


class KVTreeRep;

// Key-value tree, with primitive methods only, and representation hidden
struct KVTree : public KVTreeVal {
    friend class KVTreeRep;
private:
    KVTreeRep *tree_rep;

public:
    KVTree();
    KVTree(const KVTree& to_copy);
    ~KVTree();

    KVTree *copy() const { return new KVTree(*this); }

    KVTree *get_parent() const;
    KVTreeVal *get_child(const std::string& name) const;
    const std::string *get_child_name(const KVTreeVal *val) const;
    void set_child(const std::string& name, KVTreeVal *new_val, bool weakly);
    bool del_child(const std::string& name);

    void iter_reset();
    bool iter_next(std::string& next_key_ret, KVTreeVal *&next_val_ret);

    struct OverlayConflict {    // Exception: tree topology conflict!
        const KVTree *this_holder, *src_holder;
        const std::string child_name, reason;
        OverlayConflict(const KVTree *t_h, const KVTree *s_h,
                        const std::string& kid, const std::string& why)
            : this_holder(t_h), src_holder(s_h), child_name(kid),
              reason(why) { }
    };
    void overlay(KVTree *src, bool weakly);
};


#endif  /* KV_TREE_H */
