// -*- C++ -*-
//
// Key-value tree "value" abstract base type 
//
// Jeff Brown
// $JAB-Id: kv-tree-val.h,v 1.1 2003/01/16 00:18:47 jabrown Exp $
//

#ifndef KV_TREE_VAL_H
#define KV_TREE_VAL_H

class KVTreeVal {
public:
    virtual ~KVTreeVal() { }
    virtual KVTreeVal *copy() const = 0;
};

#endif  /* KV_TREE_VAL_H */
