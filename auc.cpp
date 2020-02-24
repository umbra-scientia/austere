/* Copyright © 2020, Mykos Hudson-Crisp <micklionheart@gmail.com>
* All rights reserved. */

#include <stdio.h>
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

#define PLAT_WINDOWS 1
#define PLAT_LINUX 2
#define PLAT_APPLE 3

struct SourceFile;
static string prefix((char*)austere_h, austere_h_len);
static string winrc((char*)default_rc, default_rc_len);
static string compiler = "cc";
static string linker = "cc";
static string resource_compiler;
static string cflags = "-fvisibility=hidden -fPIC";
static string ldflags = "-fvisibility=hidden -fPIC";
static string release_flags = "-fomit-frame-pointer -ffast-math -flto=8 -fgraphite-identity -ftree-loop-distribution -floop-nest-optimize -march=amdfam10 -mtune=znver1 -Ofast -s";
static string debug_flags = "-fstrict-aliasing -ffast-math -flto=8 -g";
static string icon, manifest, details, vendor, product, version="0,0,0,0", copyright;
static vector<string> libs;
static bool human = false;
static vector<SourceFile*> files;
static vector<string> c_files, rc_files, dll_files, cpp_files, asm_files, rs_files, cs_files;
static string build_dir = "build/";
static string output = "";
#ifdef _WIN32
static string os = "windows";
#else
static string os = "linux";
#endif
static bool debug_mode = false;
static bool dll_mode = false;
static bool quiet = false;

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
    for(;;) {
        auto pos = text.find(find);
        if (pos == string::npos) break;
        text = text.substr(0, pos) + replace + text.substr(pos+find.size());
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

static void rewrite_structs(string& code, string& head, string& public_head, string& local_head, string& tail, string space, int* outputToHeader, int isPacked, int isPublic, int isOpaque, int isPrivate) {
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
    if (isPrivate) {
        local_head += "typedef struct " + tname + " " + tname + ";\n";
        code = "struct " + tname + " " + code;
        tail = space + "};";
    } else if (isOpaque) {
        *outputToHeader = 2;
        public_head += "typedef struct " + tname + " " + tname + ";\n";
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

static void rewrite_enums(string& code, string& head, string& public_head, string& local_head, string& tail, string space, int* outputToHeader, int isPublic, int isOpaque, int isPrivate) {
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
        head += "typedef int " + tname + ";\n";
        local_head += "typedef int " + tname + ";\n";
    } else if (isOpaque) {
        head += "typedef int " + tname + ";\n";
        public_head += "typedef int " + tname + ";\n";
        *outputToHeader = 3;
    } else if (isPublic) {
        *outputToHeader = 1;
    } else {
        *outputToHeader = 2;
    }
    code = "typedef enum {";
    tail = space + "} " + tname + ";";
}

static int extract_public_signatures(string& head, string& post_head, string& local_post_head, string& local_head, string& line, int isPacked, int isPublic) {
    string space, code;
    int trigger = 0;
    for(auto c: line) {
        if (!isspace(c)) trigger = 1;
        if (trigger) code += c;
        else space += c;
    }
    code = trim(code);
    bool isNotFunction = false;
    if (code.find("if ") == 0) isNotFunction = true;
    if (code.find("else ") == 0) isNotFunction = true;
    if (code.find("switch ") == 0) isNotFunction = true;
    if (code.find("for ") == 0) isNotFunction = true;
    if (code.find("while ") == 0) isNotFunction = true;
    if (code.find("typedef") == 0 && code.find(';') != string::npos) {
        head += line + "\n";
        local_head += line + "\n";
        return 1;
    }
    auto end = code.rfind(')');
    if (end != string::npos && code.find('{') != string::npos) {
        string hcode = code.substr(0, end+1) + ";\n";
        if (isPublic) {
            post_head += "DLLIMPORT " + hcode;
            local_post_head += "DLLEXPORT " + hcode;
            line = space + "DLLEXPORT " + code;
        } else if (!isNotFunction) {
            local_head += "static " + hcode;
        }
        return 0;
    }
    return 0;
}

static string read_symbol_backwards(string line, int pos) {
    string o;
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
    if (isCustom) var_type_table[realfunc] = "custom";
    string out = line.substr(0, sp+1) + realfunc + "(";
    if (!isStatic) {
        if (isConst) {
            out += "const ";
        }
        out += type + "*";
        if (!isHead) {
            out += " restrict this";
        }
        if (hasArgs) out += ", ";
    }
    line = line.substr(par+1);
    auto curly = line.find('{');
    bool isCtor = !isCustom && !isHead && (func == "ctor");
    if (isCtor && curly != string::npos) {
        out += line.substr(0, curly+1);
        out += "if (!this) this = malloc(sizeof("+type+")); ";
        out += line.substr(curly+1);
    } else {
        out += line;
    }
    return out;
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
    return fn.substr(x+1);
}

static string extract_dir(string fn) {
    auto x = fn.rfind('/');
    if (x == string::npos) x = fn.rfind('\\');
    if (x == string::npos) return ".";
    return fn.substr(0, x);
}

static string extract_filename(string fn) {
    auto x = fn.rfind('/');
    if (x == string::npos) x = fn.rfind('\\');
    if (x == string::npos) return fn;
    return fn.substr(x+1);
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
    return fn;
}

static string strip_file_ext(string fn) {
    auto dot = fn.rfind('.');
    if (dot != string::npos) fn = fn.substr(0, dot);
    return fn;
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
    string newcode = type + "_dtor";
    bool isCustom = (var_type_table[newcode] == "custom");
    newcode += "(";
    if (!isPtr) {
        newcode += "&";
    }
    newcode += name + ");";
    if (isPtr && !isCustom) {
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
    string newcode = type+"_ctor(0";
    auto hack = code.find(obuffer) + obuffer.size();
    while (isspace(code[hack])) hack++;
    if (code[hack] != ')') {
        newcode += ", ";
    }
    code = str_replace(code, obuffer, newcode);
    return "";
}

static string extract_variable_types(string& code, map<string, string>& var_type_table) {
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
                    }
                    buffer = "";
                    continue;
                } else {
                    valid = false;
                }
                buffer = "";
            }
            if (c == ';' || c == ',' || c == '(') {
                string obuffer = buffer + c;
                buffer = trim(buffer);
                if (valid_type_def(buffer)) {
                    auto pos = buffer.rfind(' ');
                    string type = buffer.substr(0, pos);
                    string name = buffer.substr(pos + 1);
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
        if (!t.size()) return "Unknown type for "+obj;
        while (isspace(code[r+1])) r++;
        bool isPtr = false;
        if (t[t.size()-1] == '*') {
            isPtr = true;
            t = t.substr(0, t.size()-1);
        }
        if (isPtr && type != "->") return "Variable \""+obj+"\" is a pointer, use -> for member calls.";
        if (!isPtr && type != ".") return "Variable \""+obj+"\" is not a pointer, use . for member calls.";
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
    string local_head, post_head, local_post_head, public_head;
    string template_class;
    vector<string> template_vars;
    vector<string> lines;
    map<string, string> var_type_table;
    int outputToHeader;
    bool valid;
    bool processed;
    SourceFile(const char* filename_) {
        valid = false;
        processed = false;
        filename = filename_;
        FILE*fp = fopen(filename_, "r");
        if (!fp) return;
        int line_no = 0;
        if (!human) body = "#line 1 \""+filename+"\"\n";
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
                line = trim(line.substr(9));
                auto lt = line.find('<');
                auto gt = line.find('>');
                if (lt == string::npos || gt == string::npos) {
                    fprintf(stderr, "[%s:%d] Invalid #template directive.\n", filename, line_no);
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
                    local_head += z + "\n";
                    head += z + "\n";
                    body += z + "\n";
                }
                continue;
            }
            if (code.find("#global_") == 0) {
                if (plat) {
                    string z = space + "#" + line.substr(line.find("#global_") + 8);
                    local_head += z + "\n";
                    head += z + "\n";
                    body += z + "\n";
                }
                continue;
            }
            if (code.find("#define") == 0 || code.find("#include") == 0) {
                if (plat) {
                    head += line + "\n";
                    body += line + "\n";
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
                local_head += line + "\n";
                head += line + "\n";
                body += line + "\n";
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
            int isPacked = 0;
            if (code.find("packed ") == 0) {
                isPacked = 1;
                code = code.substr(6);
                while (code.size() && isspace(code[0])) {
                    code = code.substr(1);
                }
            }
            auto memberHint = code.find("::");
            int isMember = (memberHint != string::npos);
            if (isMember) {
                string obj = read_symbol_backwards(code, memberHint-1);
                if (obj.size()) {
                    var_type_table["this"] = obj+"*";
                } else {
                    fprintf(stderr, "[%s:%d] Unable to extract symbol.\n", filename.c_str(), line_no);
                }
            }
            string hcode = resolve_member_functions(code, 1, isStatic, isConst, isCustom, var_type_table);
            extract_public_signatures(head, post_head, local_post_head, local_head, hcode, isPacked, isPublic);
            string err = extract_variable_types(code, var_type_table);
            if (err.size()) {
                fprintf(stderr, "[%s:%d] %s\n", filename.c_str(), line_no, err.c_str());
                return false;
            }
            rewrite_structs(code, head, public_head, local_head, tail, space, &outputToHeader, isPacked, isPublic, isOpaque, isPrivate);
            rewrite_enums(code, head, public_head, local_head, tail, space, &outputToHeader, isPublic, isOpaque, isPrivate);
            err = rewrite_member_calls(code, var_type_table);
            if (err.size()) {
                fprintf(stderr, "[%s:%d] %s\n", filename.c_str(), line_no, err.c_str());
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
                local_head += code;
            } else if (oth == 2) {
                head += code;
                local_head += code;
            } else if (oth == 3) {
                local_head += code;
            } else {
                if (!human) {
                    char buf[512];
                    memset(buf, 0, sizeof(buf));
                    snprintf(buf, sizeof(buf)-1, "#line %d \"%s\"\n", line_no, filename.c_str());
                    body += buf;
                }
                body += code;
                code = "";
            }
        }
        return true;
    }
};

int usage() {
    printf("usage: auc <input files>\n");
    printf("\t/OUT:<output-filename> (-o)\n");
    printf("\t/DEBUG (-g)\n");
    printf("\t/DIR:<build-directory> (-d)\n");
    printf("\t/OS:<operating-system> (-m) [linux, windows, win32]\n");
    printf("\t/DLL (-shared)\n");
    printf("\t/QUIET (-q)\n");
    printf("\t/HELP (-h)\n");
    printf("\t/PRETTY\n");
    return 0;
}

int help() {
    printf("usage: auc <input files>\n * Supported input types: .au, .c, .dll/.so/.o, .rc, .manifest, .ico\n   (Not implemented: .cpp, .asm, .rs)\n\n");
    printf("/OUT:<output-filename> (-o)\n\n");
    printf("/DEBUG (-g)\n\n");
    printf("/DIR:<build-directory> (-d)\n * Intermediate compile results (generated .o .c and .h files)\n\n");
    printf("/OS:<operating-system> (-m)\n * Any cross compiler toolchain (eg. 'x86_64-w64-mingw32')\n   or a preset: 'linux', 'windows' (aka 'win64'), 'win32'\n\n");
    printf("/DLL (-shared)\n * Produce a .dll file instead of an .exe file.\n   (WARNING: Writes a .h file with the same base name as the .dll file)\n\n");
    printf("/QUIET (-q)\n * Just execute the build commands without printing them first.\n\n");
    printf("/HELP (-h)\n * Show this help screen.\n\n");
    printf("/PRETTY\n * Generate pretty .c files from .au sources\n   (Ruins compiler and debugger messages, only meant as an escape hatch.) \n\n");
    printf("-I, -D, -L, -l\n * Passed through to the compiler or linker.\n\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc <= 1) return usage();
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
        } else if (last_flag == "-m" || last_flag == "/os") {
            if (!arg.size()) continue;
            os = argl;
            last_flag = "";
            continue;
        } else if (last_flag == "-o" || last_flag == "/out") {
            if (!arg.size()) continue;
            output = arg;
            last_flag = "";
            continue;
        } else if (last_flag == "--compiler" || last_flag == "/cc") {
            if (!arg.size()) continue;
            compiler = linker = arg;
            last_flag = "";
            continue;
        } else if (last_flag == "--linker" || last_flag == "/ld") {
            if (!arg.size()) continue;
            linker = arg;
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
        } else if (last_flag == "-q" || last_flag == "/quiet") {
            quiet = true;
            last_flag = "";
            continue;
        } else if (last_flag == "/pretty") {
            human = true;
            last_flag = "";
            continue;
        }
        if (!output.size()) {
            output = strip_filename(argv[i]);
        }
        string ext = extract_ext(argv[i]);
        if (ext == "c") c_files.push_back(argv[i]);
        if (ext == "rc") rc_files.push_back(argv[i]);
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
                fprintf(stderr, "[%s] Parse failed.\n", argv[i]);
                return 1;
            }
            files.push_back(f);
        }
    }
    if (os == "windows" || os == "win32" || os == "win64") {
        if (dll_mode) {
            if (output.size()<=4 || output.substr(output.size()-4) != ".dll") {
                output += ".dll";
            }
        } else {
            if (output.size()<=4 || output.substr(output.size()-4) != ".exe") {
                output += ".exe";
            }
        }
        if (os == "win32") {
            os = "i686";
        } else {
            os = "x86_64";
        }
        compiler = os + "-w64-mingw32-gcc";
        linker = os + "-w64-mingw32-gcc";
        resource_compiler = os + "-w64-mingw32-windres";
        ldflags += " -Wl,--subsystem,windows -mwindows";
        os = "windows";
    } else if (os == "linux") {
        if (dll_mode) {
            if (output.size()<=3 || output.substr(output.size()-3) != ".so") {
                output += ".so";
            }
            ldflags += " -Wl,-soname,'"+extract_filename(output)+"'";
        } else {
            if (output.size()<=4 || output.substr(output.size()-4) != ".elf") {
                output += ".elf";
            }
        }
    } else {
        string test = os + "-cc --help 2>/dev/null >/dev/null";
        if (!system(test.c_str())) {
            compiler = os + "-cc";
            linker = os + "-cc";
        } else {
            compiler = os + "-gcc";
            linker = os + "-gcc";
        }
    }
    if (compiler == "cc" && system("cc --help 2>/dev/null >/dev/null")) {
        if (!system("gcc --help 2>/dev/null >/dev/null")) compiler = linker = "gcc";
        else if (!system("clang --help 2>/dev/null >/dev/null")) compiler = linker = "clang";
        else {
            fprintf(stderr, "Failed to find a C compiler, please specify one with /CC:<c-compiler>\n");
            return 1;
        }
    }
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
                    fprintf(stderr, "Compile error in %s\n", f->filename.c_str());
                    return 1;
                }
                f->processed = true;
            }
        }
        if (!ok) break;
    }
    mkdir(build_dir.c_str(), 0777);
    string bdir = build_dir + os + (debug_mode ? "-debug" : "-release");
    mkdir(bdir.c_str(), 0777);
    bdir += "/";
    string include_list;
    string export_h;
    for(auto f: files) {
        if (f->template_class.size()) continue;
        string file_id = str_replace(str_replace(f->filename, ".", "_"), "/", "_");
        export_h += f->public_head + f->post_head;
        f->head = "#ifndef "+file_id+"\n" + "#define "+file_id+"\n" + prefix + f->head + f->post_head + "#endif\n";
        f->head = remove_empty_ifdefs(f->head);
        string out_hname = strip_filename(f->filename);
        string out_fn = bdir + out_hname + ".au.h";
        include_list += "#include \""+out_hname+".au.h\"\n";
        if (!write_file(out_fn, f->head)) {
            fprintf(stderr, "Failed to write %s\n", out_fn.c_str());
            return 1;
        }
    }
    if (debug_mode) {
        cflags += " "+debug_flags;
        ldflags += " "+debug_flags;
    } else {
        cflags += " "+release_flags;
        ldflags += " "+release_flags;
    }
    if (dll_mode) {
        ldflags += " -shared";
        cflags += " -DBUILD_DLL";
    } else {
        cflags += " -DBUILD_EXE";
    }

    string obj_list;
    bool relink = false;
    if (dll_files.size()) {
        ldflags += " -Wl,-rpath,.";
    }
    for(auto f: dll_files) {
        cflags += " -I'"+extract_dir(f)+"'";
        ldflags += " -Wl,-rpath,'"+extract_dir(f)+"'";
        obj_list += " "+f;
    }
    for(auto f: files) {
        if (f->template_class.size()) continue;
        string file_id = str_replace(str_replace(f->filename, ".", "_"), "/", "_");
        f->body = prefix + "#define "+file_id+"\n" + include_list + f->local_head + f->local_post_head + f->body;
        string out_base = bdir + strip_filename(f->filename);
        string out_fn = out_base + ".au.c";
        if (!write_file(out_fn, f->body)) {
            fprintf(stderr, "Failed to write %s\n", out_fn.c_str());
            return 1;
        }
        string out_ob = out_base + ".au.o";
        string srcdir = extract_dir(f->filename);
        string cmd = compiler + " -c -o '"+out_ob+"' '"+out_fn+"' -I'" + srcdir + "' " + cflags;
        obj_list += " '"+out_ob+"'";
        if (should_rebuild(out_ob, file_mtime(f->filename))) {
            if (!quiet) printf("%s\n", cmd.c_str());
            int r = system(cmd.c_str());
            if (r) return r;
            relink = true;
        }
    }
    for(auto f: c_files) {
        string file_id = str_replace(str_replace(f, ".", "_"), "/", "_");
        string out_ob = bdir + strip_filename(f) + ".c.o";
        string cmd = compiler + " -c -o '"+out_ob+"' '"+f+"' " + cflags;
        obj_list += " '"+out_ob+"'";
        if (should_rebuild(out_ob, file_mtime(f))) {
            if (!quiet) printf("%s\n", cmd.c_str());
            int r = system(cmd.c_str());
            if (r) return r;
            relink = true;
        }
    }
    if (resource_compiler.size()) {
        if (!rc_files.size()) {
            string rc = winrc;
            rc = str_replace(rc, "$ICON", icon.size() ? ("APPICON ICON "+icon) : "");
            rc = str_replace(rc, "$MANIFEST", manifest.size() ? ("1 24 "+manifest) : "");
            rc = str_replace(rc, "$DETAILS", details);
            rc = str_replace(rc, "$VENDOR", vendor);
            rc = str_replace(rc, "$PRODUCT", product);
            rc = str_replace(rc, "$VERSION", version);
            rc = str_replace(rc, "$COPYRIGHT", copyright);
            rc = str_replace(rc, "$SONAME", extract_filename(output));
            string out_fn = bdir + "default.rc";
            if (read_file(out_fn) != rc) {
                if (!write_file(out_fn, rc)) {
                    fprintf(stderr, "Failed to write %s\n", out_fn.c_str());
                    return 1;
                }
            }
            rc_files.push_back(out_fn);
        }
        for(auto f: rc_files) {
            string file_id = str_replace(str_replace(f, ".", "_"), "/", "_");
            string out_ob = bdir + strip_filename(f) + ".rc.o";
            string cmd = resource_compiler + " -o '"+out_ob+"' '"+f+"'";
            obj_list += " '"+out_ob+"'";
            if (should_rebuild(out_ob, file_mtime(f))) {
                if (!quiet) printf("%s\n", cmd.c_str());
                int r = system(cmd.c_str());
                if (r) return r;
                relink = true;
            }
        }
    }
    if (relink || file_mtime(output) < 0) {
        string cmd = linker + " -o '"+output+"' "+obj_list+" "+ldflags;
        for(auto l: libs) cmd += " -l"+l;
        if (!quiet) printf("%s\n", cmd.c_str());
        int r = system(cmd.c_str());
        if (r) return r;
    }
    if (dll_mode) {
        string file_token = strip_filename(output) + "_dll";
        string out_fn = strip_file_ext(output) + ".h";
        export_h = "#ifndef "+file_token+"\n" + "#define "+file_token+"\n" + export_h + "#endif\n";
        export_h = remove_empty_ifdefs(export_h);
        if (!write_file(out_fn, export_h)) {
            fprintf(stderr, "Failed to write %s\n", out_fn.c_str());
            return 1;
        }
    }
    return 0;
}
