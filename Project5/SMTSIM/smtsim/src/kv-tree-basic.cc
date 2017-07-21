// -*- C++ -*-
//
// Some basic key-value tree "value" types
//
// Jeff Brown
// $JAB-Id: kv-tree-basic.cc,v 1.11 2006/05/08 06:50:00 jabrown Exp $
//

const char RCSid_1042666532[] = "$JAB-Id: kv-tree-basic.cc,v 1.11 2006/05/08 06:50:00 jabrown Exp $";

#include <stdlib.h>
#include <string.h>

#include <assert.h>
#include <cctype>
#include <iostream>
#include <sstream>

#include "kv-tree-basic.h"
#include "kv-tree-path.h"
#include "kv-tree-pparse.h"

using namespace KVTreeBasic;
using std::size_t;
using std::istream;
using std::ostream;
using std::istringstream;
using std::ostringstream;
using std::isxdigit;


namespace {
    // These are also hard-coded in the lexer's identifier-handling code
    const string FixedSep = "/"; 
    const string FixedCurr = "."; 
    const string FixedUp = ".."; 
    const string CmtEolStart = "//";
    const string CmtBlockStart = "/*";
    const string CmtBlockEnd = "*/";
}


// Helpers for I/O conversions
namespace {

const char HexTab[] = "0123456789abcdef";

inline void
bytes_to_str(string& dest, const unsigned char *bytes, int n_bytes)
{
    for (int i = 0; i < n_bytes; i++) {
        unsigned val = bytes[i];
        dest += HexTab[val >> 4];
        dest += HexTab[val & 0xf];
    }
}

inline int
char_to_nybble(int ch)
{
    int result;
    if ((ch >= '0') && (ch <= '9'))
        result = ch - '0';
    else if ((ch >= 'a') && (ch <= 'f'))
        result = (ch - 'a') + 10;
    else if ((ch >= 'A') && (ch <= 'F'))
        result = (ch - 'A') + 10;
    else
        result = -1;
    return result;
}

void
char_to_hex(string& dest, int ch)
{
    assert(ch >= 0);
    assert(ch <= 0xff);
    unsigned char val = ch;
    dest += HexTab[val >> 4];
    dest += HexTab[val & 0xf];
}

inline static bool
is_ident_char(int ch) {
    return std::isalnum(ch) || (ch == '_') || (ch == '/') ||
        (ch == '.');
}

// Is "str" a legal identifier "atom"?
// (no additional path components, and nothing which requires quoting)
bool
is_ident_atom(const string& str)
{
    string::const_iterator iter = str.begin(), end = str.end();
    if (str.empty() || (str == "t") || (str == "f"))
        return false;
    if (std::isdigit(*iter))
        return false;           // starts with a digit: no
    for (; iter != end; ++iter) {
        if (!std::isalnum(*iter) && (*iter != '_'))
            return false;
    }
    return true;
}

string
str_escape(const string& str)
{
    string dest;

    size_t len = str.length();
    for (size_t i = 0; i < len; i++) {
        int ch = str[i];
        if ((ch == '"') || (ch == '\\')) {
            dest += "\"" + ch;
        } else if (!std::isprint(ch)) {
            if (ch == '\n') {
                dest += "\\n";
            } else {
                dest += "\\x";
                char_to_hex(dest, ch);
            }
        } else {
            dest += ch;
        }
    }
    return dest;
}

string
str_unescape(const string& str)
{
    string dest;

    size_t len = str.length();
    for (size_t i = 0; i < len; i++) {
        int ch = str[i];
        if (ch == '\\') {
            i++;
            if (i == len)
                break;
            ch = str[i];
            if ((ch == '"') || (ch == '\\')) {
                dest += ch;
            } else if (ch == 'n') {
                dest += "\n";
            } else if ((ch == 'x') && ((i + 2) < len) &&
                       isxdigit(str[i + 1]) && isxdigit(str[i + 2])) {
                int high_nyb = char_to_nybble(str[i + 1]);
                int low_nyb = char_to_nybble(str[i + 2]);
                ch = (high_nyb << 4) | low_nyb;
                dest += ch;
                i += 2;
            }
        } else {
            dest += ch;
        }
    }
    return dest;
}

}


string
Val_String::to_str() const
{
    return "\"" + str_escape(s) + "\"";
}


string
Val_Int::to_str() const
{
    ostringstream output;
    output << i;
    return output.str();
}


string
Val_Bool::to_str() const
{
    return (b) ? "t" : "f";
}


string
Val_Double::to_str() const
{
    ostringstream output;
    output << d;
    // If the string doesn't contain '.', 'e', or 'E', it won't look like a
    // double when we read it back in, so add a trailing '.'
    // (Using the "showpoint" format also adds trailing zeros, which bugs me)
    string result = output.str();
    if (result.find_first_of(".eE") == string::npos)
        result += '.';
    return result;
}


Val_Bin::Val_Bin(const unsigned char *src, size_t byte_count)
    : n_bytes(byte_count), bytes(0)
{
    assert(n_bytes >= 0);
    if (n_bytes > 0) {
        bytes = new unsigned char[n_bytes];
        memcpy(bytes, src, n_bytes);
    }
}


Val_Bin::Val_Bin(bytevec::const_iterator start, bytevec::const_iterator end)
    : n_bytes(end - start), bytes(0)
{
    assert(n_bytes >= 0);
    if (n_bytes > 0) {
        bytes = new unsigned char[n_bytes];
        for (size_t i = 0; i < n_bytes; i++) {
            assert(start != end);
            bytes[i] = *start;
            ++start;
        }
    }
}


Val_Bin::~Val_Bin()
{
    if (bytes)
        delete[] bytes;
}


string
Val_Bin::to_str() const
{
    string dest = "0x";
    bytes_to_str(dest, bytes, n_bytes);
    return dest;
}


KVTreePath *
KVTreeBasic::new_tree()
{
    KVTreePath *result;
    result = new KVTreePath(KVPathParser(FixedSep, FixedUp, FixedCurr));
    return result;
}



namespace {

static const char * const TokenTypeNames[] = {
    "None",
    "Semi", "Comma",
    "Assign", "WeakAssign", "OverlayOp", "WeakOverlayOp",
    "LBrace", "RBrace", "LSqrBracket", "RSqrBracket",
    "Ident", "BasicVal"
};


// Low-overhead "value-only" class; this is passed around and assigned without
// making extra copies of any pointed-at objects; callers must explicitly
// manage storage with yield_*() / free_storage() method calls.
class Token {
public:
    enum Type { 
        TNone,          // "undefined", also used for EOF
        TSemi, TComma,
        TAssign, TWeakAssign, TOverlay, TWeakOverlay,
        TLBrace, TRBrace, TLSqrBracket, TRSqrBracket,
        TIdent, TBasicVal,
        Type_last
    };

private:
    Type type;
    Val *basic_val;
    Val *debug_basic_val;       // Last-held BasicVal, for debug printing
    string ident_val;

    inline static bool type_has_val(Type t) {
        return (t == TIdent) || (t == TBasicVal);
    }

public:
    Token() : type(TNone), basic_val(0), debug_basic_val(0) { }
    ~Token() {
        // Explicitly does not delete basic_val etc. (see above comments)
    }

    void free_storage() {
        // prepare for overwrite
        delete basic_val;
        basic_val = 0;
    }

    Token(Type type_) : type(type_), basic_val(0), debug_basic_val(0)
    { assert(!type_has_val(type)); }

    Token(Type type_, const string& str)
        : type(type_), basic_val(0), debug_basic_val(0), ident_val(str)
    { assert(type == TIdent); }

    Token(Type type_, Val *basic_val_)
        : type(type_), basic_val(basic_val_), debug_basic_val(basic_val_)
    { assert(type == TBasicVal); assert(basic_val != NULL); }

    string debug_str() const;

    bool def() const { return type != TNone; }
    Type get_type() const { return type; }
    Val *yield_basic_val() {
        assert(type == TBasicVal);
        Val *result = basic_val;
        basic_val = 0;
        return result;
    }
    string get_ident() const 
    { assert(type == TIdent); return ident_val; }
};


string
Token::debug_str() const
{
    assert((type >= 0) && (type < Type_last));

    string result("type ");
    result += TokenTypeNames[type];

    if (type_has_val(type))
        result += ", val ";

    switch(type) {
    case TIdent:
        result += "\"" + ident_val + "\"";
        break;
    case TBasicVal:
        // Assumes debug_basic_val remains valid (i.e. that the values being
        // inserted into the tree remain live until the parser returns, as
        // they should.)
        result += ((basic_val) ? basic_val : debug_basic_val)->to_str();
        break;
    default:
        break;
    }

    return result;
}


class Lexer {
    string file_name;
    istream& in_stream;
    size_t line_num, line_offs;
    string line_buff;

    void skip_block_comment();
    bool skip_to_token();
    void append_string_lit(string& dest);
    void append_binary_lit(Val_Bin::bytevec& dest);
    static bool is_ident_prefix(const string& str);
    string read_ident();
    Val *read_numeric_val();
    Val *read_bool_val();
    
public:
    // Exceptions
    struct LexErr {
        string reason;
        LexErr(const string& reason_) : reason(reason_) { }
    };

    Lexer(const string& file_name_, istream& in_stream_) 
        : file_name(file_name_), in_stream(in_stream_), line_num(0),
          line_offs(0)
    { }

    const string& get_fname() const { return file_name; }
    size_t get_lnum() const { return line_num; }

    Token get_token();          // Caller must yield/free storage!
};


void
Lexer::skip_block_comment()
{
    int block_depth = 1;

    while ((block_depth > 0) && in_stream) {
        string::size_type next_start = line_buff.find(CmtBlockStart,
                                                      line_offs);
        string::size_type next_end = line_buff.find(CmtBlockEnd, line_offs);

        // Check for the next block comment start or end on this line
        if ((next_start != string::npos) && 
            ((next_end == string::npos) || (next_end > next_start))) {
            // Block start found, and it precedes any block end
            line_offs = next_start + CmtBlockStart.length();
            block_depth++;
        } else if (next_end != string::npos) {
            assert(next_start != next_end);
            // Block end found, and it precedes any block start
            line_offs = next_end + CmtBlockEnd.length();
            block_depth--;
        } else {
            // No more block starts or ends on this line -- get another
            std::getline(in_stream, line_buff);
            line_num++;
            line_offs = 0;
        }
    }

    if (block_depth > 0)
        throw LexErr("unterminated block comment (note: they nest)");
}


// Skip ahead in the input until the next buffered thing is not either comment
// text or whitespace, or an EOF/error.
bool 
Lexer::skip_to_token()
{
    while (in_stream) {
        while (line_offs < line_buff.length()) {
            if (!std::isspace(line_buff[line_offs])) {
                // Found something that's not whitespace
                if (line_buff.substr(line_offs, CmtEolStart.length()) 
                    == CmtEolStart) {
                    // It's a comment, skip to end of line
                    line_offs = line_buff.length();
                } else if (line_buff.substr(line_offs, CmtBlockStart.length()) 
                    == CmtBlockStart) {
                    // It's a comment block, skip to end of it
                    line_offs += CmtBlockStart.length();
                    skip_block_comment();
                } else {
                    // It's a token!
                    return true;
                }
            } else {
                line_offs++;
            }
        }
        std::getline(in_stream, line_buff);
        line_num++;
        line_offs = 0;
    }

    return false;
}


void
Lexer::append_string_lit(string& dest)
{
    size_t line_len;
    line_len = line_buff.length();

    assert(line_offs < line_len);
    assert(line_buff[line_offs] == '"');

    line_offs += 1;
    string::size_type next_sep = line_buff.find("\"", line_offs);

    if (next_sep == string::npos)
        throw LexErr("newline/EOF in string literal");

    size_t start_offs = line_offs, end_offs = next_sep;
    line_offs = end_offs + 1;

    string lit = line_buff.substr(start_offs, end_offs - start_offs);
    lit = str_unescape(lit);
    dest += lit;
    assert(line_offs <= line_len);
}


void
Lexer::append_binary_lit(Val_Bin::bytevec& dest)
{
    size_t line_len = line_buff.length();
    assert(line_offs <= line_len);

    int digit_num = 0;
    int last_odd_digit = 0;

    while (line_offs < line_len) {
        int ch = line_buff[line_offs];
        if (std::isspace(ch) || std::ispunct(ch))
            break;
        int digit = char_to_nybble(ch);
        if (digit < 0)
            throw LexErr("invalid character in binary(hex) literal");

        if (digit_num & 1) {
            // Odd digit: now we have a pair of nybbles to combine into a byte
            unsigned char byte = (last_odd_digit << 4) | digit;
            dest.push_back(byte);
        } else {
            last_odd_digit = digit;
        }

        digit_num++;
        line_offs++;
    }

    if (digit_num & 1)
        throw LexErr("odd number of hex digits in binary literal");
    assert(line_offs <= line_len);
}


bool 
Lexer::is_ident_prefix(const string& str)
{
    size_t str_len = str.length();

    if (!str_len)
        return false;
    if (isdigit(str[0]) || !is_ident_char(str[0]))
        return false;
    if (str[0] == '.') {
        // "." only starts an identifier if it's "./" or "../"
        if (((str_len < 2) || (str[1] != '/')) &&
            ((str_len < 3) || (str[1] != '.') || (str[2] != '/')))
            return false;
    }

    return true;
}


string
Lexer::read_ident()
{
    size_t line_len = line_buff.length();
    assert(line_offs < line_len);

    string ident;
    while (line_offs < line_len) {
        int ch = line_buff[line_offs];
        if (!is_ident_char(ch))
            break;
        ident.push_back(ch);
        line_offs++;
    }

    assert(line_offs <= line_len);
    return ident;
}


Val *
Lexer::read_numeric_val()
{
    size_t line_len = line_buff.length();
    assert(line_offs < line_len);

    bool is_fp = false;

    // We'll say it's floating-point if the first non-digit we see is
    // '.', 'e', or 'E'
    { 
        size_t scan_offs = line_offs;
        if ((line_buff[scan_offs] == '-') || (line_buff[scan_offs] == '+'))
            scan_offs++;

        while (scan_offs < line_len) {
            int ch = line_buff[scan_offs];
            if (!std::isdigit(ch)) {
                if ((ch == '.') || (ch == 'e') || (ch == 'E'))
                    is_fp = true;
                break;
            }
            scan_offs++;
        }
    }

    istringstream input(line_buff.substr(line_offs));

    Val *result = 0;
    if (is_fp) {
        double d;
        input >> d;
        if (!input)
            throw LexErr("invalid floating-point literal");
        result = new Val_Double(d);
    } else {
        int i;
        input >> i;
        if (!input)
            throw LexErr("invalid integer literal");
        result = new Val_Int(i);
    }

    if (input.eof()) {
        line_offs = line_len;
    } else {
        istream::pos_type chars_used = input.tellg();
        assert(chars_used > 0);
        line_offs += chars_used;
    }

    assert(line_offs <= line_len);
    return result;    
}


Val *
Lexer::read_bool_val()
{
    size_t line_len = line_buff.length();
    assert(line_offs < line_len);

    char first_ch = line_buff[line_offs];
    assert((first_ch == 't') || (first_ch == 'f'));
    bool val = first_ch == 't';
    line_offs++;
    return new Val_Bool(val);
}


Token
Lexer::get_token()
{
    Token result;               // default: type TNone

    if (!skip_to_token())
        return result;
    
    string prefix = line_buff.substr(line_offs, 3);
    while (prefix.length() < 3) {
        prefix += " ";
    }

    size_t used = 0;

    // Check for operators etc. first
    switch (prefix[0]) {
    case ';':
        result = Token(Token::TSemi); 
        used = 1;
        break;
    case ',':
        result = Token(Token::TComma); 
        used = 1;
        break;
    case '=':
        result = Token(Token::TAssign); 
        used = 1;
        break;
    case '?':
        if (prefix[1] == '=') {
            result = Token(Token::TWeakAssign);
            used = 2; 
        } else if ((prefix[1] == '|') && (prefix[2] == '=')) {
            result = Token(Token::TWeakOverlay);
            used = 3;
        }
        break;
    case '|':
        if (prefix[1] == '=') {
            result = Token(Token::TOverlay);
            used = 2; 
        }
        break;
    case '{':
        result = Token(Token::TLBrace);
        used = 1;
        break;
    case '}':
        result = Token(Token::TRBrace);
        used = 1;
        break;
    case '[':
        result = Token(Token::TLSqrBracket);
        used = 1;
        break;
    case ']':
        result = Token(Token::TRSqrBracket);
        used = 1;
        break;
    }

    line_offs += used;

    if (!result.def()) {
        // No tokens matched yet: check for identifiers, values, etc.
        if (prefix[0] == '"') {
            // Concatenate consecutive string literals
            string str;
            do {
                append_string_lit(str);
            } while (skip_to_token() && line_buff[line_offs] == '"');
            result = Token(Token::TBasicVal, new Val_String(str));
        } else if ((prefix[0] == '0') && (prefix[1] == 'x')) {
            line_offs += 2;
            Val_Bin::bytevec bytes;
            do {
                append_binary_lit(bytes);
            } while (skip_to_token() && isxdigit(line_buff[line_offs]));
            result = Token(Token::TBasicVal, new Val_Bin(bytes.begin(),
                                                         bytes.end()));
        } else if (((prefix[0] == 't') || (prefix[0] == 'f')) &&
                   !is_ident_char(prefix[1])) {
            Val *bv = read_bool_val();
            result = Token(Token::TBasicVal, bv);
        } else if (is_ident_prefix(prefix)) {
            string str = read_ident();
            result = Token(Token::TIdent, str);
        } else if (std::isdigit(prefix[0]) || (prefix[0] == '.') ||
                   (prefix[0] == '+') || (prefix[0] == '-')) {
            Val *numeric = read_numeric_val();
            result = Token(Token::TBasicVal, numeric);
        }
    }

    if (!result.def()) {
        throw LexErr("invalid token");
    }

    return result;
}


// -- Grammar; hopefully LL(1) --
//
// (hints: no left-recursion; avoid starting productions with nullable
// non-terminals)
//
// START: A-LIST
// A-LIST: ASSGT A-LIST2   (assignment list)
//       | empty
// A-LIST2: Semi A-LIST    (using seperate rules to allow some missing semis)
//        | empty
// ASSGT: KEY OPTIONAL-VAL
// KEY: Ident
//    | BasicVal  (with additional semantic restrictions on this BasicVal)
// OPTIONAL-VAL: OP VAL
//             | empty
// OP: Assign
//   | WeakAssign
//   | OverlayOp
//   | WeakOverlayOp
// VAL: TREE-VAL
//    | BasicVal
//    | Ident
// TREE-VAL: LBrace A-LIST RBrace
//         | LSqrBracket VAL-LIST RSqrBracket
// VAL-LIST: VAL VAL-LIST2
//         | empty
// VAL-LIST2: Comma VAL-LIST
//          | empty
//
// For the grammar to be LL(1):
//   1) The FIRST sets for alternative productions of a given nonterminal must
//      be disjoint.  (Including "empty", representing nullability; that is,
//      at most one production may start with a nullable nonterminal.)
//   2) If a nonterminal is nullable, its FIRST and FOLLOW sets must be
//      disjoint.
//
// Nullable non-terminals: A-LIST A-LIST2 OPTIONAL-VAL VAL-LIST VAL-LIST2
//
// FIRST(x ...) = { x }, when x is a terminal
// FIRST(A-LIST) = { empty Ident BasicVal }
// FIRST(A-LIST2) = { empty Semi }
// FIRST(ASSGT A-LIST2) = { Ident BasicVal }
// FIRST(KEY OPTIONAL-VAL) = { Ident BasicVal }
// FIRST(OPTIONAL-VAL) = { empty Assign WeakAssign OperlayOp WeakOperlayOp }
// FIRST(OP VAL) = { Assign WeakAssign OperlayOp WeakOperlayOp }
// FIRST(TREE-VAL) = { LBrace LSqrBracket }
// FIRST(VAL VAL-LIST2) = { LBrace LSqrBracket BasicVal Ident }
// FIRST(VAL-LIST) = { empty LBrace LSqrBracket BasicVal Ident }
// FIRST(VAL-LIST2) = { empty Comma }
//
// FOLLOW(A-LIST) = { RBrace }
// FOLLOW(A-LIST2) = { RBrace }
// FOLLOW(VAL-LIST) = { RSqrBracket }
// FOLLOW(VAL-LIST2) = { RSqrBracket }
// FOLLOW(ASSGT) = { Semi RBrace }
// FOLLOW(OPTIONAL-VAL) = { Semi RBrace }


// This is a relatively simple recursive-descent parser
class Parser {
    Lexer& lex;
    Token next_tok;
    KVPathParser& pparser;
    string last_assgt;  // Last name assigned, for debugging

    void read_tok() {
        next_tok.free_storage();
        next_tok = lex.get_token();
    }
    void do_assgt(KVTree *dst, const string& path_name, KVTreeVal *val,
                  bool is_overlay, bool is_weak);
    const KVTreeVal *read_val(const KVTree *src, const string& path_name);
    KVTree *parse_treeval();
    static bool in_firstof_assgt(const Token& tok);
    void parse_alist(KVTree *dst);
    void parse_alist2(KVTree *dst);
    static bool in_firstof_val(const Token& tok);
    void parse_vallist(KVTree *dst, int size_before);
    bool parse_vallist2(KVTree *dst, int size_before);
    void parse_assgt(KVTree *dst);
    static string key_from_val(Val *victim_val);
    string parse_key();
    KVTreeVal *parse_val(KVTree *src);
    KVTreeVal *parse_optional_val(KVTree *src, Token::Type& oper_ret);
    
public:
    // Exceptions
    struct ParseErr {
        string reason;
        ParseErr(const string& reason_) : reason(reason_) { }
    };

    Parser(Lexer& lexer, KVPathParser& pparser_)
        : lex(lexer), next_tok(), pparser(pparser_),
          last_assgt("(none)")
    { 
    }

    ~Parser() { next_tok.free_storage(); }

    KVTree *parse();
    const string& get_last_assgt() const { return last_assgt; }
    const string get_next_tok_debug() const { return next_tok.debug_str(); }
};


void 
Parser::do_assgt(KVTree *dst, const string& path_name, KVTreeVal *val, 
                 bool is_overlay, bool is_weak)
{
    KVPath path;
    pparser.parse(path, path_name);

    try {
        if (is_overlay) {
            if (KVTree *val_tree = dynamic_cast<KVTree *>(val)) {
                KVParsedPath::overlay(dst, path, val_tree, is_weak);
            } else {
                delete val;
                throw ParseErr("non-treeval RHS in overlay");
            }
        } else {
            KVParsedPath::set(dst, path, val, is_weak);
        }
    } catch (KVParsedPath::BadPath& bp) {
        // KVParsedPath::set() deletes the value on failure
        throw ParseErr("bad path as lvalue in " +
                       string((is_overlay) ? "overlay" : "assignment") +
                       ": " + bp.to_str(pparser));
    }

    last_assgt = path_name;
}


// Returns pointer to val linked in tree: don't hurt it
const KVTreeVal *
Parser::read_val(const KVTree *src, const string& path_name)
{
    const KVTreeVal *result;
    KVPath path;
    pparser.parse(path, path_name);

    try {
        result = KVParsedPath::get(src, path);
    } catch (KVParsedPath::BadPath& bp) {
        throw ParseErr("bad path as rvalue: " + bp.to_str(pparser));
    }
    return result;
}


KVTree *
Parser::parse_treeval()
{
    KVTree *t = new KVTree;
    if (next_tok.get_type() == Token::TLBrace) {
        read_tok();
        parse_alist(t);
        if (next_tok.get_type() != Token::TRBrace) 
            throw ParseErr("missing '}' in tree value");
        read_tok();
    } else if (next_tok.get_type() == Token::TLSqrBracket) {
        read_tok();
        parse_vallist(t, 0);
        if (next_tok.get_type() != Token::TRSqrBracket) 
            throw ParseErr("missing ']' in sequence value");
        read_tok();
    } else {
        delete t;
        throw ParseErr("missing '{' / '[' where tree value expected");
    }

    return t;
}


bool
Parser::in_firstof_assgt(const Token& tok)
{
    bool result;
    switch (tok.get_type()) {
    case Token::TIdent:
    case Token::TBasicVal:
        result = true; break;
    default:
        result = false;
    }
    return result;
}


void 
Parser::parse_alist(KVTree *dst)
{
    if (in_firstof_assgt(next_tok)) {
        parse_assgt(dst);
        parse_alist2(dst);
    }
}


void 
Parser::parse_alist2(KVTree *dst)
{
    if (next_tok.get_type() == Token::TSemi) {
        read_tok();
        parse_alist(dst);
    } else if (next_tok.def() && (next_tok.get_type() != Token::TRBrace)) {
        // This isn't strictly part of the grammar; an ASSGT in an A-LIST
        // expansion is followed by something which isn't in FOLLOW(ASSGT).
        // The grammar dictates we just match this A-LIST2 as an empty string.
        // However, this particular case never occurs for valid input; the
        // likely cause is a missing semicolon between to assignments.  Rather
        // then stop matching A-LIST and throw an error from parse_treeval(),
        // we'll throw a specific error here since we have the info.
        throw ParseErr("missing semicolon after assignment");
    }
}


bool
Parser::in_firstof_val(const Token& tok)
{
    bool result;
    switch (tok.get_type()) {
    case Token::TIdent:
    case Token::TBasicVal:
    case Token::TLBrace:
    case Token::TLSqrBracket:
        result = true; break;
    default:
        result = false;
    }
    return result;
}


void
Parser::parse_vallist(KVTree *dst, int size_before)
{
    bool final_val_assigned = false;    // flag: just discovered end-of-list
    if (in_firstof_val(next_tok)) {     // Match: VAL VAL-LIST2
        KVTreeVal *val = parse_val(dst);
        ostringstream key;
        key << "_" << size_before;
        dst->set_child(key.str(), val, false);
        size_before++;
        if (parse_vallist2(dst, size_before)) {
            // VAL-LIST2 matched null-string: just saw end of list
            final_val_assigned = true;
        }
    } else {                            // Match: empty
        final_val_assigned = true;
    }
    if (final_val_assigned) {
        string size_key("size");
        assert(dst->get_child(size_key) == NULL);
        assert(size_before >= 0);
        dst->set_child(size_key, new Val_Int(size_before), false);
    }
}


// true: this VAL-LIST2 expanded to "empty"
bool
Parser::parse_vallist2(KVTree *dst, int size_before)
{
    bool matched_empty = true;
    if (next_tok.get_type() == Token::TComma) { // Match: Comma VAL-LIST
        read_tok();
        matched_empty = false;
        parse_vallist(dst, size_before);
    } else if (next_tok.def() &&
               (next_tok.get_type() != Token::TRSqrBracket)) {
        // Not strictly part of the grammar; an VAL in a VAL-LIST is followed
        // by something which caused VAL-LIST2 to expand to "empty", but is
        // also not in FOLLOW(VAL-LIST2).  Rather than blindly not matching
        // it, we'll throw a specific error to be helpful.  (See analagous
        // case in parse_alist2()).
        throw ParseErr("missing comma in value list");
    }
    return matched_empty;
}


void 
Parser::parse_assgt(KVTree *dst)
{
    string key = parse_key();
    Token::Type oper = Token::TNone;
    KVTreeVal *val = parse_optional_val(dst, oper);

    bool is_overlay = false, is_weak = false;
    if (val) {
        switch (oper) {
        case Token::TAssign:
            break;
        case Token::TWeakAssign:
            is_weak = true;
            break;
        case Token::TOverlay:
            is_overlay = true;
            break;
        case Token::TWeakOverlay:
            is_weak = is_overlay = true;
            break;
        default:
            throw ParseErr("'impossible' unmatched operator in parse_assgt");
        }
    } else {
        // No value specificied, use a default value: boolean true
        val = new Val_Bool(true);
    }
    do_assgt(dst, key, val, is_overlay, is_weak);
}


// Always destroys "victim_val"
string
Parser::key_from_val(Val *victim_val)
{
    string key;
    Val_String *val_str;
    if ((val_str = dynamic_cast<Val_String *>(victim_val))) {
        key = val_str->val();
        delete victim_val;
    } else {
        string msg_str(victim_val->to_str());
        delete victim_val;
        throw ParseErr("non-identifier, non-string key \"" +
                       msg_str + "\" in assignment");
    }
    // Reject string keys which would muck up path parsing
    if ((key == FixedCurr) || (key == FixedUp) || key.empty()) {
        throw ParseErr("illegal key \"" + key + "\" matches reserved value");
    }
    return key;
}


string
Parser::parse_key()
{
    string key;
    if (next_tok.get_type() == Token::TIdent) {
        key = next_tok.get_ident();
        read_tok();
    } else if (next_tok.get_type() == Token::TBasicVal) {
        Val *val = next_tok.yield_basic_val();
        read_tok();
        key = key_from_val(val);        // deletes val
    } else {
        throw ParseErr("missing key in assignment");
    }
    return key;
}


KVTreeVal *
Parser::parse_val(KVTree *src)
{
    KVTreeVal *result;

    if ((next_tok.get_type() == Token::TLBrace) ||
        (next_tok.get_type() == Token::TLSqrBracket)) {
        result = parse_treeval();
    } else if (next_tok.get_type() == Token::TBasicVal) {
        result = next_tok.yield_basic_val();
        read_tok();
    } else if (next_tok.get_type() == Token::TIdent) {
        string path = next_tok.get_ident();
        result = read_val(src, path)->copy();
        read_tok();
    } else {
        result = 0;
        throw ParseErr("missing value");
    }

    return result;
}


// Also parses "OP".  Returns NULL for no value, otherwise returns 
// the parsed value, and writes the parsed operator to oper_ret.
KVTreeVal *
Parser::parse_optional_val(KVTree *src, Token::Type& oper_ret)
{
    switch (next_tok.get_type()) {
    case Token::TAssign:
    case Token::TWeakAssign:
    case Token::TOverlay:
    case Token::TWeakOverlay:
        oper_ret = next_tok.get_type();
        read_tok();
        break;
    default:
        // "OP" unmatched: no value
        return NULL;
    }

    return parse_val(src);
}


KVTree *
Parser::parse()
{
    KVTree *result = new KVTree();
    read_tok();

    try {
        parse_alist(result);
        if (next_tok.def())
            throw ParseErr("unmatched token at top-level: " +
                           next_tok.debug_str());
    } catch (...) {
        delete result;
        throw;
    }
    return result;
}


}


void
KVTreeBasic::test_lexer(const string& src_name, istream& src_stream)
{
    Lexer lex(src_name, src_stream);

    try {
        Token tok;
        while ((tok = lex.get_token()).def()) {
            std::cout << tok.debug_str() << "\n";
            tok.free_storage();
        }
    } catch (Lexer::LexErr& ex) {
        std::cout << lex.get_fname() << ":" << lex.get_lnum() << ": " 
                  << ex.reason << "\n";
    }
}


KVTreePath *
KVTreeBasic::read_tree(const string& src_name, std::istream& src_stream)
{
    Lexer lex(src_name, src_stream);
    KVPathParser p_parser(FixedSep, FixedUp, FixedCurr);
    Parser parser(lex, p_parser);

    KVTree *result_tree = 0;
    try {
        result_tree = parser.parse();
    } catch (Parser::ParseErr& ex) {
        result_tree = 0;
        ostringstream err;
        err << lex.get_fname() << ":" << lex.get_lnum() << ": " 
            << ex.reason << ".  Last assigned \""
            << str_escape(parser.get_last_assgt())
            << "\", next token: " << parser.get_next_tok_debug();
        throw BadParse(err.str());
    } catch (Lexer::LexErr& ex) {
        result_tree = 0;
        ostringstream err;
        err << lex.get_fname() << ":" << lex.get_lnum() << ": " 
            << ex.reason << ".  Last assigned \""
            << str_escape(parser.get_last_assgt())
            << "\", last token: " << parser.get_next_tok_debug();
        throw BadParse(err.str());
    }

    KVTreePath *result = 0;
    if (result_tree) {
        result = new KVTreePath(p_parser, result_tree);
    }

    return result;
}


namespace {

// Transform the text of a key for output, such that it can be read back
// in later.
string fmt_key_out(const string& key)
{
    string result;
    if (false && ((key == "t") || (key == "f"))) {
        // Hack for older grammar, to handle "t" and "f" as keys
        result = "./" + key;
    } else if ((key == FixedCurr) || (key == FixedUp) || key.empty()) {
        std::cerr << __FILE__ << ":" << __LINE__
                  << ": 'impossible' illegal key \"" << key << "\"\n";
        abort();
    } else if (is_ident_atom(key)) {
        result = key;
    } else {
        result = "\"" + str_escape(key) + "\"";
    }
    return result;
}

bool write_tree_alist(std::ostream& out, KVTree *t, const string& indent_step,
                      const string& indent)
{
    t->iter_reset();
    
    string name;
    KVTreeVal *val;

    while (t->iter_next(name, val)) {
        string out_name(fmt_key_out(name));
        if (KVTree *val_tree = dynamic_cast<KVTree *>(val)) {
            out << indent << out_name << " = {\n";
            if (!write_tree_alist(out, val_tree, indent_step, 
                                  indent + indent_step))
                return false;
            out << indent << "};\n";
        } else if (Val *val_bval = dynamic_cast<Val *>(val)) {
            out << indent << out_name << " = " << val_bval->to_str() << ";\n";
        } else {
            std::cerr << __FILE__ << ":" << __LINE__ << ": value for \""
                      << name << "\" is not a KVTree or a basic value!\n";
            abort();
        }
    }
    return true;
}

}


bool 
KVTreeBasic::write_tree(std::ostream& out, KVTree *tree)
{
    return write_tree_alist(out, tree, "  ", "");
}


bool 
KVTreeBasic::write_tree(std::ostream& out, KVTreePath *tree)
{
    return write_tree(out, tree->root());
}

