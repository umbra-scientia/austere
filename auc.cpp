/* Copyright © 2020, Mykos Hudson-Crisp <micklionheart@gmail.com>
* All rights reserved. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "austere_h.h"
#include "default_rc.h"

using namespace std;

#define WARNING_STYLE "\x1B[1m\x1B[33m"
#define ERROR_STYLE "\x1B[1m\x1B[31m"
#define HILITE "\x1B[1m"
#define REGGS "\x1B[0m"

#define PLAT_WINDOWS 1
#define PLAT_LINUX 2
#define PLAT_APPLE 3

struct SourceFile;
static map<string, int> global_symbol_flags;
static map<string, string> global_symbol_parent;
static map<string, string> global_symbol_sig;
static string prefix((char*)austere_h, austere_h_len);
static string winrc((char*)default_rc, default_rc_len);
static bool auto_output = false;
static bool utf8 = true;
static string systag;
static string compiler = "cc";
static string cpp_compiler = "c++";
static string asm_compiler = "c++";
static string linker;
static string cs_compiler;
static string resource_compiler;
static string user_auflags, user_cflags, user_cppflags, user_ldflags, user_csflags, user_asmflags;
static string cs_version, asm_fmt="elf64";
static string cflags = "-fvisibility=hidden -fPIC";
static string ldflags = "-fvisibility=hidden -fPIC";
static string cpu_flags = "-march=amdfam10 -mtune=znver1";
static string release_flags = "-fomit-frame-pointer -ffast-math -fopenmp -flto=8 -fgraphite-identity -ftree-loop-distribution -floop-nest-optimize -Ofast -s";
static string debug_flags = "-fstrict-aliasing -ffast-math -fopenmp -flto=8 -g";
static string icon, manifest, details, vendor, product, version, copyright;
static vector<string> libs;
static vector<string> res_files;
static bool cs_use_res = false;
static bool human = false;
static vector<SourceFile*> files;
static vector<string> c_files, rc_files, dll_files, cpp_files, asm_files, rs_files, cs_files;
static string build_dir = "build/";
static string output = "";
#ifdef _WIN32
static string os = "windows";
static string dllext = ".dll";
static string exeext = ".exe";
#else
static string os = "linux";
static string dllext = ".so";
static string exeext;
#endif
static bool debug_mode = false;
static bool dll_mode = false;
static bool quiet = true;

#ifdef _WIN32
#define stat _stat
#endif

static long file_mtime(string filename) {
    struct stat st;
    if (!stat(filename.c_str(), &st)) return st.st_mtime;
    return -1;
}

static bool should_rebuild(string dst, long src_mtime) {
    if (file_mtime(dst) < src_mtime) return true;
    return false;
}

static string line_directive(string in, bool human, int line_no, string filename) {
    if (human) return in;
    string out;
    bool doit = true;
    for(auto c: in) {
        if (doit) {
            char buf[512];
            memset(buf, 0, sizeof(buf));
            snprintf(buf, sizeof(buf)-1, "#line %d \"%s\"\n", line_no, filename.c_str());
            out += buf;
            doit = false;
        }
        out += c;
        if (c == '\n') doit = true;
    }
    return out;
}

static string trim(string line) {
    while (line.size() && isspace(line[line.size()-1])) line = line.substr(0, line.size()-1);
    while (line.size() && isspace(line[0])) line = line.substr(1);
    return line;
}

static string lowercase(string line) {
    string out;
    for(auto c: line) out += tolower(c);
    return out;
}

static string str_replace(string text, string find, string replace) {
    auto pos = text.find(find);
    for(;;) {
        if (pos == string::npos) break;
        text = text.substr(0, pos) + replace + text.substr(pos+find.size());
        pos = text.find(find, pos + replace.size());
    }
    return text;
}

static string chairs(string str, int chairs) {
    if (str.size() <= chairs) return str;
    return str.substr(0, chairs);
}

static string remove_empty_ifdefs(string h) {
    string last_line, line, out;
    bool lastWasIf = false;
    string lastIf;
    for(auto c: h) {
        line += c;
        if (c != '\n') continue;
        if (last_line == "#pragma pack(pop)\n" && line == "#pragma pack(push, 1)\n") {
            line = "";
            last_line = "";
            continue;
        }
        if (chairs(last_line, 3) == "#if" && chairs(line, 5) == "#else") {
            if (chairs(last_line, 6) == "#ifdef") {
                last_line = "#ifndef" + last_line.substr(6);
            } else if (chairs(last_line, 7) == "#ifndef") {
                last_line = "#ifdef" + last_line.substr(7);
            } else {
                last_line = "#if !(" + last_line.substr(3, last_line.size()-4)+")\n";
            }
            line = "";
            continue;
        }
        if (chairs(last_line, 3) == "#if" && chairs(line, 6) == "#endif") {
            line = "";
            last_line = "";
            continue;
        }
        out += last_line;
        last_line = line;
        line = "";
    }
    out += last_line;
    out += line;
    return out;
}

static void rewrite_structs(string& code, string& head, string& public_head, vector<string>& public_csv, string& local_head, string& tail, string space, int* outputToHeader, int isPacked, int isPublic, int isOpaque, int isPrivate, map<string,int>& symbol_flags) {
    int isStruct = (code.find("struct") == 0);
    int isClass = (code.find("class") == 0);
    if (!isStruct && !isClass) return;
    if (code.find('{') == string::npos) return;
    code = code.substr(isStruct ? 6 : 5);
    if (!code.size() || !isspace(code[0])) return;
    while (code.size() && isspace(code[0])) {
        code = code.substr(1);
    }
    string tname;
    while (code.size() && !isspace(code[0])) {
        tname += code[0];
        code = code.substr(1);
    }
    while (code.size() && isspace(code[0])) {
        code = code.substr(1);
    }
    symbol_flags[tname] |= 2;
    if (isPrivate) {
        symbol_flags[tname] |= 16;
        local_head += "typedef struct " + tname + " " + tname + ";\n";
        code = "struct " + tname + " " + code;
        tail = space + "};";
    } else if (isOpaque) {
        *outputToHeader = 3;
        head += "typedef struct " + tname + " " + tname + ";\n";
        if (isPublic) {
            global_symbol_flags[tname] |= 32;
            public_head += "typedef struct " + tname + " " + tname + ";\n";
            public_csv.push_back("\x01unsafe struct "+tname+" {\n");
            public_csv.push_back("\t}\n");
        }
        code = "typedef struct " + tname + " {";
        tail = space + "} " + tname + ";";
    } else if (isPublic) {
        *outputToHeader = 1;
        code = "typedef struct " + tname + " {";
        tail = space + "} " + tname + ";";
    } else {
        *outputToHeader = 2;
        code = "typedef struct " + tname + " {";
        tail = space + "} " + tname + ";";
    }
    if (isPacked) {
        code = "#pragma pack(push, 1)\n" + space + code;
        tail += "\n" + space + "#pragma pack(pop)";
    }
}

static void rewrite_enums(string& code, string& head, string& public_head, vector<string>& public_csv, string& local_head, string& tail, string space, int* outputToHeader, int isPublic, int isOpaque, int isPrivate) {
    int isEnum = (code.find("enum") == 0);
    if (!isEnum) return;
    if (code.find('{') == string::npos) return;
    code = code.substr(4);
    if (!code.size() || !isspace(code[0])) return;
    while (code.size() && isspace(code[0])) {
        code = code.substr(1);
    }
    string tname;
    while (code.size() && !isspace(code[0])) {
        tname += code[0];
        code = code.substr(1);
    }
    while (code.size() && isspace(code[0])) {
        code = code.substr(1);
    }
    if (isPrivate) {
        //head += "typedef int " + tname + ";\n";
        local_head += "typedef int " + tname + ";\n";
    } else if (isOpaque) {
        head += "typedef int " + tname + ";\n";
        public_head += "typedef int " + tname + ";\n";
        global_symbol_flags[tname] |= 64;
        //public_csv.push_back("\tpublic enum "+tname+" {\n");
        *outputToHeader = 3;
    } else if (isPublic) {
        public_csv.push_back("\tpublic enum "+tname+" {\n");
        *outputToHeader = 1;
    } else {
        *outputToHeader = 2;
    }
    code = "typedef enum {";
    tail = space + "} " + tname + ";";
}

static bool write_file(string filename, string data) {
    FILE* fp = fopen(filename.c_str(), "wb");
    if (!fp) return false;
    fwrite(data.data(), 1, data.size(), fp);
    fclose(fp);
    return true;
}

static string read_file(string filename) {
    string out;
    FILE* fp = fopen(filename.c_str(), "rb");
    if (!fp) return out;
    while (!feof(fp)) {
        int c = fgetc(fp);
        if (c == EOF) break;
        out += (char)c;
    }
    fclose(fp);
    return out;
}

static string extract_ext(string fn) {
    auto x = fn.rfind('.');
    if (x == string::npos) return "";
    auto y = fn.find('/', x);
    if (y != string::npos) return "";
    return fn.substr(x+1);
}

static string extract_dir(string fn) {
    auto x = fn.rfind('/');
    if (x == string::npos) x = fn.rfind('\\');
    if (x == string::npos) return "./";
    return fn.substr(0, x+1);
}

static string extract_filename(string fn) {
    auto x = fn.rfind('/');
    if (x == string::npos) x = fn.rfind('\\');
    if (x == string::npos) return fn;
    return fn.substr(x+1);
}

static string trim2(string line, char c) {
    while (line.size() && line[line.size()-1] == c) line = line.substr(0, line.size()-1);
    while (line.size() && line[0] == c) line = line.substr(1);
    return line;
}

static string strip_filename(string fn) {
    auto slash = fn.rfind('/');
    if (slash != string::npos) fn = fn.substr(slash+1);
    slash = fn.rfind('\\');
    if (slash != string::npos) fn = fn.substr(slash+1);
    auto dot = fn.rfind('.');
    if (dot != string::npos) fn = fn.substr(0, dot);
    for(auto& c: fn) {
        if (!isalnum(c)) c = '_';
    }
    return trim2(fn, '_');
}

static string flatten_filename(string fn) {
    auto dot = fn.rfind('.');
    if (dot != string::npos) fn = fn.substr(0, dot);
    for(auto& c: fn) {
        if (!isalnum(c)) c = '_';
    }
    return trim2(fn, '_');
}

static string strip_file_ext(string fn) {
    auto dot = fn.rfind('.');
    if (dot != string::npos) {
        auto slash = fn.find('/', dot);
        if (slash == string::npos) {
            fn = fn.substr(0, dot);
        }
    }
    return fn;
}

static string read_symbol_backwards(string line, int pos) {
    string o;
    pos--;
    while (pos >= 0) {
        char c = line[pos];
        if (!isspace(c)) break;
        pos--;
    }
    while (pos >= 0) {
        char c = line[pos--];
        if (isalnum(c) || c == '_') {
            o = c + o;
            continue;
        }
        break;
    }
    return o;
}

static string read_symbol(string line, int pos) {
    string o;
    while (pos < line.size()) {
        char c = line[pos++];
        if (isalnum(c) || c == '_') {
            o = o + c;
            continue;
        }
        break;
    }
    return o;
}

static string translate_type(string type, int opaqueLevel, bool& unsafed, string parent) {
    string t = trim(type);
    bool isConst = type.find("const ") == 0;
    if (isConst) t = trim(t.substr(5));
    t = str_replace(t, "restrict ", "");
    t = str_replace(t, " restrict", "");
    t = trim(t);
    if (!t.size()) return "void";
    if (opaqueLevel & 2) {
        // translate to C#
        bool ptr = (t[t.size()-1] == '*');
        if (ptr) {
            t = t.substr(0, t.size()-1);
            t = trim(t);
        }
        int flags = global_symbol_flags[t];
        if (flags & 32) {
            unsafed = true;
            if (ptr) {
                t = t + "*";
            }
        } else if (flags & 64) {
            t = "uint";
        } else if (ptr && t == "void") {
            t = "IntPtr";
        } else if (ptr && t == "char") {
            if (isConst && t == "char") {
                //t = "[MarshalAs(UnmanagedType.CustomMarshaler, MarshalTypeRef = typeof(UTF8Marshaler))] string";
                t = "[MarshalAs(UnmanagedType.LPStr)] string";
            } else {
                t = "byte*";
            }
        } else {
                 if (t == "u32") t = "uint";
            else if (t == "i32") t = "int";
            else if (t == "u64") t = "ulong";
            else if (t == "i64") t = "long";
            else if (t == "u16") t = "ushort";
            else if (t == "i16") t = "short";
            else if (t == "u8") t = "byte";
            else if (t == "i8") t = "sbyte";
            else if (t == "f64") t = "double";
            else if (t == "f32") t = "float";
            else if (t == "f16") t = "ushort";
            else if (t == "f16") t = "ushort";
            if (ptr) {
                t = "ref " + t;
            }
        }
    }
    return t;
}

static string extract_entry_point(string sig) {
    auto lpar = sig.find('(');
    if (lpar == string::npos) return "";
    return read_symbol_backwards(sig, lpar);
}

static string translate_delegate(string sig) {
    sig = trim(sig);
    if (!sig.find("typedef ")) {
        sig = trim(sig.substr(7));
    }
    auto opar = sig.find('(');
    if (opar == string::npos) return "";
    auto ast = sig.find('*', opar);
    if (ast == string::npos) return "";
    string func = read_symbol(sig, ast+1);
    auto lpar = sig.find('(', ast);
    if (lpar == string::npos) return "";
    auto rpar = sig.find(')', lpar);
    if (rpar == string::npos) return "";
    string rtype = trim(sig.substr(0, opar));
    vector<string> args;
    string buf = trim(sig.substr(lpar + 1, rpar - lpar - 1));
    while (buf.size()) {
        auto comma = buf.find(',');
        if (comma == string::npos) break;
        args.push_back(trim(buf.substr(0, comma)));
        buf = trim(buf.substr(comma+1));
    }
    if (buf.size()) args.push_back(buf);
    bool unsafed = false;
    string rsig = translate_type(rtype, 2, unsafed, "") + " " + func + "(";
    for(int i=0;i<args.size();i++) {
        string arg = args[i];
        auto sp = arg.rfind(' ');
        string aname = (sp == string::npos) ? "" : trim(arg.substr(sp));
        string atype = trim((sp == string::npos) ? arg : arg.substr(0, sp));
        if (i) rsig += ", ";
        rsig += translate_type(atype, 2, unsafed, "") + " " + aname;
    }
    //if (unsafed) rsig = "unsafe "+rsig;
    return "\tpublic delegate "+rsig+");\n";
}

static string replace_argument_types(string sig, int opaqueLevel) {
    bool isOpaque = opaqueLevel & 1;
    bool csharp = opaqueLevel & 2;
    auto lpar = sig.find('(');
    if (lpar == string::npos) return sig;
    auto rpar = sig.find(')', lpar);
    if (rpar == string::npos) return sig;
    string func = read_symbol_backwards(sig, lpar);
    string rtype = trim(sig.substr(0, lpar - func.size()));
    vector<string> args;
    string buf = trim(sig.substr(lpar + 1, rpar - lpar - 1));
    while (buf.size()) {
        auto comma = buf.find(',');
        if (comma == string::npos) break;
        args.push_back(trim(buf.substr(0, comma)));
        buf = trim(buf.substr(comma+1));
    }
    if (buf.size()) args.push_back(buf);
    bool unsafed = false;
    string parent = csharp ? global_symbol_parent[func] : "";
    if (parent.size() && func.size() > parent.size()) {
        func = func.substr(parent.size() + 1);
    }
    string rsig = translate_type(rtype, opaqueLevel, unsafed, parent) + " " + func + "(";
    for(int i=0;i<args.size();i++) {
        string arg = args[i];
        auto sp = arg.rfind(' ');
        string aname = (sp == string::npos) ? "" : trim(arg.substr(sp));
        string atype = trim((sp == string::npos) ? arg : arg.substr(0, sp));
        if (i) rsig += ", ";
        if (!i && parent.size()) {
            if (atype.size() && atype[atype.size()-1] == '*') {
                atype = trim(atype.substr(0, atype.size() - 1));
            }
            rsig += "[MarshalAs(UnmanagedType.Struct)] this " + atype + " this_";
        } else {
            rsig += translate_type(atype, opaqueLevel, unsafed, i ? "" : parent) + " " + aname;
        }
    }
    rsig += ");\n";
    if (unsafed) rsig = "unsafe "+rsig;
    return rsig;
}

static bool enum_mode = false;
static string translate_to_cs(string line) {
    string tline = trim(line);
    bool isUnsafe = false;
    if (tline.find("unsafe ") == 0) {
        isUnsafe = true;
        tline = trim(tline.substr(6));
    }
    tline = str_replace(tline, "typedef ", "");
    tline = str_replace(tline, "struct", "public class");
    tline = str_replace(tline, "enum", "public enum");
    tline = str_replace(tline, "public public", "public");
    if (tline == "public enum {") {
        enum_mode = true;
        return "";
    }
    string indent = enum_mode ? "\t\t" : "\t\tpublic ";
    if (!tline.find("}")) {
        indent = "\t";
        tline = "}";
        enum_mode = false;
    } else if (!tline.find("public enum ")) {
        enum_mode = true;
        indent = "\t\t";
    } else if (!tline.find("public class ")) {
        string out, cls = read_symbol(trim(tline.substr(13)), 0);
        int hasCtor = global_symbol_parent[cls+"_new"].size();
        if (hasCtor) {
            string ctorArgs = global_symbol_sig[cls+"_new"];
            string csargs = "";//replace_argument_types(ctorArgs, 2);
            out += "\t[DllImport(\""+strip_filename(output)+systag+"\", CharSet = CharSet.Ansi)]\n";
            //out += "\textern private static IntPtr "+cls+"_new([MarshalAs(UnmanagedType.Struct)] this "+cls+" this_";
            out += "\textern private static IntPtr "+cls+"_new(IntPtr handle";
            if (csargs.size()) {
                out += ", ";
                out += csargs;
            }
            out += ");\n";
        }
        int hasDtor = global_symbol_parent[cls+"_delete"].size();
        if (hasDtor) {
            out += "\t[DllImport(\""+strip_filename(output)+systag+"\", CharSet = CharSet.Ansi)]\n";
            //out += "\textern private static void "+cls+"_delete([MarshalAs(UnmanagedType.Struct)] this "+cls+" this_);\n";
            out += "\textern private static void "+cls+"_delete(IntPtr handle);\n";
        }
        out += "\t[StructLayout(LayoutKind.Sequential)]\n\t";
        if (isUnsafe) out += "unsafe ";
        //out += tline+"\n";
        out += "\tpublic class "+cls+" {\n";
        out += "\t\tIntPtr handle;\n";
        if (hasCtor) out += "\t\tpublic "+cls+"() {handle = "+cls+"_new(new IntPtr(0));}\n";
        if (hasDtor) out += "\t\t~"+cls+"() {"+cls+"_delete(handle);}\n";
        return out;
    }
    return indent+tline+"\n";
}

static int extract_public_signatures(string& head, string& post_head, string& public_post_head, vector<string>& public_csv, string& local_post_head, string& local_head, string& line, int isPacked, int isPublic, int isPrivate, int isOpaque) {
    string space, code;
    int trigger = 0;
    for(auto c: line) {
        if (!isspace(c)) trigger = 1;
        if (trigger) code += c;
        else space += c;
    }
    code = trim(code);
    bool isNotFunction = false;
    if (!code.size() || code[0] == '}') isNotFunction = true;
    if (code.find("if ") == 0) isNotFunction = true;
    if (code.find("else ") == 0) isNotFunction = true;
    if (code.find("switch ") == 0) isNotFunction = true;
    if (code.find("for ") == 0) isNotFunction = true;
    if (code.find("while ") == 0) isNotFunction = true;
    if (code.find("typedef") == 0 && code.find(';') != string::npos) {
        head += line + "\n";
        if (code.find("(*") != string::npos) {
            public_csv.push_back("\x02"+code);
        }
	    //local_head += line + "\n";
        return 1;
    }
    auto end = code.rfind(')');
    if (end != string::npos && code.find('{') != string::npos) {
        string hcode = code.substr(0, end+1) + ";\n";
        string entry = extract_entry_point(hcode);
        string par = global_symbol_parent[entry];
        string lentry = entry;
        if (par.size() && entry.size() > par.size()) {
            lentry = entry.substr(par.size() + 1);
        }
        if (isPublic) {
            if (lentry != "new" && lentry != "delete") {
                public_csv.push_back("\t[DllImport(\""+strip_filename(output)+systag+"\", CharSet = CharSet.Ansi, EntryPoint = \""+entry+"\")]\n");
                public_csv.push_back("\textern public static "+replace_argument_types(hcode, 2));
            }
            post_head += "DLLEXPORT " + hcode;
            public_post_head += "DLLIMPORT " + replace_argument_types(hcode, 0);
            //local_post_head += "DLLEXPORT " + hcode;
            line = space + "DLLEXPORT " + code;
        } else if (isOpaque) {
            if (lentry != "new" && lentry != "delete") {
                public_csv.push_back("\t[DllImport(\""+strip_filename(output)+systag+"\", CharSet = CharSet.Ansi, EntryPoint = \""+entry+"\")]\n");
                public_csv.push_back("\textern public static "+replace_argument_types(hcode, 3));
            }
            //post_head += "DLLEXPORT " + hcode;
            //public_post_head += "DLLIMPORT " + replace_argument_types(hcode, 1);
            //local_post_head += "DLLEXPORT " + hcode;
            line = space + "DLLEXPORT " + code;
        } else if (isPrivate) {
            local_head += "static " + hcode;
        } else if (!isNotFunction) {
            head += hcode;
        }
        return 0;
    }
    return 0;
}

static string resolve_member_functions(string line, int isHead, int isStatic, int isConst, int isCustom, map<string, string>& var_type_table) {
    auto col = line.find("::");
    if (col == string::npos) return line;
    auto sp = line.rfind(' ', col);
    auto tab = line.rfind('\t', col);
    if (tab != string::npos && tab > sp) sp = tab;
    string type = line.substr(sp+1, col-sp-1);
    auto par = line.find('(', col);
    auto rparen = line.find(')', par);
    int hasArgs = 0;
    for(int i=par+1;i<rparen;i++) {
        if (!isspace(line[i])) {
            hasArgs = 1;
            break;
        }
    }
    string func = line.substr(col+2, par-col-2);
    string realfunc = type + "_" + func;
    global_symbol_parent[realfunc] = type;
    global_symbol_sig[realfunc] = line;
    if (isCustom) var_type_table[realfunc] = "custom";
    string out = line.substr(0, sp+1) + realfunc + "(";
    if (!isStatic) {
        if (isConst) {
            out += "const ";
        }
        out += type + "*";
        if (!isHead) {
            if (func != "new") out += " const";
            out += " restrict this";
        } else {
            out += " this";
        }
        if (hasArgs) out += ", ";
    }
    line = line.substr(par+1);
    auto curly = line.find('\n');
    bool isCtor = !isCustom && !isHead && (func == "new");
    out += line;
    if (isCtor) out += "\n\tif (!this) this = malloc(sizeof("+type+"));";
    return out;
}

static bool valid_type_def(string buffer) {
    bool valid = false;
    for(auto c: buffer) {
        if (c == ' ') valid = true;
        if (isalnum(c) || c == '_' || c == '*' || c == ' ') continue;
        return false;
    }
    return valid;
}

static string trim_type(string type) {
    for(;;) {
        string test = type;
        type = trim(type);
        type = str_replace(type, "const ", "");
        type = str_replace(type, "static ", "");
        type = str_replace(type, " restrict", "");
        type = str_replace(type, "  ", " ");
        type = str_replace(type, " *", "*");
        if (type == test) break;
    }
    return type;
}

static string rewrite_delete(string& code, string type, string name, map<string, string>& var_type_table) {
    type = var_type_table[name];

    if (!type.size()) {
        return "Unknown type for "+name;
    }
    bool isPtr = false;
    if (type[type.size()-1] == '*') {
        isPtr = true;
        type = type.substr(0, type.size()-1);
    }
    string newcode = type + "_delete";
    bool isCustom = (var_type_table[newcode] == "custom");
    newcode += "(";
    if (!isPtr) {
        newcode += "&";
    }
    newcode += name + ");";
    if (isPtr && !isCustom) {
        // FIXME: this is horrible. doing free() here COMPLETELY BREAKS public delete functions using "custom" keyword
        newcode += " free(" + name + ");";
    }
    code = str_replace(code, "delete "+name, newcode);
    return "";
}

static string rewrite_new(string& code, string type, string name, string obuffer, map<string, string>& var_type_table) {
    type = name;
    bool isPtr = false;
    if (type[type.size()-1] == '*') {
        isPtr = true;
        type = type.substr(0, type.size()-1);
    }
    string newcode = type+"_new(0";
    auto hack = code.find(obuffer) + obuffer.size();
    while (isspace(code[hack])) hack++;
    if (code[hack] != ')') {
        newcode += ", ";
    }
    code = str_replace(code, obuffer, newcode);
    return "";
}

static string extract_variable_types(string& code, map<string, string>& var_type_table, map<string,int>& symbol_flags, bool hasTail) {
    bool hasCurly = (code.find('{') != string::npos);
    bool done = false;
    while (!done) {
        string buffer;
        bool valid = true;
        bool skip_whitespace = true;
        code = trim(code);
        done = true;
        for(auto c: code) {
            if (skip_whitespace && isspace(c)) continue;
            skip_whitespace = false;
            if (c == ';' || c == '=' || c == ',' || c == ')') {
                buffer = trim(buffer);
                if (valid && valid_type_def(buffer)) {
                    auto pos = buffer.rfind(' ');
                    string type = buffer.substr(0, pos);
                    string name = buffer.substr(pos + 1);
                    if (type == "delete") {
                        // weird place to handle this but it naturally gets caught here anyways...
                        string err = rewrite_delete(code, type, name, var_type_table);
                        if (err.size()) return err;
                        done = false;
                        break;
                    } else if (type != "return") {
                        type = trim_type(type);
                        var_type_table[name] = type;
                        symbol_flags[type] |= 4 + 8;
                        if (hasTail) {
                            symbol_flags[type] |= 1;
                        }
                    }
                } else {
                    valid = false;
                }
                buffer = "";
                continue;
            }
            if (c == ';' || c == ',' || c == '(') {
                string obuffer = buffer + c;
                buffer = trim(buffer);
                if (valid_type_def(buffer)) {
                    auto pos = buffer.rfind(' ');
                    string type = buffer.substr(0, pos);
                    string name = buffer.substr(pos + 1);
                    symbol_flags[type] |= 8;
                    if (type == "new") {
                        string err = rewrite_new(code, type, name, obuffer, var_type_table);
                        if (err.size()) return err;
                        done = false;
                        break;
                    }
                }
                skip_whitespace = true;
                buffer = "";
                valid = true;
                continue;
            }
            if (isspace(c)) {
                buffer = trim(buffer) + c;
            } else {
                buffer += c;
            }
        }
    }
    return "";
}

static string rewrite_member_calls(string& code, map<string, string>& var_type_table) {
    size_t l, r = 0;
    for(;;) {
        r = code.find('(', r ? (r+1) : 0);
        if (r == string::npos) break;
        string type = "->";
        l = code.rfind("->", r);
        size_t l2 = code.rfind(".", r);
        if (l == string::npos || (l2 != string::npos && l2 < l)) {
            l = l2;
            type = ".";
        }
        if (l == string::npos) continue;
        string func;
        int state = 1;
        for(int i=l+type.size();i<r;i++) {
            char c = code[i];
            if (state == 2) {
                if (isspace(c)) continue;
                if (c != '(') {
                    state = 0;
                    break;
                }
            }
            if (isalpha(c) || c == '_' || (isalnum(c) && (i > l+type.size()))) {
                func += c;
                state = 1;
                continue;
            } else if (isspace(c)) {
                state = 2;
                continue;
            } else if (c == '(') {
                state = 1;
                break;
            }
            state = 0;
            break;
        }
        if (state != 1) continue;
        string obj;
        while (--l >= 0) {
            char c = code[l];
            if (isalnum(c) || c == '_') {
                obj = c + obj;
                continue;
            }
            break;
        }
        auto t = var_type_table[obj];
        if (!t.size()) return "'" HILITE + obj + REGGS "' has unknown type";
        while (isspace(code[r+1])) r++;
        bool isPtr = false;
        if (t[t.size()-1] == '*') {
            isPtr = true;
            t = t.substr(0, t.size()-1);
        }
        if (isPtr && type != "->") return "'" HILITE + obj + REGGS "' is a pointer, use -> for member calls";
        if (!isPtr && type != ".") return "'" HILITE + obj + REGGS "' is not a pointer, use . for member calls";
        string newcode = code.substr(0, l+1) + t + "_" + func + "(";
        if (isPtr) {
            newcode += obj;
        } else {
            newcode += "&"+obj;
        }
        if (code[r+1] != ')') newcode += ", ";
        newcode += code.substr(r+1);
        code = newcode;
    }
    return "";
}

struct SourceFile {
    string filename;
    string head, body, tail;
    string local_head, post_head, public_post_head, local_post_head, public_head;
    string public_cs;
    vector<string> public_csv;
    string template_class;
    vector<string> template_vars;
    vector<string> lines;
    map<string, string> var_type_table;
    int outputToHeader;
    bool valid;
    bool processed;
    int rearrangements;
    bool rebuild;
    map<string,int> symbol_flags;
    vector<SourceFile*> exports_to, imports_from;
    SourceFile(const char* filename_) {
        valid = false;
        processed = false;
        filename = filename_;
        rearrangements = 0;
        rebuild = false;
        FILE*fp = fopen(filename_, "r");
        if (!fp) return;
        int line_no = 0;
        while (!feof(fp)) {
            string line = "";
            while (!feof(fp)) {
                int c = fgetc(fp);
                if (c == EOF) break;
                if (c == '\r') continue;
                if (c == '\n') break;
                line += (char)c;
            }
            line_no++;
            string l = trim(line);
            if (l.find("#copyright") == 0) {
                string cline = trim(l.substr(10));
                lines.push_back("// Copyright (C) "+cline+"\n");
                auto clip = cline.find('<');
                if (clip != string::npos) {
                    cline = trim(cline.substr(0, clip));
                }
                if (copyright.size()) {
                    copyright += ", ";
                }
                copyright += cline;
                continue;
            }
            if (l.find("#template") == 0) {
                lines.push_back("");
                if (template_class.size()) {
                    fprintf(stderr, HILITE "%s:%d: " WARNING_STYLE "warning:" REGGS " multiple #template directives in one file\n", filename.c_str(), line_no);
                    continue;
                }
                line = trim(line.substr(9));
                auto lt = line.find('<');
                auto gt = line.find('>');
                if (lt == string::npos || gt == string::npos) {
                    fprintf(stderr, HILITE "%s:%d: " WARNING_STYLE "warning:" REGGS " invalid #template directive\n", filename.c_str(), line_no);
                    continue;
                }
                template_class = trim(line.substr(0, lt));
                line = trim(line.substr(lt + 1, gt - lt - 1));
                while (line.size()) {
                    auto comma = line.find(',');
                    if (comma == string::npos) break;
                    template_vars.push_back(trim(line.substr(0, comma)));
                    line = trim(line.substr(comma+1));
                }
                template_vars.push_back(line);
                continue;
            }
            lines.push_back(line);
        }
        fclose(fp);
        valid = true;
    }
    bool Compile(map<string, string> template_params = map<string, string>()) {
        outputToHeader = 0;
        int line_no = 0;
        int ifdef_depth = 0;
        int platform = 0;
        int build_mode = 0;
        for(string line: lines) {
            line_no++;
            for(auto i: template_params) {
                line = str_replace(line, "<"+i.first+">", i.second);
                line = str_replace(line, i.first, i.second);
            }
            string space, code;
            bool trigger = false;
            for(auto c: line) {
                if (!isspace(c)) trigger = true;
                if (trigger) code += c;
                else space += c;
            }
            code = trim(code);
            bool plat = true;
            if (platform == PLAT_WINDOWS) plat = os == "windows";
            if (platform == PLAT_LINUX) plat = os == "linux";
            if (platform == PLAT_APPLE) plat = os == "apple";
            if (platform == -PLAT_WINDOWS) plat = os != "windows";
            if (platform == -PLAT_LINUX) plat = os != "linux";
            if (platform == -PLAT_APPLE) plat = os != "apple";
            if (build_mode == 1 && dll_mode != 0) plat = false;
            if (build_mode == 2 && dll_mode != 1) plat = false;
            if (code.find("#link") == 0) {
                string lib = trim(code.substr(5));
                if (plat) libs.push_back(lib);
                continue;
            }
            if (code.find("#vendor") == 0) {
                string x = trim(code.substr(7));
                if (plat) {
                    if (vendor.size()) vendor += ", ";
                    vendor += x;
                }
                continue;
            }
            if (code.find("#product") == 0) {
                string x = trim(code.substr(8));
                if (!product.size() && plat) product = x;
                continue;
            }
            if (code.find("#detail") == 0) {
                string x = trim(code.substr(7));
                if (!details.size() && plat) details = x;
                continue;
            }
            if (code.find("#version") == 0) {
                string x = trim(code.substr(8));
                if (!version.size() && plat) version = x;
                continue;
            }
            if (code.find("#icon") == 0) {
                string x = trim(code.substr(5));
                if (!icon.size() && plat) icon = x;
                continue;
            }
            if (code.find("#manifest") == 0) {
                string x = trim(code.substr(9));
                if (!manifest.size() && plat) manifest = x;
                continue;
            }
            if (code.find("#public_") == 0) {
                if (plat) {
                    string z = space + "#" + line.substr(line.find("#public_") + 8);
                    public_head += z + "\n";
                    //local_head += z + "\n";
                    head += z + "\n";
                    body += line_directive(z+"\n", human, line_no, filename);
                    //body += z + "\n";
                }
                continue;
            }
            if (code.find("#global_") == 0) {
                if (plat) {
                    string z = space + "#" + line.substr(line.find("#global_") + 8);
                    //local_head += z + "\n";
                    head += z + "\n";
                    body += line_directive(z+"\n", human, line_no, filename);
                }
                continue;
            }
            if (code.find("#define") == 0 || code.find("#include") == 0) {
                if (plat) {
                    local_head += line + "\n";
                    //body += line_directive(line+"\n", human, line_no, filename);
                }
                continue;
            }
            if (code.find("#if") == 0 || code.find("#endif") == 0 || code.find("#else") == 0 || code.find("#elif") == 0) {
                if (code == "#ifdef BUILD_EXE") {
                    build_mode = 1;
                    ifdef_depth++;
                } else if (code == "#ifdef BUILD_DLL") {
                    build_mode = 2;
                    ifdef_depth++;
                } else if (code == "#ifndef BUILD_EXE") {
                    build_mode = 2;
                    ifdef_depth++;
                } else if (code == "#ifndef BUILD_DLL") {
                    build_mode = 1;
                    ifdef_depth++;
                } else if (code == "#ifdef OS_WINDOWS") {
                    platform = PLAT_WINDOWS;
                    ifdef_depth++;
                } else if (code == "#ifdef OS_LINUX") {
                    platform = PLAT_LINUX;
                    ifdef_depth++;
                } else if (code == "#ifdef OS_APPLE") {
                    platform = PLAT_APPLE;
                    ifdef_depth++;
                } else if (code == "#ifndef OS_WINDOWS") {
                    platform = -PLAT_WINDOWS;
                    ifdef_depth++;
                } else if (code == "#ifndef OS_LINUX") {
                    platform = -PLAT_LINUX;
                    ifdef_depth++;
                } else if (code == "#ifndef OS_APPLE") {
                    platform = -PLAT_APPLE;
                    ifdef_depth++;
                } else if (code == "#else") {
                    if (ifdef_depth == 1) {
                        platform = -platform;
                        if (build_mode) {
                            build_mode = 3 - build_mode;
                        }
                    }
                } else {
                    if (code.find("#if") == 0) ifdef_depth++;
                    if (code.find("#endif") == 0) {
                        if (ifdef_depth <= 1) {
                            platform = 0;
                            build_mode = 0;
                        }
                        ifdef_depth--;
                    }
                }
                //local_head += line + "\n";
                head += line + "\n";
                body += line_directive(line+"\n", human, line_no, filename);
                //body += line + "\n";
                continue;
            }
            if (platform == PLAT_WINDOWS && os != "windows") continue;
            if (platform == PLAT_LINUX && os != "linux") continue;
            if (platform == PLAT_APPLE && os != "apple") continue;
            if (platform == -PLAT_WINDOWS && os == "windows") continue;
            if (platform == -PLAT_LINUX && os == "linux") continue;
            if (platform == -PLAT_APPLE && os == "apple") continue;
            int isConst = 0;
            if (code.find("const ") == 0) {
                isConst = 1;
                code = code.substr(5);
                while (code.size() && isspace(code[0])) {
                    code = code.substr(1);
                }
            }
            int isCustom = 0;
            if (code.find("custom ") == 0) {
                isCustom = 1;
                code = code.substr(6);
                while (code.size() && isspace(code[0])) {
                    code = code.substr(1);
                }
            }
            int isOpaque = 0;
            if (code.find("opaque ") == 0) {
                isOpaque = 1;
                code = code.substr(6);
                while (code.size() && isspace(code[0])) {
                    code = code.substr(1);
                }
            }
            int isPacked = 0;
            if (code.find("packed ") == 0) {
                isPacked = 1;
                code = code.substr(6);
                while (code.size() && isspace(code[0])) {
                    code = code.substr(1);
                }
            }
            int isPrivate = 0;
            if (code.find("private ") == 0) {
                isPrivate = 1;
                code = code.substr(7);
                while (code.size() && isspace(code[0])) {
                    code = code.substr(1);
                }
            }
            int isPublic = 0;
            if (code.find("public ") == 0) {
                isPublic = 1;
                code = code.substr(6);
                while (code.size() && isspace(code[0])) {
                    code = code.substr(1);
                }
            }
            int isStatic = 0;
            if (code.find("static ") == 0) {
                isStatic = 1;
                code = code.substr(6);
                while (code.size() && isspace(code[0])) {
                    code = code.substr(1);
                }
            }
            auto memberHint = code.find("::");
            int isMember = (memberHint != string::npos);
            if (isMember) {
                string obj = read_symbol_backwards(code, memberHint);
                if (obj.size()) {
                    var_type_table["this"] = obj+"*";
                    symbol_flags[obj] |= 4 + 8;
                } else {
                    fprintf(stderr, HILITE "%s:%d: " WARNING_STYLE "warning:" REGGS " failed to deduce type for 'this'\n", filename.c_str(), line_no);
                }
            }
            string hcode = resolve_member_functions(code, 1, isStatic, isConst, isCustom, var_type_table);
            extract_public_signatures(head, post_head, public_post_head, public_csv, local_post_head, local_head, hcode, isPacked, isPublic, isPrivate, isOpaque);
            string err = extract_variable_types(code, var_type_table, symbol_flags, tail.size());
            if (err.size()) {
                fprintf(stderr, HILITE "%s:%d: " ERROR_STYLE "error:" REGGS " %s\n", filename.c_str(), line_no, err.c_str());
                return false;
            }
            rewrite_structs(code, head, public_head, public_csv, local_head, tail, space, &outputToHeader, isPacked, isPublic, isOpaque, isPrivate, symbol_flags);
            rewrite_enums(code, head, public_head, public_csv, local_head, tail, space, &outputToHeader, isPublic, isOpaque, isPrivate);
            err = rewrite_member_calls(code, var_type_table);
            if (err.size()) {
                fprintf(stderr, HILITE "%s:%d: " ERROR_STYLE "error:" REGGS " %s\n", filename.c_str(), line_no, err.c_str());
                return false;
            }
            if (isMember) {
                code = resolve_member_functions(code, 0, isStatic, isConst, isCustom, var_type_table);
            }
            if (isStatic && !isMember) code = "static " + code;
            if (isConst && !isMember) code = "const " + code;
            int oth = outputToHeader;
            if (tail.size() && (code == "}" || code == "};")) {
                code = tail;
                tail = "";
                outputToHeader = 0;
            }
            code = space + code + "\n";
            if (oth == 1) {
                head += code;
                public_head += code;
                public_csv.push_back("\x01" + code);
                //local_head += code;
            } else if (oth == 2) {
                head += code;
                //local_head += code;
            } else if (oth == 3) {
                local_head += code;
            } else {
                body += line_directive(code, human, line_no, filename);
                //body += code;
                code = "";
            }
        }
        return true;
    }
};

void set_rebuild_recursive(SourceFile* f) {
    if (f->rebuild) return;
    f->rebuild = true;
    for(auto d: f->exports_to) {
        set_rebuild_recursive(d);
    }
}

int usage() {
    printf("usage: auc <input files>\n");
    printf("\t/OUT:<output-filename> (-o)\n");
    printf("\t/DEBUG (-g)\n");
    printf("\t/DIR:<build-directory> (-d)\n");
    printf("\t/OS:<operating-system> (-m) [linux, windows, win32]\n");
    printf("\t/DLL (-shared)\n");
    printf("\t/VERBOSE (-v)\n");
    printf("\t/HELP (-h)\n");
    printf("\t/PRETTY\n");
    return 0;
}

int help() {
    printf("usage: auc <input files> [options] [more input files]\n\nSupported input types:\n\t.au (Austere)\n\t.cs (C#)\n\t.c, .cpp (C/C++)\n\t.dll, .so, .o (Libraries)\n\t.ico, .rc, .res, .manifest (Resources)\n\n");
    printf("/OUT:<output-filename> (-o)\n\n");
    printf("/DEBUG (-g)\n\n");
    printf("/DIR:<build-directory> (-d)\n * Intermediate compile results (generated .o .c and .h files)\n\n");
    printf("/OS:<operating-system> (-m)\n * Any cross compiler toolchain (eg. 'x86_64-w64-mingw32')\n   or a preset: 'linux', 'windows' (aka 'win64'), 'win32'\n\n");
    printf("/DLL (-shared)\n * Produce a .dll file instead of an .exe file.\n   (WARNING: Writes *.dll.h and *.dll.cs in the same dir as the .dll file)\n\n");
    printf("/VERBOSE (-v)\n * Show the sub-commands being executed.\n\n");
    printf("/HELP (-h)\n * Show this help screen.\n\n");
    printf("/PRETTY\n * Generate pretty .c files from .au sources\n   (Ruins compiler and debugger messages, only meant as an escape hatch.) \n\n");
    printf("-I, -D, -L, -l\n * Passed through to the compiler or linker.\n\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc <= 1) return usage();
    /*
    char* envv = getenv("CC"); if (envv) compiler = envv;
    envv = getenv("CPP"); if (envv) cpp_compiler = envv;
    envv = getenv("LD"); if (envv) linker = envv;
    envv = getenv("CS"); if (envv) cs_compiler = envv;
    */
    string last_flag;
    for(int i=1;i<argc;i++) {
        string arg(argv[i]);
        if (!arg.size()) continue;
        string argl = lowercase(arg);
        if (arg.size() >= 2) {
            string z = arg.substr(0, 2);
            if (z == "-D" || z == "-I") {
                cflags += " "+arg;
                continue;
            }
            if (z == "-L" || z == "-l") {
                ldflags += " "+arg;
                continue;
            }
        }
        if (arg[0] == '-' || (arg[0] == '/' && (file_mtime(arg) < 0))) {
            last_flag = lowercase(argv[i]);
            arg = argl = "";
            if (last_flag[0] == '-') {
                auto co = last_flag.find('=');
                if (co != string::npos) {
                    arg = last_flag.substr(co+1);
                    argl = lowercase(arg);
                    last_flag = last_flag.substr(0, co);
                }
            }
            if (last_flag[0] == '/') {
                auto co = last_flag.find(':');
                if (co != string::npos) {
                    arg = last_flag.substr(co+1);
                    argl = lowercase(arg);
                    last_flag = last_flag.substr(0, co);
                }
            }
        }
        if (last_flag == "-d" || last_flag == "/dir") {
            if (!arg.size()) continue;
            build_dir = arg;
            if (!build_dir.size()) build_dir = ".";
            if (build_dir[build_dir.size()-1] != '/') build_dir += "/";
            last_flag = "";
            continue;
        } else if (last_flag.size() > 2 && last_flag.substr(0, 2) == "-d") {
            build_dir = last_flag.substr(2);
            if (!build_dir.size()) build_dir = ".";
            if (build_dir[build_dir.size()-1] != '/') build_dir += "/";
            last_flag = "";
            continue;
        } else if (last_flag == "-m" || last_flag == "/os") {
            if (!arg.size()) continue;
            os = argl;
            last_flag = "";
            continue;
        } else if (last_flag.size() > 2 && last_flag.substr(0, 2) == "-m") {
            os = last_flag.substr(2);
            last_flag = "";
            continue;
        } else if (last_flag == "-o" || last_flag == "/out") {
            if (!arg.size()) continue;
            auto_output = false;
            output = arg;
            last_flag = "";
            continue;
        } else if (last_flag.size() > 2 && last_flag.substr(0, 2) == "-o") {
            auto_output = false;
            output = last_flag.substr(2);
            last_flag = "";
            continue;
        } else if (last_flag == "--compiler" || last_flag == "/cc") {
            if (!arg.size()) continue;
            compiler = cpp_compiler = linker = arg;
            last_flag = "";
            continue;
        } else if (last_flag == "--cpp-compiler" || last_flag == "/cpp") {
            if (!arg.size()) continue;
            cpp_compiler = arg;
            last_flag = "";
            continue;
        } else if (last_flag == "--cs-compiler" || last_flag == "/cs") {
            if (!arg.size()) continue;
            cs_compiler = arg;
            last_flag = "";
            continue;
        } else if (last_flag == "--linker" || last_flag == "/ld") {
            if (!arg.size()) continue;
            linker = arg;
            last_flag = "";
            continue;
        } else if (last_flag == "--au-flags" || last_flag == "/auflags") {
            if (!arg.size()) continue;
            user_auflags = arg;
            last_flag = "";
            continue;
        } else if (last_flag == "--c-flags" || last_flag == "/cflags") {
            if (!arg.size()) continue;
            user_cflags = arg;
            last_flag = "";
            continue;
        } else if (last_flag == "--cpp-flags" || last_flag == "/cppflags") {
            if (!arg.size()) continue;
            user_cppflags = arg;
            last_flag = "";
            continue;
        } else if (last_flag == "--ld-flags" || last_flag == "/ldflags") {
            if (!arg.size()) continue;
            user_ldflags = arg;
            last_flag = "";
            continue;
        } else if (last_flag == "--cs-flags" || last_flag == "/csflags") {
            if (!arg.size()) continue;
            user_csflags = arg;
            last_flag = "";
            continue;
        } else if (last_flag == "-h" || last_flag == "-?" || last_flag == "--help" || last_flag == "/h" || last_flag == "/?" || last_flag == "/help") {
            return help();
        } else if (last_flag == "-g" || last_flag == "/debug") {
            debug_mode = true;
            last_flag = "";
            continue;
        } else if (last_flag == "-shared" || last_flag == "/dll") {
            dll_mode = true;
            last_flag = "";
            continue;
        } else if (last_flag == "-v" || last_flag == "/verbose") {
            quiet = false;
            last_flag = "";
            continue;
        } else if (last_flag == "/pretty") {
            human = true;
            last_flag = "";
            continue;
        }
        if (!output.size()) {
            auto_output = true;
            output = argv[i];
        }
        string ext = extract_ext(argv[i]);
        if (ext == "c") c_files.push_back(argv[i]);
        if (ext == "rc") {
            rc_files.push_back(argv[i]);
            cs_use_res = true;
        }
        if (ext == "res") {
            res_files.push_back(argv[i]);
            cs_use_res = true;
        }
        if (ext == "ico") icon = argv[i];
        if (ext == "manifest") manifest = argv[i];
        if (ext == "so" || ext == "dll" || ext == "o") dll_files.push_back(argv[i]);
        if (ext == "cpp") cpp_files.push_back(argv[i]);
        if (ext == "asm") asm_files.push_back(argv[i]);
        if (ext == "rs") rs_files.push_back(argv[i]);
        if (ext == "cs") cs_files.push_back(argv[i]);
        if (ext == "au") {
            auto f = new SourceFile(argv[i]);
            if (!f->valid) {
                fprintf(stderr, HILITE "%s: " ERROR_STYLE "error:" REGGS " parse failed\n", argv[i]);
                return 1;
            }
            files.push_back(f);
        }
    }
    if (os == "windows" || os == "win32" || os == "win64" || os == "win") {
        if (os == "win32") {
            os = "i686";
            cpu_flags = "-march=k6 -mtune=k8";
            asm_fmt = "elf";
            //systag = "_x86";
        } else {
            os = "x86_64";
            //systag = "_x64";
        }
        compiler = os + "-w64-mingw32-gcc";
        cpp_compiler = os + "-w64-mingw32-g++";
        linker = cpp_files.size() ? cpp_compiler : compiler;
        resource_compiler = os + "-w64-mingw32-windres";
        ldflags += " -Wl,--subsystem,windows -mwindows";
        cflags += " -DOS_WINDOWS";
        os = "windows";
        dllext = ".dll";
        exeext = ".exe";
    } else if (os == "linux" || os == "lin32" || os == "lin64" || os == "lin") {
        if (os == "lin32") {
            cpu_flags = "-m32 -march=k6 -mtune=k8";
            asm_fmt = "elf";
            //systag = "_x86";
        } else {
            //systag = "_x64";
        }
        cflags += " -DOS_LINUX";
        os = "linux";
    } else {
        cpu_flags = "";
        release_flags = "-O2";
        debug_flags = "-g";
        string test, cmd;
        const char* tries[] = {"-cc", "-gcc", 0};
        for(int i=0;tries[i];i++) {
            cmd = os + tries[i];
            test = "which " + cmd + " 2>/dev/null >/dev/null";
            if (!system(test.c_str())) {
                compiler = cmd;
                linker = cmd;
            }
        }
    }
    if (!compiler.size() && !system("which cc 2>/dev/null >/dev/null")) compiler = "cc";
    if (!compiler.size() && !system("which gcc 2>/dev/null >/dev/null")) compiler = "gcc";
    if (!compiler.size() && !system("which clang 2>/dev/null >/dev/null")) compiler = "clang";
    if (!cpp_compiler.size() && !system("which c++ 2>/dev/null >/dev/null")) cpp_compiler = "c++";
    if (!cpp_compiler.size() && !system("which g++ 2>/dev/null >/dev/null")) cpp_compiler = "g++";
    if (!cpp_compiler.size() && !system("which clang++ 2>/dev/null >/dev/null")) cpp_compiler = "clang++";
    if (!asm_compiler.size() && !system("which nasm 2>/dev/null >/dev/null")) asm_compiler = "nasm";
    if (!linker.size()) linker = cpp_files.size() ? cpp_compiler : compiler;
    if ((files.size() || c_files.size()) && !compiler.size()) {
        fprintf(stderr, ERROR_STYLE "error:" REGGS " no C compiler found, please specify with /CC:{your-c-compiler}\n");
        return 1;
    }
    if (cpp_files.size() && !cpp_compiler.size()) {
        fprintf(stderr, ERROR_STYLE "error:" REGGS " no C++ compiler found, please specify with /CPP:{your-C++-compiler}\n");
        return 1;
    }
    if (asm_files.size() && !asm_compiler.size()) {
        fprintf(stderr, ERROR_STYLE "error:" REGGS " no ASM compiler found, please specify with /ASM:{your-assembler}\n");
        return 1;
    }

    if (!resource_compiler.size() && !system("which windres 2>/dev/null >/dev/null")) resource_compiler = "windres";
    if (!resource_compiler.size() && !system("which x86_64-w64-mingw32-windres 2>/dev/null >/dev/null")) resource_compiler = "x86_64-w64-mingw32-windres";
    if (!resource_compiler.size() && !system("which i686-w64-mingw32-windres 2>/dev/null >/dev/null")) resource_compiler = "i686-w64-mingw32-windres";

    string head, body;
    map<string, string> template_renders;
    bool more = true;
    bool ok = true;
    while (more) {
        more = false;
        for(auto f: files) {
            if (f->processed) continue;
            // FIXME: detect use of unresolved templates, generate virtual implementation files for the templates, mark the templates resolved, continue loop
            if (!f->template_class.size()) {
                if (!f->Compile()) {
                    return 1;
                }
                f->processed = true;
            }
        }
        if (!ok) break;
    }
    if (!(build_dir.size() >= 1 && build_dir[0] == '/') && (build_dir.size()<2 || build_dir.substr(0,2) != "./")) {
        build_dir = "./"+build_dir;
    }
    mkdir(build_dir.c_str(), 0777);
    string bdir = build_dir + os + (debug_mode ? "-debug/" : "-release/");
    cflags += " -I'"+bdir+"'";
    mkdir(bdir.c_str(), 0777);
    string gdir = build_dir + "generic/";
    mkdir(gdir.c_str(), 0777);
    string export_h, export_cs;
    for(auto f: files) {
        if (f->template_class.size()) continue;
        string file_id = str_replace(str_replace(f->filename, ".", "_"), "/", "_");
        export_h += f->public_head + f->public_post_head;
        for(auto csl: f->public_csv) {
            if (csl.size() && csl[0] == 1) {
                f->public_cs += translate_to_cs(csl.substr(1));
            } else if (csl.size() && csl[0] == 2) {
                f->public_cs += translate_delegate(csl.substr(1));
            } else {
                f->public_cs += csl;
            }
        }
        export_cs += f->public_cs;
        f->head = "#ifndef "+file_id+"\n" + "#define "+file_id+"\n" + prefix + f->head + f->post_head + "#endif\n";
        f->head = remove_empty_ifdefs(f->head);
        string out_hname = strip_filename(f->filename);
        string out_fn = bdir + out_hname + ".au.h";
        if (!write_file(out_fn, f->head)) {
            fprintf(stderr, HILITE "%s: " ERROR_STYLE "error:" REGGS " failed to write file %s\n", f->filename.c_str(), out_fn.c_str());
            return 1;
        }
    }
    /*
    for(auto f: files) {
        printf("\nSymbol flags for %s:\n", f->filename.c_str());
        for(auto sym: f->symbol_flags) {
            if (!(sym.second&3)) continue;
            printf("\t%s: %d\n", sym.first.c_str(), sym.second&3);
        }
    }
    */
    bool again, solved = false;
    int moves = 0;
    for(int wd=files.size()*files.size()*100;wd--;) {
        again = false;
        for(int i=0;i<files.size();i++) {
            if (files[i]->template_class.size()) continue;
            for(int j=i+1;j<files.size();j++) {
                if (files[j]->template_class.size()) continue;
                for(auto expo: files[j]->symbol_flags) {
                    if (!(expo.second & 2)) continue;
                    if ((files[i]->symbol_flags[expo.first] & 3) == 1) {
                        files[j]->rearrangements++;
                        SourceFile* tmp = files[i];
                        files[i] = files[j];
                        for(int k=j-i;k>=2;k--) {
                            files[i+k] = files[i+k-1];
                        }
                        files[i+1] = tmp;
                        again = true;
                        moves++;
                    }
                }
            }
        }
        if (!again) {
            solved = true;
            break;
        }
    }
    if (moves && !solved) {
        fprintf(stderr, WARNING_STYLE "warning:" REGGS " failed to solve dependency ordering between files:\n");
        for(auto f: files) {
            if (f->template_class.size()) continue;
            if (f->rearrangements < files.size()) continue;
            fprintf(stderr, " * %s\n", f->filename.c_str());
        }
        fprintf(stderr, "\n");
    }
    string include_list;
    for(auto f: files) {
        if (f->template_class.size()) continue;
        string out_hname = strip_filename(f->filename);
        include_list += "#include \""+out_hname+".au.h\"\n";
    }
    if (debug_mode) {
        cflags += " "+debug_flags+" "+cpu_flags;
        ldflags += " "+debug_flags+" "+cpu_flags;
    } else {
        cflags += " "+release_flags+" "+cpu_flags;
        ldflags += " "+release_flags+" "+cpu_flags;
    }
    string output_base;
    string insert = "lib";
    if (os == "windows") insert = "";
    if (dll_mode) {
        if (output.size() <= dllext.size() || output.substr(output.size() - dllext.size()) != dllext) output += dllext;
        output_base = strip_file_ext(output);
        string xtr = extract_filename(output);
        if (xtr.size() <= insert.size() || xtr.substr(insert.size()) != insert) {
            output = extract_dir(output) + insert + xtr;
        }
    } else if (!cs_files.size()) {
        if (output.size() <= exeext.size() || output.substr(output.size() - exeext.size()) != exeext) output += exeext;
    }
    if (!output_base.size()) {
        output_base = strip_filename(output);
    }
    if (auto_output) {
        if (debug_mode) {
            output = "./"+strip_filename(output);
        } else {
            output = "./"+strip_filename(output);
        }
    }
    string out_dir = extract_dir(output);
    mkdir(out_dir.c_str(), 0777);
    string real_output = output;
    if (cs_files.size()) {
        ldflags += " -shared";
        if (dll_mode) {
            cflags += " -DBUILD_DLL";
            output = extract_dir(output) + insert + strip_filename(output) + systag + dllext;
        } else {
            cflags += " -DBUILD_EXE -DBUILD_EXE_AS_DLL";
            output = extract_dir(output) + insert + strip_filename(output) + systag + dllext;
        }
    } else if (dll_mode) {
        ldflags += " -shared";
        cflags += " -DBUILD_DLL";
    } else {
        cflags += " -DBUILD_EXE";
    }
    if ((dll_mode || cs_files.size()) && os == "linux") {
        ldflags += " -Wl,-soname,'./"+extract_filename(output)+"'";
    }

    string obj_list;
    bool relink = false, native = files.size() || c_files.size() || cpp_files.size() || asm_files.size();
    if (dll_files.size()) {
        ldflags += " -Wl,-rpath,.";
    }
    for(auto f: dll_files) {
        cflags += " -I'"+extract_dir(f)+"'";
        ldflags += " -Wl,-rpath,'"+extract_dir(f)+"'";
        obj_list += " "+f;
    }
    for(int i=0;i<files.size();i++) {
        if (files[i]->template_class.size()) continue;
        for(int j=0;j<files.size();j++) {
            if (files[j]->template_class.size()) continue;
            for(auto expo: files[j]->symbol_flags) {
                if (!(expo.second & 2)) continue;
                if ((files[i]->symbol_flags[expo.first] & 3) == 1) {
                    files[i]->imports_from.push_back(files[j]);
                }
                if (files[i]->symbol_flags[expo.first] & 5) {
                    files[j]->exports_to.push_back(files[i]);
                }
            }
        }
    }
    for(auto f: files) {
        string out_ob = bdir + flatten_filename(f->filename) + ".au.o";
        if (should_rebuild(out_ob, file_mtime(f->filename))) {
            set_rebuild_recursive(f);
        }
    }
    for(auto f: files) {
        if (f->template_class.size()) continue;
        string file_id = str_replace(str_replace(f->filename, ".", "_"), "/", "_");
        f->body = include_list + f->local_head + f->local_post_head + f->body;
        string out_base = bdir + flatten_filename(f->filename);
        string out_fn = out_base + ".au.c";
        if (!write_file(out_fn, f->body)) {
            fprintf(stderr, HILITE "%s: " ERROR_STYLE "error:" REGGS " failed to write file %s\n", f->filename.c_str(), out_fn.c_str());
            return 1;
        }
        string out_ob = out_base + ".au.o";
        string srcdir = extract_dir(f->filename);
        string cmd = compiler + " -c -o '"+out_ob+"' '"+out_fn+"' -I'" + srcdir + "' " + cflags + " " + user_auflags;
        obj_list += " '"+out_ob+"'";
        if (f->rebuild) {
            if (!quiet) printf("%s\n", cmd.c_str());
            int r = system(cmd.c_str());
            if (r) return r;
            relink = true;
        }
    }
    for(auto f: c_files) {
        string file_id = str_replace(str_replace(f, ".", "_"), "/", "_");
        string out_ob = bdir + flatten_filename(f) + ".c.o";
        string cmd = compiler + " -c -o '"+out_ob+"' '"+f+"' " + cflags + " " + user_cflags;
        obj_list += " '"+out_ob+"'";
        if (should_rebuild(out_ob, file_mtime(f))) {
            if (!quiet) printf("%s\n", cmd.c_str());
            int r = system(cmd.c_str());
            if (r) return r;
            relink = true;
        }
    }
    for(auto f: cpp_files) {
        string file_id = str_replace(str_replace(f, ".", "_"), "/", "_");
        string out_ob = bdir + flatten_filename(f) + ".cpp.o";
        string cmd = cpp_compiler + " -c -o '"+out_ob+"' '"+f+"' " + cflags + " " + user_cppflags;
        obj_list += " '"+out_ob+"'";
        if (should_rebuild(out_ob, file_mtime(f))) {
            if (!quiet) printf("%s\n", cmd.c_str());
            int r = system(cmd.c_str());
            if (r) return r;
            relink = true;
        }
    }
    for(auto f: asm_files) {
        string file_id = str_replace(str_replace(f, ".", "_"), "/", "_");
        string out_ob = bdir + flatten_filename(f) + ".asm.o";
        string cmd = asm_compiler + " -f"+asm_fmt+" -o '"+out_ob+"' " + user_asmflags;
        obj_list += " '"+out_ob+"'";
        if (should_rebuild(out_ob, file_mtime(f))) {
            if (!quiet) printf("%s\n", cmd.c_str());
            int r = system(cmd.c_str());
            if (r) return r;
            relink = true;
        }
    }
    if (resource_compiler.size()) {
        if (!version.size()) {
            version = "0,0,0,0";
        }
        if (!rc_files.size() && (os == "windows" || cs_files.size())) {
            string rc = winrc;
            rc = str_replace(rc, "$ICON", icon.size() ? ("APPICON ICON "+icon) : "");
            rc = str_replace(rc, "$MANIFEST", manifest.size() ? ("1 24 "+manifest) : "");
            rc = str_replace(rc, "$DETAILS", details);
            rc = str_replace(rc, "$VENDOR", vendor);
            rc = str_replace(rc, "$PRODUCT", product);
            rc = str_replace(rc, "$VERSION", version);
            rc = str_replace(rc, "$COPYRIGHT", copyright);
            rc = str_replace(rc, "$SONAME", extract_filename(output));
            string out_fn = gdir + "default.rc";
            if (read_file(out_fn) != rc) {
                if (!write_file(out_fn, rc)) {
                    string errfn = "default.rc";
                    if (icon.size()) errfn = icon;
                    if (manifest.size()) errfn = manifest;
                    fprintf(stderr, HILITE "%s: " ERROR_STYLE "error:" REGGS " failed to write file %s\n", errfn.c_str(), out_fn.c_str());
                    return 1;
                }
            }
            rc_files.push_back(out_fn);
        }
        for(auto f: res_files) {
            string out_ob = bdir + flatten_filename(f) + ".res.o";
            if (should_rebuild(out_ob, file_mtime(f))) {
                string cmd = resource_compiler + " -o '"+out_ob+"' '"+f+"'";
                if (!quiet) printf("%s\n", cmd.c_str());
                int r = system(cmd.c_str());
                if (r) {
                    //return r;
                    fprintf(stderr, HILITE "%s: " WARNING_STYLE "warning:" REGGS " resource compiler returned code %d (.o target)\n", f.c_str(), r);
                } else {
                    obj_list += " '"+out_ob+"'";
                }
                relink = true;
            }
        }
        for(auto f: rc_files) {
            if (cs_files.size()) {
                string out_res = gdir + flatten_filename(f) + ".rc.res";
                if (should_rebuild(out_res, file_mtime(f))) {
                    string cmd = resource_compiler + " -o '"+out_res+"' '"+f+"'";
                    if (!quiet) printf("%s\n", cmd.c_str());
                    int r = system(cmd.c_str());
                    if (r) {
                        //return r;
                        fprintf(stderr, HILITE "%s: " WARNING_STYLE "warning:" REGGS " resource compiler returned code %d (.res target)\n", f.c_str(), r);
                    } else {
                        res_files.push_back(out_res);
                    }
                    relink = true;
                }
            } else if (os == "windows") {
                string out_ob = bdir + flatten_filename(f) + ".rc.o";
                if (should_rebuild(out_ob, file_mtime(f))) {
                    string cmd = resource_compiler + " -o '"+out_ob+"' '"+f+"'";
                    if (!quiet) printf("%s\n", cmd.c_str());
                    int r = system(cmd.c_str());
                    if (r) {
                        //return r;
                        fprintf(stderr, HILITE "%s: " WARNING_STYLE "warning:" REGGS " resource compiler returned code %d (.o target)\n", f.c_str(), r);
                    } else {
                        obj_list += " '"+out_ob+"'";
                    }
                    relink = true;
                }
            }
        }
    }
    if (relink || (native && (file_mtime(output) < 0))) {
        if (!linker.size()) {
            fprintf(stderr, ERROR_STYLE "error:" REGGS " no linker found, please specify with /LD:{your-linker}\n");
            return 1;
        }
        string cmd = linker + " -o '"+output+"' "+obj_list+" "+ldflags + " " + user_ldflags;
        for(auto l: libs) cmd += " -l"+l;
        if (!quiet) printf("%s\n", cmd.c_str());
        int r = system(cmd.c_str());
        if (r) return r;
    }
    string out_fn = output_base + ".dll.h";
    string export_cs_file = output_base + ".dll.cs";
    if (!dll_mode) out_fn = gdir + out_fn;
    if (!dll_mode) export_cs_file = gdir + export_cs_file;
    string token = strip_filename(real_output);
    string token2 = token + "_dll";
    long exetime = file_mtime(output);
    long dllhtime = file_mtime(out_fn);
    long dllcstime = file_mtime(export_cs_file);
    bool stale = (dllhtime < 0) || (dllcstime < 0) || (dllhtime < exetime) || (dllcstime < exetime);
    if (stale && (dll_mode || cs_files.size())) {
        export_h = "#ifndef "+token2+"\n" + "#define "+token2+"\n" + export_h + "#endif\n";
        export_h = remove_empty_ifdefs(export_h);
        if (!write_file(out_fn, export_h)) {
            fprintf(stderr, HILITE "%s: " ERROR_STYLE "error:" REGGS " failed to write file %s\n", token.c_str(), out_fn.c_str());
            return 1;
        }
        
        export_cs = "using System;\nusing System.Runtime.InteropServices;\npublic static class "+token2+" {\n" + export_cs + "}\n";
        if (!write_file(export_cs_file, export_cs)) {
            fprintf(stderr, HILITE "%s: " ERROR_STYLE "error:" REGGS " failed to write file %s\n", token.c_str(), export_cs_file.c_str());
            return 1;
        }
    }
    string cs_list = export_cs_file;
    for(auto f: cs_files) {
        string out_cs = gdir + flatten_filename(f) + ".cs";
        cs_list += " '"+out_cs+"'";
        string code = "using static "+token2+";\n" + read_file(f);
        if (!write_file(out_cs, code)) {
            fprintf(stderr, HILITE "%s: " ERROR_STYLE "error:" REGGS " failed to write file %s\n", token.c_str(), out_cs.c_str());
            return 1;
        }
    }
    if (cs_files.size()) {
        string cs_exe = real_output;
        if (real_output.size() <= 4 || real_output.substr(real_output.size()-4) != ".exe") cs_exe += ".exe";

        long exetime = file_mtime(cs_exe);
        long srctime = file_mtime(export_cs_file);
        for(auto f: cs_files) {
            srctime = max(srctime, file_mtime(f));
        }
        if (exetime < 0 || exetime < srctime) {
            if (!cs_compiler.size() && !system("which mcs 2>/dev/null >/dev/null")) cs_compiler = "mcs";
            if (!cs_compiler.size() && !system("which csc 2>/dev/null >/dev/null")) cs_compiler = "csc";
            if (!cs_compiler.size()) {
                fprintf(stderr, ERROR_STYLE "error:" REGGS " no C# compiler found, please specify with /CS:{your-C#-compiler}\n");
                return 1;
            }
            string cmd = cs_compiler + " /unsafe";
            if (cs_version.size()) {
                cmd += " /langversion:"+cs_version;
            }
            cmd += " /out:'"+cs_exe+"' "+cs_list;
            if (cs_use_res) {
                cs_use_res = false;
                for(auto f: res_files) {
                    cmd += " /win32res:'"+f+"'";
                    cs_use_res = true;
                }
            }
            if (!cs_use_res && icon.size()) {
                cmd += " /win32icon:'"+icon+"'";
            }
            cmd += " " + user_csflags;
            if (!quiet) printf("%s\n", cmd.c_str());
            int r = system(cmd.c_str());
            if (r) return r;
        }
        /*
        cmd = "mkbundle -o '"+real_output+"' --simple '"+cs_exe+"' --library '"+token+"','"+output+"' --no-machine-config --no-config --static";
        if (!quiet) printf("%s\n", cmd.c_str());
        r = system(cmd.c_str());
        if (r) return r;
        mkbundle -c './build/windows-release/test.cs.exe' --no-machine-config -L /usr/lib/mono/4.5/ -o A.c
        mkbundle --deps --no-machine-config -oo B.o ./build/windows-release/test.cs.exe -L /usr/lib/mono/4.5
        */
    }
    return 0;
}
