// -*- C++ -*-
//
// gzstream, C++ I/O wrapper for zlib-compressed files
// 
// Jeff Brown
// $Id: gzstream.cc,v 1.1.2.2.6.3 2009/07/19 02:15:43 jbrown Exp $
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
// File          : gzstream.C
// Revision      : $Revision: 1.1.2.2.6.3 $
// Revision_date : $Date: 2009/07/19 02:15:43 $
// Author(s)     : Deepak Bandyopadhyay, Lutz Kettner
// 
// Standard streambuf implementation following Nicolai Josuttis, "The 
// Standard C++ Library".
// ============================================================================


#include <assert.h>
#include <errno.h>
#include <stdlib.h>     // for abort
#include <string.h>     // for memmove, strerror

#include <stdexcept>
#include <iostream>
#include <sstream>

#include <zlib.h>

#include "gzstream.h"


// JAB: bring in the bits of "std::" we're using, for convenience.
using std::ios;
using std::istream;
using std::ostream;
using std::ostringstream;
using std::streambuf;
using std::string;


// JAB: decide if this version of zlib offers the gzdirect() function;
// it was added in zlib v1.2.2.3.  Sigh, it's macro time.
#if (ZLIB_VERNUM >= 0x1223)
    #define HAVE_ZLIB_GZDIRECT 1
    #define CALL_ZLIB_GZDIRECT_MAYBE(gzf) (gzdirect(gzf))
#else
    #define HAVE_ZLIB_GZDIRECT 0
    #define CALL_ZLIB_GZDIRECT_MAYBE(gzf) (0)
#endif


namespace gzstream {

// JAB: we've simplified class organization quite a bit, removing the multiple
// inheritance use of gzstreambase (with virtually-inherited std::ios
// instance), in favor of directly embedding gzstreambuf in
// igzstream/ogzstream classes.  This makes control flow much easier to
// follow. (The cost is a handful of lines of duplicated code in those
// classes' open/close/etc. methods.)
//
// (There was a substantial correctness bug where an error in gzopen() could
// be ignored due to the single-instance ios base class having its init()
// method called from the constructors of both children, where the later call
// came after the error was detected, improperly resetting the stream state to
// "good".  The PITA of tracking that down motivated the reorganization.)


class gzstreambuf : public streambuf {
    // JAB: internal constants and knobs
    //
    // Total size of data buffer, including any putback area
    static const int kBufferSize = 256;
    // Max amount of buffer to keep as putback area on an input re-fill
    static const int kPutbackSize = 1;
    // Whether to throw a detailed exception when open method fails; if not,
    // we just silently set "badbit" and return.  (We always throw on
    // gzread/gzwrite errors; in those cases, ios catches the exceptions by
    // default and sets "badbit" for us.)
    static const bool kThrowOnOpenFail = false;
    // Whether to throw when gzclose() fails on an output stream which hasn't
    // already reported an error.
    static const bool kThrowOnCloseWriteFail = false;
    // Whether to allow non-gzipped input data to decompression.  zlib
    // silently passes unrecognized data through, but most other tools
    // (e.g. zcat) require gzip-compressed input.  (If your zlib is too
    // old to offer gzdirect(), this is ignored and effectively "true".)
    static const bool kAllowNonGzipInput = false;

    // JAB: gz_file_ NULL <=> not opened
    gzFile gz_file_;                    // zlib handle for compressed file
    ios::openmode mode_;                // requested I/O mode 
    char buffer_[kBufferSize];          // data buffer
    bool write_failed_;                 // JAB: we've noticed a write failure
    string filename_;                   // JAB: copy of filename (for messages)

    int flush_buffer();

    void throw_for_zlib_error(const char *verbing, int gz_errno,
                              const char *gz_msg_or_null);

    // JAB: make sure we don't copy/assign by accident
    gzstreambuf(const gzstreambuf& src);
    gzstreambuf& operator = (const gzstreambuf &src);
    bool for_read() const { return mode_ & ios::in; }
    bool for_write() const { return mode_ & ios::out; }

public:
    gzstreambuf();
    ~gzstreambuf();
    bool is_open() const { return gz_file_ != NULL; }
    gzstreambuf* open(const char* name, ios::openmode mode,
                      int compress_level);
    gzstreambuf* close();
    
    virtual int overflow(int c = traits_type::eof());
    virtual int underflow();
    virtual int sync();
};


gzstreambuf::gzstreambuf()
    : streambuf(), gz_file_(NULL), write_failed_(false)
{
    // JAB: leave get/put buffer pointers all NULL until open()
}


gzstreambuf::~gzstreambuf()
{
    if (is_open()) {
        // JAB: close() may return fail, or throw an exception, if
        // we're being used for output and the close itself fails.  Since
        // this may result in output data loss, we care.
        bool close_lost_data = false;
        string ios_fail_message;
        try {
            if (!this->close()) {
                close_lost_data = true;
            }
        } catch (const ios::failure& exc) {
            close_lost_data = true;
            ios_fail_message = exc.what();
            // not re-thrown: exceptions cannot safely leave destructors
        }
        if (close_lost_data) {
            // JAB: we just lost data, the caller didn't have a chance to
            // learn about it, and we no longer have any good way to inform
            // them.  Our remaining options are to silently ignore this, or
            // freak the heck out.  We'll chose the latter; if the caller
            // wants to avoid this ugly situation, they'll have to close()
            // things themselves.
            fflush(0);
            fprintf(stderr, "%s (%s:%i): implicit close failed, "
                    "output file compromised; %s\n",
                    "gzstreambuf::~gzstreambuf()", __FILE__, __LINE__,
                    ios_fail_message.c_str());
            fflush(0);
            abort();
        }
    }
}


// (compress_level ignored when reading; inelegant, but at least it's private)
gzstreambuf *
gzstreambuf::open(const char* name, ios::openmode open_mode,
                  int compress_level)
{
    if (is_open())
        return NULL;
    mode_ = open_mode;
    filename_ = name;
    write_failed_ = false;

    // no append nor read/write mode
    if ((mode_ & ios::ate) || (mode_ & ios::app)
        || ((mode_ & ios::in) && (mode_ & ios::out))
        || (!(mode_ & ios::in) && !(mode_ & ios::out))) {
        if (kThrowOnOpenFail) {
            ostringstream msg;
            msg << "gzstreambuf illegal open_mode #" << mode_;
            throw ios::failure(msg.str());
        }
        return NULL;
    }

    char mode_str[10];
    {
        char *fmodeptr = mode_str;
        if (for_read()) {
            *fmodeptr++ = 'r';
        } else {
            assert(for_write());
            assert((compress_level >= -1) && (compress_level <= 9));
            *fmodeptr++ = 'w';
            if (compress_level >= 0) {
                *fmodeptr++ = '0' + compress_level;
            }
        }
        // JAB: we need a 'b' here; zlib passes it through to fopen(3)
        // underneath.  Without it, compressed data may be written in "text
        // mode" on some systems.
        *fmodeptr++ = 'b';
        *fmodeptr = '\0';
    }

    errno = 0;  // JAB: per gzopen API, use errno to detect Z_MEM_ERROR
    gz_file_ = gzopen(name, mode_str);
    if (!gz_file_) {
        if (kThrowOnOpenFail) {
            // JAB: per gzopen API, failures are either "errno" or "out of mem"
            int gz_errno = (errno) ? Z_ERRNO : Z_MEM_ERROR;
            throw_for_zlib_error("opening", gz_errno, NULL);
        }
        return NULL;
    }

    if (!kAllowNonGzipInput && CALL_ZLIB_GZDIRECT_MAYBE(gz_file_)) {
        assert(for_read());
        gzclose(gz_file_);
        gz_file_ = NULL;
        if (kThrowOnOpenFail) {
            ostringstream msg;
            msg << "gzstreambuf error opening \"" << filename_
                << "\": not in gzip format";
            throw ios::failure(msg.str());
        }
        return NULL;
    }

    // initialize buffer pointers
    if (for_read()) {
        this->setg(buffer_,     // beginning of putback (none at first)
                   buffer_,     // read position
                   buffer_);    // end position (buffer is empty)
    } else {
        this->setp(buffer_, buffer_ + kBufferSize);
    }

    return this;
}


void
gzstreambuf::throw_for_zlib_error(const char *verbing, int gz_errno,
                                  const char *gz_msg_or_null)
{
    // JAB: note that gz_msg may be NULL when unavailable.  (In particular,
    // after a failed gzopen() or gzclose().)
    ostringstream msg;
    msg << "zlib error " << verbing << " \"" << filename_ << "\": ";
    if (gz_errno == Z_ERRNO) {
        msg << "system errno " << errno << ": " << strerror(errno);
    } else if (gz_msg_or_null != NULL) {
        msg << "zlib " << gz_msg_or_null;
    } else if (gz_errno == Z_MEM_ERROR) {
        msg << "zlib Z_MEM_ERROR";
    } else {
        msg << "zlib error #" << gz_errno;
    }
    // JAB: I'm not sure if this is the "right" exception to throw here, but
    // we really need to throw something on read errors, since underflow()
    // offers no other way to distinguish between EOF and error.
    throw ios::failure(msg.str());
}


gzstreambuf *
gzstreambuf::close()
{
    if (is_open()) {
        sync();
        // JAB: gzclose() always destroys the gzFile object, and returns any
        // error code.
        int gz_errno = gzclose(gz_file_);
        gz_file_ = NULL;
        // JAB: we'll ignore gzclose() errors for read-only streams; the
        // close operation worked, and if we've read far enough to
        // "see" the error, it was already noticed in underflow().
        if ((gz_errno == Z_OK) || !for_write()) {
            filename_.clear();
            return this;
        }
        // JAB: we may have just lost data.  If we haven't already 
        // detected such a failure, we'd better not let this slide.
        if (!write_failed_) {
            write_failed_ = true;
            if (kThrowOnCloseWriteFail) {
                throw_for_zlib_error("closing", gz_errno, NULL);
            }
        }
    }
    filename_.clear();
    return NULL;
}


int gzstreambuf::underflow()
{
    if (!is_open() || !for_read()) {
        return traits_type::eof();
    }

    // used for input buffer only
    if (gptr() && (gptr() < egptr())) {
        // buffer is not empty (why were we called?)
        return traits_type::to_int_type(*this->gptr());
    }

    // JAB: decide how much of the current putback area [eback...gptr) we want
    // to keep as the future the putback area after the read.
    int putback_to_keep = gptr() - eback();
    if (putback_to_keep > kPutbackSize)
        putback_to_keep = kPutbackSize;

    if (putback_to_keep) {
        // JAB: move desired putback to start of buffer.  This is simple but
        // wasteful for large putback areas, but 1) we don't expect large
        // kPutbackSize, and 2) we can revisit this without changing the API
        // if need be.
        if (kPutbackSize == 1) {
            buffer_[0] = *(gptr() - 1);
        } else {
            memmove(buffer_, gptr() - putback_to_keep, putback_to_keep);
        }
    }

    int read_size = gzread(gz_file_, buffer_ + putback_to_keep,
                           kBufferSize - putback_to_keep);

    // JAB: note that zlib may have detected an error, and also returned us
    // data (read_size > 0).  In that case, we'll buffer the data as if
    // nothing was wrong, and then handle the error at the next underflow().

    if (read_size <= 0) { // ERROR or EOF
        // JAB: despite the published API, we can't rely on a <0 result to
        // signal an error; we've seen a variety of error behavior.
        // zlib-1.2.1.2 failed to detect some truncated files and just
        // returned short reads (like an EOF) with gzerror code 1
        // (Z_STREAM_END), while zlib-1.2.3 set error code -5 (Z_BUF_ERROR).
        //
        // In light of observed behavior, we'll ignore published API and
        // assume that 1) either negative read sizes OR gz_errno < 0 signals
        // errors, and 2) the "silently accepted truncated input" behavior was
        // a zlib bug in the earlier version.
        int gz_errno;
        const char *gz_msg = gzerror(gz_file_, &gz_errno);
        if ((read_size < 0) || (gz_errno < 0)) {
            throw_for_zlib_error("reading", gz_errno, gz_msg);
        }
        return traits_type::eof();
    }

    // reset buffer pointers
    this->setg(buffer_,
               buffer_ + putback_to_keep,
               buffer_ + putback_to_keep + read_size);

    // return next character
    return traits_type::to_int_type(*this->gptr());
}


// JAB: this is an internal routine, not part of std::streambuf
int
gzstreambuf::flush_buffer()
{
    // Separate the writing of the buffer from overflow() and
    // sync() operation.
    int w = pptr() - pbase();
    int written = gzwrite(gz_file_, pbase(), w);
    if (written != w) {
        // JAB: a glance at the zlib source code indicates that gzwrite may
        // return a short count on an error, not just 0 per the API.
        int gz_errno;
        const char *gz_msg = gzerror(gz_file_, &gz_errno);
        write_failed_ = true;
        throw_for_zlib_error("writing", gz_errno, gz_msg);
        return traits_type::eof();
    }
    pbump(-w);
    return w;
}


int 
gzstreambuf::overflow(int c)
{
    // used for output buffer only
    if (!is_open() || !for_write())
        return traits_type::eof();
    if (c != traits_type::eof()) {
        *pptr() = c;
        pbump(1);
    }
    if (flush_buffer() == traits_type::eof())
        return traits_type::eof();
    return c;
}


int
gzstreambuf::sync()
{
    // Changed to use flush_buffer() instead of overflow(EOF)
    // which caused improper behavior with std::endl and flush(),
    // bug reported by Vincent Ricard.
    if (pptr() && pptr() > pbase()) {
        if (flush_buffer() == traits_type::eof())
            return -1;
    }
    // JAB: it's arguable that we should do a gzflush(..., Z_SYNC_FLUSH) here
    // which ends up calling fflush(3), if we're trying to push data out to
    // stable storage; however, doing so can degrade compression.
    return 0;
}



// ----------------------------------------------------------------------------
// Implementation of user classes.
// ----------------------------------------------------------------------------


// JAB: moved even the small method bodies here, instead of the header file,
// to allow for a minimal include footprint and to make debugging easier.
// (These methods should be off the critical-path anyway, since the heavy
// lifting is done through the inherited streambuf/ios methods.)


igzstream::igzstream()
    : istream(), buf_(new gzstreambuf())
{
    this->istream::init(buf_);
}


igzstream::igzstream(const char* name, ios::openmode mode)
    : istream(), buf_(new gzstreambuf())
{
    this->istream::init(buf_);
    this->open(name, mode);
}


igzstream::~igzstream()
{
    delete buf_;
}


bool igzstream::is_open() const
{
    return buf_->is_open();
}


void igzstream::open(const char* name, ios::openmode mode)
{
    if (!buf_->open(name, mode, -1)) {
        buf_->close();          // JAB: in case it was already open
        this->setstate(ios::badbit);
    }
}


void igzstream::close()
{
    if (buf_->is_open()) {
        if (!buf_->close()) {
            this->setstate(ios::badbit);
        }
    }
}


ogzstream::ogzstream()
    : ostream(), buf_(new gzstreambuf()), next_compress_level_(-1)
{
    this->ostream::init(buf_);
}


ogzstream::ogzstream(const char* name, ios::openmode mode)
    : ostream(), buf_(new gzstreambuf()), next_compress_level_(-1)
{
    this->ostream::init(buf_);
    this->open(name, mode);
}


ogzstream::~ogzstream()
{
    delete buf_;
}


bool ogzstream::is_open() const
{
    return buf_->is_open();
}


void ogzstream::open(const char* name, ios::openmode mode)
{
    if (!buf_->open(name, mode, next_compress_level_)) {
        buf_->close();          // JAB: in case it was already open
        this->setstate(ios::badbit);
    }
}


void ogzstream::close()
{
    if (buf_->is_open()) {
        if (!buf_->close()) {
            this->setstate(ios::badbit);
        }
    }
}


void
ogzstream::set_compression_level(int compress_level)
{
    if ((compress_level >= -1) && (compress_level <= 9)) {
        next_compress_level_ = compress_level;
    } else {
        ostringstream msg;
        msg << "gzstreambuf illegal compression level #" << compress_level;
        throw ios::failure(msg.str());
    }
}


}       // namespace gzstream
