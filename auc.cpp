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

using namespace std;

#define PLAT_WINDOWS 1
#define PLAT_LINUX 2

#include "prefix_h.h"
string compiler = "gcc";
string linker = "gcc";
string cflags = "-fvisibility=hidden -fPIC";
string ldflags = "-fvisibility=hidden -fPIC";
string release_flags = "-fomit-frame-pointer -ffast-math -flto=8 -fgraphite-identity -ftree-loop-distribution -floop-nest-optimize -march=amdfam10 -mtune=znver1 -Ofast -s";
string debug_flags = "-fstrict-aliasing -ffast-math -flto=8 -g";

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

static void rewrite_structs(string& code, string& head, string& local_head, string& tail, string space, bool* outputToHeader, int isPacked, int isPublic, int isOpaque) {
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
    if (isOpaque) {
        local_head += "typedef struct " + tname + " " + tname + ";\n";
        head += "typedef struct " + tname + " " + tname + ";\n";
        code = "struct " + tname + " " + code;
        tail = space + "};";
    } else if (isPublic) {
        *outputToHeader = true;
        code = "typedef struct " + tname + " {";
        tail = space + "} " + tname + ";";
    } else {
        local_head += "typedef struct " + tname + " " + tname + ";\n";
        code = "struct " + tname + " {";
        tail = space + "};";
    }
    if (isPacked) {
        code = "#pragma pack(push, 1)\n" + space + code;
        tail += "\n" + space + "#pragma pack(pop)";
    }
}

static void rewrite_enums(string& code, string& head, string& local_head, string& tail, string space, bool* outputToHeader, int isPublic, int isOpaque) {
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
    if (isOpaque) {
        head += "typedef int " + tname + ";\n";
        local_head += "typedef int " + tname + ";\n";
    } else if (isPublic) {
        *outputToHeader = true;
    }
    code = "typedef enum {";
    tail = space + "} " + tname + ";";
}

static int extract_public_signatures(string& head, string& local_head, string line, int isPacked, int isPublic) {
    string space, code;
    int trigger = 0;
    for(auto c: line) {
        if (!isspace(c)) trigger = 1;
        if (trigger) code += c;
        else space += c;
    }
    if (code.find("typedef") == 0 && code.find(';') != string::npos) {
        head += line + "\n";
        local_head += line + "\n";
        return 1;
    }
    auto end = code.rfind(')');
    if (end != string::npos && code.find('{') != string::npos) {
        string hcode = code.substr(0, end+1) + ";\n";
        if (isPublic) {
            head += "DLLIMPORT " + hcode;
            local_head += "DLLEXPORT " + hcode;
        } else {
            head += hcode;
            local_head += hcode;
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

static string resolve_member_functions(string line, int isHead, int isStatic, int isConst) {
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
    string out = line.substr(0, sp+1) + type + "_" + line.substr(col+2, par-col-2) + "(";
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
    out += line.substr(par+1);
    return out;
}

static bool write_file(string filename, string data) {
    FILE* fp = fopen(filename.c_str(), "wb");
    if (!fp) return false;
    fwrite(data.data(), 1, data.size(), fp);
    fclose(fp);
    return true;
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

static void extract_variable_types(string code, map<string, string>& var_type_table) {
    string buffer;
    code = trim(code);
    bool valid = true;
    bool hasCurly = (code.find('{') != string::npos);
    bool skip_whitespace = true;
    for(auto c: code) {
        if (skip_whitespace && isspace(c)) continue;
        skip_whitespace = false;
        if (c == ';' || c == '=' || c == ',' || c == ')') {
            buffer = trim(buffer);
            if (valid && valid_type_def(buffer)) {
                auto pos = buffer.rfind(' ');
                string type = buffer.substr(0, pos);
                string name = buffer.substr(pos + 1);
                if (type != "return") {
                    type = trim_type(type);
                    var_type_table[name] = type;
                }
            } else {
                valid = false;
            }
        }
        if (c == ';' || c == ',' || c == '(') {
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
        if (!t.size()) return "[%s:%d] Unknown type for "+obj+"\n";
        while (isspace(code[r+1])) r++;
        bool isPtr = false;
        if (t[t.size()-1] == '*') {
            isPtr = true;
            t = t.substr(0, t.size()-1);
        }
        if (isPtr && type != "->") return "[%s:%d] Variable \""+obj+"\" is a pointer, use -> for member calls.\n";
        if (!isPtr && type != ".") return "[%s:%d] Variable \""+obj+"\" is not a pointer, use . for member calls.\n";
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
    string local_head;
    string template_class;
    vector<string> template_vars;
    vector<string> lines;
    map<string, string> var_type_table;
    bool outputToHeader;
    bool valid;
    bool processed;
    SourceFile(const char* filename_) {
        valid = false;
        processed = false;
        filename = filename_;
        FILE*fp = fopen(filename_, "r");
        if (!fp) return;
        int line_no = 0;
        body = "#line 1 \""+filename+"\"\n";
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
            if (trim(line).find("#template") == 0) {
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
        outputToHeader = false;
        int line_no = 0;
        int ifdef_depth = 0;
        int platform = 0;
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
            if (code.find("#if") == 0 || code.find("#endif") == 0 || code.find("#else") == 0 || code.find("#elif") == 0) {
                if (code == "#ifdef OS_WINDOWS") {
                    platform = PLAT_WINDOWS;
                    ifdef_depth++;
                } else if (code == "#ifdef OS_LINUX") {
                    platform = PLAT_LINUX;
                    ifdef_depth++;
                } else if (code == "#else") {
                    if (ifdef_depth == 1) {
                        if (platform == PLAT_WINDOWS) {
                            platform = PLAT_LINUX;
                        } else if (platform == PLAT_LINUX) {
                            platform = PLAT_WINDOWS;
                        }
                    }
                } else {
                    if (code.find("#if") == 0) ifdef_depth++;
                    if (code.find("#endif") == 0) {
                        if (ifdef_depth <= 1) {
                            platform = 0;
                        }
                        ifdef_depth--;
                    }
                }
                head += line + "\n";
                body += line + "\n";
                continue;
            }
            int isConst = 0;
            if (code.find("const ") == 0) {
                isConst = 1;
                code = code.substr(5);
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
            if (isPublic) {
                string hcode = resolve_member_functions(code, 1, isStatic, isConst);
                extract_public_signatures(head, local_head, hcode, isPacked, isPublic);
            }
            extract_variable_types(code, var_type_table);
            rewrite_structs(code, head, local_head, tail, space, &outputToHeader, isPacked, isPublic, isOpaque);
            rewrite_enums(code, head, local_head, tail, space, &outputToHeader, isPublic, isOpaque);
            string err = rewrite_member_calls(code, var_type_table);
            if (err.size()) {
                fprintf(stderr, err.c_str(), filename.c_str(), line_no);
                return false;
            }
            if (isMember) {
                code = resolve_member_functions(code, 0, isStatic, isConst);
            }
            if (isStatic && !isMember) code = "static " + code;
            if (isConst && !isMember) code = "const " + code;
            int oth = outputToHeader;
            if (tail.size() && (code == "}" || code == "};")) {
                code = tail;
                tail = "";
                outputToHeader = false;
            }
            code = space + code + "\n";
            if (oth) {
                head += code;
                local_head += code;
            } else {
                char buf[512];
                memset(buf, 0, sizeof(buf));
                snprintf(buf, sizeof(buf)-1, "#line %d \"%s\"\n", line_no, filename.c_str());
                body += buf;

                body += code;
                code = "";
            }
        }
        return true;
    }
};

int main(int argc, char** argv) {
    vector<SourceFile*> files;
    string build_dir = "build/";
    string output = "";
#ifdef _WIN32
    string os = "windows";
#else
    string os = "linux";
#endif
    bool debug_mode = false;
    bool dll_mode = false;
    string last_flag;
    for(int i=1;i<argc;i++) {
        string arg(argv[i]);
        if (!arg.size()) continue;
        if (arg[0] == '-' || arg[0] == '/') {
            last_flag = argv[i];
            if (last_flag[0] == '/') {
                auto co = last_flag.find(':');
                if (co != string::npos) {
                    arg = last_flag.substr(co+1);
                    last_flag = last_flag.substr(0, co);
                }
            }
            if (last_flag[0] == '-') {
                auto co = last_flag.find('=');
                if (co != string::npos) {
                    arg = last_flag.substr(co+1);
                    last_flag = last_flag.substr(0, co);
                }
            }
        }
        if (last_flag == "-b" || last_flag == "--build-dir") {
            build_dir = arg;
            last_flag = "";
            continue;
        } else if (last_flag == "-m" || last_flag == "/os") {
            os = lowercase(arg);
            last_flag = "";
            continue;
        } else if (last_flag == "-o" || last_flag == "/out") {
            output = arg;
            last_flag = "";
            continue;
        }
        last_flag = "";
        if (arg == "-g" || arg == "/debug") {
            debug_mode = true;
            continue;
        } else if (arg == "-shared" || arg == "/dll") {
            dll_mode = true;
            continue;
        }
        if (!output.size()) {
            output = strip_filename(argv[i]);
        }
        auto f = new SourceFile(argv[i]);
        if (!f->valid) {
            fprintf(stderr, "[%s] Parse failed.\n", argv[i]);
            return 1;
        }
        files.push_back(f);
    }
    if (os == "windows" || os == "win32" || os == "win64") {
        if (dll_mode) {
            if (output.substr(output.size()-4) != ".dll") {
                output += ".dll";
            }
        } else {
            if (output.substr(output.size()-4) != ".exe") {
                output += ".exe";
            }
        }
        if (os == "win32") {
            compiler = "i686-w64-mingw32-" + compiler;
            linker = "i686-w64-mingw32-" + linker;
        } else {
            compiler = "x86_64-w64-mingw32-" + compiler;
            linker = "x86_64-w64-mingw32-" + linker;
        }
        ldflags += " -Wl,--subsystem,windows -mwindows";
    } else if (os == "linux") {
        if (dll_mode && output.substr(output.size()-3) != ".so") {
            output += ".so";
        }
        ldflags += " -Wl,-soname,"+output;
    } else {
        compiler = os + "-" + compiler;
        linker = os + "-" + linker;
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
    string include_list;
    string export_h;
    for(auto f: files) {
        if (f->template_class.size()) continue;
        string file_id = str_replace(str_replace(f->filename, ".", "_"), "/", "_");
        export_h += f->head;
        f->head = "#ifndef "+file_id+"\n" + "#define "+file_id+"\n" + string(prefix_h) + f->head + "#endif\n";
        string out_hname = strip_filename(f->filename);
        string out_fn = build_dir + out_hname + ".au.h";
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
    }

    string obj_list;
    bool relink = false;
    for(auto f: files) {
        if (f->template_class.size()) continue;
        string file_id = str_replace(str_replace(f->filename, ".", "_"), "/", "_");
        f->body = string(prefix_h) + "#define "+file_id+"\n" + include_list + f->local_head + f->body;
        string out_base = build_dir + strip_filename(f->filename);
        string out_fn = out_base + ".au.c";
        if (!write_file(out_fn, f->body)) {
            fprintf(stderr, "Failed to write %s\n", out_fn.c_str());
            return 1;
        }
        string out_ob = out_base + ".au.o";
        string cmd = compiler + " -c -o " + out_ob + " " + out_fn + " " + cflags;
        if (obj_list.size()) obj_list += " ";
        obj_list += out_ob;
        if (should_rebuild(out_ob, file_mtime(out_fn))) {
            printf("%s\n", cmd.c_str());
            system(cmd.c_str());
            relink = true;
        }
    }
    if (relink || file_mtime(output) < 0) {
        string cmd = linker + " -o " + output + " " + obj_list + " " + ldflags;
        printf("%s\n", cmd.c_str());
        system(cmd.c_str());
    }
    if (dll_mode) {
        string file_token = strip_filename(output) + "_dll";
        string out_fn = strip_file_ext(output) + ".h";
        export_h = "#ifndef "+file_token+"\n" + "#define "+file_token+"\n" + string(prefix_h) + export_h + "#endif\n";
        if (!write_file(out_fn, export_h)) {
            fprintf(stderr, "Failed to write %s\n", out_fn.c_str());
            return 1;
        }
    }
    return 0;
}
