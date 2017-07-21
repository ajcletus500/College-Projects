// -*- C++ -*-
//
// A simpler interface to the KVTree system, with a small, fixed set of types,
// fixed path seperators, and I/O facilities.
//
// Jeff Brown
// $JAB-Id: kv-tree-basic.h,v 1.3 2004/03/04 20:33:34 jabrown Exp $
//

#ifndef KV_TREE_BASIC_H
#define KV_TREE_BASIC_H

#include <iosfwd>
#include <string>
#include <vector>

#include "kv-tree-val.h"


class KVTree;
class KVTreePath;


namespace KVTreeBasic {
    using std::string;

    class Val : public KVTreeVal {
    public:
        virtual string to_str() const = 0;
    };

    class Val_String : public Val {
        string s;
    public:
        Val_String(const string& str) : s(str) { }
        Val_String *copy() const { return new Val_String(s); }
        string to_str() const;
        const string& val() const { return s; }
    };

    class Val_Int : public Val {
        int i;
    public:
        Val_Int(int num) : i(num) { }
        Val_Int *copy() const { return new Val_Int(i); }
        string to_str() const;
        int val() const { return i; }
    };

    class Val_Bool : public Val {
        bool b;
    public:
        Val_Bool(bool b_) : b(b_) { }
        Val_Bool *copy() const { return new Val_Bool(b); }
        string to_str() const;
        bool val() const { return b; }
    };

    class Val_Double : public Val {
        double d;
    public:
        Val_Double(double num) : d(num) { }
        Val_Double *copy() const { return new Val_Double(d); }
        string to_str() const;
        double val() const { return d; }
    };

    class Val_Bin : public Val {
    public:
        typedef std::vector<unsigned char> bytevec;
    private:
        std::size_t n_bytes;
        unsigned char *bytes;
    public:
        Val_Bin(const unsigned char *src, size_t byte_count);
        Val_Bin(bytevec::const_iterator start, bytevec::const_iterator end);
        ~Val_Bin();
        Val_Bin *copy() const { return new Val_Bin(bytes, n_bytes); }
        string to_str() const;
        const unsigned char *val() const { return bytes; }
        size_t size() const { return n_bytes; }
    };


    // Exceptions
    struct BadParse {
        string reason;
        BadParse(const string& reason_) : reason(reason_) { }
    };

    KVTreePath *new_tree();
    KVTreePath *read_tree(const string& src_name, std::istream& src_stream);
    bool write_tree(std::ostream& out, KVTree *tree);
    bool write_tree(std::ostream& out, KVTreePath *tree);

    void test_lexer(const string& src_name, std::istream& src_stream);
}


#endif  /* KV_TREE_BASIC_H */
