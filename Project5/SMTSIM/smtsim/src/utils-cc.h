// -*- C++ -*-
//
// Miscellaneous C++ utility functions used in SMTSIM
//
// Jeff Brown
// $Id: utils-cc.h,v 1.1.2.5 2009/08/03 22:32:33 jbrown Exp $
//

#ifndef UTILS_CC_H
#define UTILS_CC_H

#include <stdexcept>            // for e.g. map_at's exception
#include <iosfwd>

#include "scoped-ptr.h"         // tiny, and we use it a lot


// Macros to fill in the extremely common for-loop control statements when
// iterating over STL containers.  (I got sick of typing these out for the
// millionth time, they do tend to clutter things up visually, and I
// didn't see a simple way to template<> it instead.)
//
// e.g. FOR_ITER(MyStlContainerType, map_of_stuff, iter) { ... }"
#define FOR_ITER(box_type, box_var, iter_var) \
    for (box_type::iterator iter_var = (box_var).begin(); \
         iter_var != (box_var).end(); \
         ++iter_var)
#define FOR_CONST_ITER(box_type, box_var, iter_var) \
    for (box_type::const_iterator iter_var = (box_var).begin(); \
         iter_var != (box_var).end(); \
         ++iter_var)


// Silly little class that, when embedded or inherited from, causes
// compilation of implicit copy constructors and assignment operators to
// fail.
class NoDefaultCopy {
private:
    NoDefaultCopy(const NoDefaultCopy& src);
    NoDefaultCopy& operator = (const NoDefaultCopy &src);
public:
    NoDefaultCopy() { }
};


// intsize(container): returns the number of elements in an STL container,
// as an int.  This is handy when subtracting and comparing sizes.
template <typename StlContainerType>
int intsize(const StlContainerType& cont) {
    return static_cast<int>(cont.size());
}


// pop_front_ret() / pop_back_ret(): these remove and return a COPY of the
// first or last element of an STL container.
template <typename StlContainerType>
typename StlContainerType::value_type
pop_back_ret(StlContainerType& cont) {
    sim_assert(!cont.empty());
    typename StlContainerType::value_type to_ret = cont.back();
    cont.pop_back();
    return to_ret;
}
template <typename StlContainerType>
typename StlContainerType::value_type
pop_front_ret(StlContainerType& cont) {
    sim_assert(!cont.empty());
    typename StlContainerType::value_type to_ret = cont.front();
    cont.pop_front();
    return to_ret;
}


// map_find(), map_at() -- non-modifying lookup functions for STL std::map
// (and proto-standard hash_map).
//
// These use template expansion to infer the necessary iterator and value
// types from the map<> supplied, without any explicit type arguments.
//
// These template functions access elements of STL map/hash_map, but without
// the "create missing elements using default-value constructors and
// assignment" behavior of operator[].  This allows convenient "const"
// operation, and convenient indexing of maps containing value types that lack
// 0-arg constructors or assignment operators.

// map_find(m, k): search map "m" for key "k".  If found, returns a pointer to
// the corresponding value; if not, returns a null pointer.  This is a wrapper
// for map<>::find(), which hides the verbosity of iterators.
template <typename MapType>
typename MapType::mapped_type *
map_find(MapType& holder, const typename MapType::key_type& key) {
    typename MapType::iterator found = holder.find(key);
    return (found != holder.end()) ? &(found->second) : 0;
}
template <typename MapType>
const typename MapType::mapped_type *
map_find(const MapType& holder, const typename MapType::key_type& key) {
    typename MapType::const_iterator found = holder.find(key);
    return (found != holder.end()) ? &(found->second) : 0;
}

// map_at(m, k): search map "m" for key "k".  If found, returns a reference to
// the corresponding value; if not, throws an out_of_range exception.  This 
// is analagous to vector<>::at(), or a non-modifying "m[k]".
template <typename MapType>
typename MapType::mapped_type&
map_at(MapType& holder, const typename MapType::key_type& key) {
    typename MapType::iterator found = holder.find(key);
    if (found == holder.end())
        throw std::out_of_range("map_at(): key not found");
    return found->second;
}
template <typename MapType>
const typename MapType::mapped_type&
map_at(const MapType& holder, const typename MapType::key_type& key) {
    typename MapType::const_iterator found = holder.find(key);
    if (found == holder.end())
        throw std::out_of_range("map_at(): key not found");
    return found->second;
}

// map_at_default(m, k, d): search map "m" for key "k".  If found, returns a
// COPY of the corresponding value (not a pointer); if not, returns a copy of 
// default value "d".
template <typename MapType>
typename MapType::mapped_type
map_at_default(const MapType& holder, 
               const typename MapType::key_type& key,
               const typename MapType::mapped_type& default_val) {
    typename MapType::const_iterator found = holder.find(key);
    return (found != holder.end()) ? found->second : default_val;
}

// map_insert_uniq(m, k, v): insert a COPY of value "v" in map "m" with key
// "k".  If successful, returns a pointer to the inserted value, otherwise
// returns a null pointer (e.g. if "k" was already present).  This is a
// wrapper for map<>::insert() which hides the verbosity of the pair<>s and
// iterators in the inputs and outputs.
template <typename MapType>
typename MapType::mapped_type *
map_insert_uniq(MapType& holder, const typename MapType::key_type& key,
                const typename MapType::mapped_type& value_to_copy) {
    std::pair<typename MapType::iterator, bool> insert_result =
        holder.insert(std::make_pair(key, value_to_copy));
    return (insert_result.second) ? &(insert_result.first->second) : 0;
}

// map_put_uniq(m, k, v): insert a COPY of value "v" in map "m" with key
// "k", returning a reference to the inserted value.  Throws out_of_range if
// the insert fails (e.g. if k was already present).  This is a
// non-overwriting analog of "m[k] = v".
template <typename MapType>
typename MapType::mapped_type&
map_put_uniq(MapType& holder, const typename MapType::key_type& key,
             const typename MapType::mapped_type& value_to_copy) {
    std::pair<typename MapType::iterator, bool> insert_result =
        holder.insert(std::make_pair(key, value_to_copy));
    if (!insert_result.second)
        throw std::out_of_range("map_put_uniq(): key already present");
    return insert_result.first->second;
}

// vec_idx(vec, idx): returns a reference to element "idx" of vector
// "vec", after asserting that the index is in-bounds.  This is an
// efficiency/safety midpoint between the always-checked vector.at() and the
// never-checked vector.operator[].
template <typename VecType>
typename VecType::value_type&
vec_idx(VecType& vec, size_t idx) {
    sim_assert((idx >= 0) && (idx < vec.size()));
    return vec[idx];
}
template <typename VecType>
const typename VecType::value_type&
vec_idx(const VecType& vec, size_t idx) {
    sim_assert((idx >= 0) && (idx < vec.size()));
    return vec[idx];
}


// Helper function to extract the pointer value from a scoped_ptr<>, and
// release its ownership of that pointer.  (Convenient for returning a new
// object as a function result, using a scoped_ptr<> to hold it until
// completion.)  This is separate from the class, so as not to change
// the already widely-accepted scoped_ptr<> API.
template <typename ValType>
ValType *
scoped_ptr_release(scoped_ptr<ValType>& scoped_ptr_obj) {
    ValType *result = scoped_ptr_obj.get();
    scoped_ptr_obj.reset();
    return result;
}


// Generic hash adapter that just uses the given object's stl_hash()
// method, for insertion in quasi-STL hash-based containers.  (aka "go
// hash yourself")
template <typename TypeToHash>
struct StlHashMethod {
    size_t operator() (const TypeToHash& obj) const {
        return obj.stl_hash();
    }
};
// As above, but with a pointer-to the given object type
template <typename TypeToHash>
struct StlHashThroughPtr {
    size_t operator() (const TypeToHash obj_ptr) const {
        return (*obj_ptr).stl_hash();
    }
};


// Comparison functor templates, which compare objects using their built-in
// comparison methods, but through pointer indirection.
template <typename TypeToCmp>
struct CmpLTThroughPtr {
    bool operator() (const TypeToCmp obj1, const TypeToCmp obj2) const {
        return (*obj1) < (*obj2);
    }
};
template <typename TypeToCmp>
struct CmpGTThroughPtr {
    bool operator() (const TypeToCmp obj1, const TypeToCmp obj2) const {
        return (*obj1) > (*obj2);
    }
};
template <typename TypeToCmp>
struct CmpEqThroughPtr {
    bool operator() (const TypeToCmp obj1, const TypeToCmp obj2) const {
        return (*obj1) == (*obj2);
    }
};


// Open the given file and return a new istream object to read from it.  Iff
// the filename ends in ".gz", the file is assumed to be gzipped.  Silently
// returns NULL if open fails.
std::istream *open_istream_auto_decomp(const char *filename_c);

// create the given file and return a new ostream object to write to it,
// truncating the file if it already exists.  Iff the filename ends in ".gz",
// data will be gzipped.  Silently returns NULL if open fails.
std::ostream *open_ostream_auto_comp(const char *filename_c);

#endif  // UTILS_CC_H
