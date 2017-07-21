//
// Pared-down, standalone copy of Boost's scoped_ptr<>
//
// Jeff Brown
// $Id: scoped-ptr.h,v 1.1.2.3 2008/10/24 20:34:33 jbrown Exp $
//

#ifndef SCOPED_PTR_H
#define SCOPED_PTR_H

#ifdef __cplusplus

// Derived from boost-1.32.0-6, boost/scoped_ptr.hpp

//  (C) Copyright Greg Colvin and Beman Dawes 1998, 1999.
//  Copyright (c) 2001, 2002 Peter Dimov
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
//  http://www.boost.org/libs/smart_ptr/scoped_ptr.htm
//

//  scoped_ptr mimics a built-in pointer except that it guarantees deletion
//  of the object pointed to, either on destruction of the scoped_ptr or via
//  an explicit reset(). scoped_ptr is a simple solution for simple needs;
//  use shared_ptr or std::auto_ptr if your needs are more complex.

template<class T> class scoped_ptr
{
private:
    T * ptr;
    scoped_ptr(scoped_ptr const &); // noncopyable
    scoped_ptr & operator=(scoped_ptr const &);
    typedef scoped_ptr<T> this_type;

public:
    typedef T element_type;
    explicit scoped_ptr(T * p = 0): ptr(p) { }
    ~scoped_ptr() { delete ptr; }

    void reset(T * p = 0) {
        sim_assert(p == 0 || p != ptr); // catch self-reset errors
        ptr = p;
    }

    T & operator*() const { sim_assert(ptr != 0); return *ptr; }
    T * operator->() const { sim_assert(ptr != 0); return ptr; }
    T * get() const { return ptr; }

    operator bool () const { return ptr != 0; }
    bool operator! () const { return ptr == 0; }
    void swap(scoped_ptr & b) { 
        T * tmp = b.ptr;
        b.ptr = ptr;
        ptr = tmp;
    }
};


#endif  // __cplusplus

#endif  // SCOPED_PTR_H
