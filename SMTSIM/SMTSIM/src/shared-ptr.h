//
// Pared-down, standalone copy of Boost's shared_ptr<>.  Atomicity stuff has
// been removed.  (This is notably heavier than scoped_ptr<>, which you should
// prefer.)
//
// Jeff Brown
// $Id: shared-ptr.h,v 1.1.2.2 2009/07/19 02:15:45 jbrown Exp $
//

#ifndef SHARED_PTR_H
#define SHARED_PTR_H

#ifdef __cplusplus

// Derived from boost-1.32.0-7, boost/shared_ptr.hpp,
// boost/detail/shared_ptr_nmt.hpp

//  (C) Copyright Greg Colvin and Beman Dawes 1998, 1999.
//  Copyright (c) 2001, 2002, 2003 Peter Dimov
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
//  See http://www.boost.org/libs/smart_ptr/shared_ptr.htm for documentation.
//


//
//  shared_ptr
//
//  An enhanced relative of scoped_ptr with reference counted copy semantics.
//  The object pointed to is deleted when the last shared_ptr pointing to it
//  is destroyed or reset.
//


template<class T> class shared_ptr
{
private:

    // changed: ditched pointer to atomic counter for plain-old int
    //typedef detail::atomic_count count_type;
    typedef int count_type;

public:

    typedef T element_type;
    typedef T value_type;

    explicit shared_ptr(T * p = 0): px(p) {
        try {  // prevent leak if new throws
            pn = new count_type(1);
        } catch(...) {
            delete p;
            throw;
        }
    }

    ~shared_ptr() {
        if(--*pn == 0) {
            delete px;
            delete pn;
        }
    }

    shared_ptr(shared_ptr const & r): px(r.px)  // never throws
    {
        pn = r.pn;
        ++*pn;
    }

    shared_ptr & operator=(shared_ptr const & r)
    {
        shared_ptr(r).swap(*this);
        return *this;
    }

    void reset(T * p = 0)
    {
        sim_assert(p == 0 || p != px);
        shared_ptr(p).swap(*this);
    }

    T & operator*() const  // never throws
    {
        sim_assert(px != 0);
        return *px;
    }

    T * operator->() const  // never throws
    {
        sim_assert(px != 0);
        return px;
    }

    T * get() const  // never throws
    {
        return px;
    }

    long use_count() const  // never throws
    {
        return *pn;
    }

    bool unique() const  // never throws
    {
        return *pn == 1;
    }
    
    void swap(shared_ptr<T> & other)  // never throws
    {
        std::swap(px, other.px);
        std::swap(pn, other.pn);
    }

private:

    T * px;            // contained pointer
    count_type *pn;    // ptr to reference counter
};


#ifdef DISABLED_TEMPLATE_ADAPTER_FUNCTIONS
template<class T, class U>
inline bool operator==(shared_ptr<T> const & a, shared_ptr<U> const & b)
{
    return a.get() == b.get();
}

template<class T, class U>
inline bool operator!=(shared_ptr<T> const & a, shared_ptr<U> const & b)
{
    return a.get() != b.get();
}

template<class T>
inline bool operator<(shared_ptr<T> const & a, shared_ptr<T> const & b)
{
    return std::less<T*>()(a.get(), b.get());
}

template<class T> void swap(shared_ptr<T> & a, shared_ptr<T> & b)
{
    a.swap(b);
}

// get_pointer() enables boost::mem_fn to recognize shared_ptr

template<class T> inline T * get_pointer(shared_ptr<T> const & p)
{
    return p.get();
}
#endif  // DISABLED_TEMPLATE_ADAPTER_FUNCTIONS


#endif  // __cplusplus

#endif  // SHARED_PTR_H
