// -*- C++ -*-
//
// gzstream, C++ I/O wrapper for zlib-compressed files
// 
// Jeff Brown
// $Id: gzstream.h,v 1.1.2.3.6.2 2009/04/30 03:45:37 jbrown Exp $
//
// Based on v1.5 of http://www.cs.unc.edu/Research/compgeom/gzstream/ ,
// downloaded with SHA1 sum 3cdd797c7e5a10408eb664aca9666525fe00652a.
// See below for original license info.
//

// ============================================================================
// gzstream, C++ iostream classes wrapping the zlib compression library.
// Copyright (C) 2001  Deepak Bandyopadhyay, Lutz Kettner
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
// ============================================================================
//
// File          : gzstream.h
// Revision      : $Revision: 1.1.2.3.6.2 $
// Revision_date : $Date: 2009/04/30 03:45:37 $
// Author(s)     : Deepak Bandyopadhyay, Lutz Kettner
// 
// Standard streambuf implementation following Nicolai Josuttis, "The 
// Standard C++ Library".
// ============================================================================

#ifndef GZSTREAM_H
#define GZSTREAM_H 1

#include <iosfwd>

namespace gzstream {

// JAB: moved gzstreambuf to private implementation file, in part to
// minimize the #include footprint.  (Otherwise, consumers end up pulling in
// zlib.h, which adds many symbols, just for the declaration of the "gzFile"
// pointer type.)
class gzstreambuf;


// ----------------------------------------------------------------------------
// User classes. Use igzstream and ogzstream analogously to ifstream and
// ofstream respectively. They read and write files based on the gz* 
// function interface of the zlib. Files are compatible with gzip compression.
// ----------------------------------------------------------------------------

// JAB: we could make {i,o}gzstream inherit from ifstream/ofstream instead of
// istream/ostream; I'm not enough of a C++ standard library philosophy whiz
// to know which is more appropriate.  (ifstream/ofstream are much more than
// just interfaces: inheriting from them brings in a bunch of explicit
// implementation, filebufs/etc.)  At any rate, we're aping the public
// interface of ifstream/ofstream.

class igzstream : public std::istream {
    gzstreambuf *buf_;                  // owned (ideally a scoped_ptr<>)
public:
    igzstream();
    explicit igzstream(const char *name,
                       std::ios::openmode mode = std::ios::in);
    ~igzstream();
    bool is_open() const;
    void open(const char* name, std::ios::openmode mode = std::ios::in);
    void close();
};


class ogzstream : public std::ostream {
    gzstreambuf *buf_;                  // owned (ideally a scoped_ptr<>)
    int next_compress_level_;           // for next open; 0-9 (-1: zlib def.)
public:
    ogzstream();
    explicit ogzstream(const char *name,
                       std::ios::openmode mode = std::ios::out);
    ~ogzstream();
    bool is_open() const;
    void open(const char *name, std::ios::openmode mode = std::ios::out);
    void close();

    // used at next open(); legal values are 0-9, or -1 for zlib default
    void set_compression_level(int compress_level);
};


} // namespace gzstream


#endif  // GZSTREAM_H

