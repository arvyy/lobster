// Copyright 2014 Wouter van Oortmerssen. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// lobster.cpp : Defines the entry point for the console application.
//
#include "stdafx.h"

#include "vmdata.h"
#include "natreg.h"

#include "vm.h"

namespace lobster
{

static SlabAlloc *parserpool = nullptr;    // set during the lifetime of a Parser object

}

#include "ttypes.h"
#include "lex.h"
#include "idents.h"
#include "node.h"
#include "parser.h"
#include "typecheck.h"
#include "optimizer.h"
#include "codegen.h"
#include "wentropy.h"

#include "tocpp.h"

namespace lobster
{

NativeRegistry natreg;
const Type g_type_int(V_INT);                           TypeRef type_int = &g_type_int;
const Type g_type_float(V_FLOAT);                       TypeRef type_float = &g_type_float;
const Type g_type_string(V_STRING);                     TypeRef type_string = &g_type_string;
const Type g_type_any(V_ANY);                           TypeRef type_any = &g_type_any;
const Type g_type_vector_any(V_VECTOR, &*type_any);     TypeRef type_vector_any = &g_type_vector_any;
const Type g_type_vector_int(V_VECTOR, &*type_int);     TypeRef type_vector_int = &g_type_vector_int;
const Type g_type_vector_float(V_VECTOR, &*type_float); TypeRef type_vector_float = &g_type_vector_float;
const Type g_type_function_null(V_FUNCTION);            TypeRef type_function_null = &g_type_function_null;
const Type g_type_function_cocl(V_YIELD);               TypeRef type_function_cocl = &g_type_function_cocl;
const Type g_type_coroutine(V_COROUTINE);               TypeRef type_coroutine = &g_type_coroutine;
const Type g_type_typeid(V_TYPEID);                     TypeRef type_typeid = &g_type_typeid;
const Type g_type_function_nil(V_NIL,
                                &*type_function_null);  TypeRef type_function_nil = &g_type_function_nil;

const char *fileheader = "\xA5\x74\xEF\x19";
const int fileheaderlen = 4;

void Compile(const char *fn, char *stringsource, vector<uchar> &bytecode, string *parsedump = nullptr)
{
    SymbolTable st;

    Parser parser(fn, st, stringsource);
    parser.Parse();

    TypeChecker tc(parser, st);
    
    // Optimizer is not optional, must always run at least one pass, since TypeChecker and CodeGen rely
    // on it culling const if-thens and other things.
    Optimizer opt(parser, st, tc, 100);

    if (parsedump) *parsedump = parser.DumpAll();

    CodeGen cg(parser, st);

    st.Serialize(cg.code, cg.code_attr, cg.type_table, cg.vint_typeoffsets, cg.vfloat_typeoffsets, cg.lineinfo, cg.sids,
                 cg.stringtable, bytecode);

    //parserpool->printstats();
}

bool VerifyBytecode(const vector<uchar> &bytecode)
{
    flatbuffers::Verifier verifier(bytecode.data(), bytecode.size());
    auto ok = bytecode::VerifyBytecodeFileBuffer(verifier);
    assert(ok);
    return ok;
}

void SaveByteCode(const char *bcf, const vector<uchar> &bytecode)
{
    vector<uchar> out;
    WEntropyCoder<true>(bytecode.data(), bytecode.size(), bytecode.size(), out);

    FILE *f = OpenForWriting(bcf, true);
    if (f)
    {
        fwrite(fileheader, fileheaderlen, 1, f);
        auto len = (uint)bytecode.size();
        fwrite(&len, sizeof(uint), 1, f);  // FIXME: not endianness-safe
        fwrite(out.data(), out.size(), 1, f);
        fclose(f);
    }
}

bool LoadByteCode(const char *bcf, vector<uchar> &bytecode)
{
    size_t bclen = 0;
    uchar *bc = LoadFile(bcf, &bclen);
    if (!bc) return false;

    if (memcmp(fileheader, bc, fileheaderlen)) { free(bc); throw string("bytecode file corrupt: ") + bcf; }
    uint origlen = *(uint *)(bc + fileheaderlen);
    bytecode.clear();
    WEntropyCoder<false>(bc + fileheaderlen + sizeof(uint), bclen - fileheaderlen - sizeof(uint), origlen, bytecode);

    free(bc);

    return VerifyBytecode(bytecode);
}

void RegisterBuiltin(const char *name, void (* regfun)())
{
    Output(OUTPUT_DEBUG, "subsystem: %s", name);
    natreg.NativeSubSystemStart(name);
    regfun();
}

void DumpBuiltins(bool justnames)
{
    if (justnames) {
        FILE *f = OpenForWriting("builtin_functions_names.txt", false);
        if (f)
        {
            for (auto nf : natreg.nfuns) fprintf(f, "%s ", nf->name.c_str());
        }
        fclose(f);
        return;
    }

    FILE *f = OpenForWriting("builtin_functions_reference.html", false);
    if (f)
    {
        fprintf(f, "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 3.2 Final//EN\">\n");
        fprintf(f, "<html>\n<head>\n<title>lobster builtin function reference</title>\n");
        fprintf(f, "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\" />\n");
        fprintf(f, "<style type=\"text/css\">table.a, tr.a, td.a {font-size: 10pt;border: 1pt solid #DDDDDD;");
        fprintf(f, " border-Collapse: collapse; max-width:1200px}</style>\n");
        fprintf(f, "</head>\n<body><center><table border=0><tr><td>\n<p>lobster builtin functions:");
        fprintf(f, "(file auto generated by compiler, do not modify)</p>\n\n");
        int cursubsystem = -1;
        bool tablestarted = false;
        for (auto nf : natreg.nfuns)
        {
            if (nf->subsystemid != cursubsystem)
            {
                if (tablestarted) fprintf(f, "</table>\n");
                tablestarted = false;
                fprintf(f, "<h3>%s</h3>\n", natreg.subsystems[nf->subsystemid].c_str());
                cursubsystem = nf->subsystemid;
            }

            if (!tablestarted)
            {
                fprintf(f, "<table class=\"a\" border=1 cellspacing=0 cellpadding=4>\n");
                tablestarted = true;
            }

            fprintf(f, "<tr class=\"a\" valign=top><td class=\"a\"><tt><b>%s</b>(", nf->name.c_str());
            int i = 0;
            for (auto &a : nf->args.v)
            {
                fprintf(f, "%s%s%s<font color=\"#666666\">%s</font>%s",
                    a.type->t == V_NIL ? " [" : "",
                    i ? ", " : "",
                    nf->args.GetName(i).c_str(),
                    a.type->t == V_ANY ? "" : (string(":") + 
                        TypeName(a.type->t == V_NIL ? a.type->Element() : a.type)).c_str(),
                    a.type->t == V_NIL ? "]" : ""
                );
                i++;
            }
            fprintf(f, ")");
            if (nf->retvals.v.size())
            {
                fprintf(f, " -> ");
                size_t i = 0;
                for (auto &a : nf->retvals.v)
                {
                    fprintf(f, "<font color=\"#666666\">%s</font>%s",
                                TypeName(a.type).c_str(), 
                                i++ < nf->retvals.v.size() - 1 ? ", " : "");
                }
            }
            fprintf(f, "</tt></td><td class=\"a\">%s</td></tr>\n", nf->help);
        }
        fprintf(f, "</table>\n</td></tr></table></center></body>\n</html>\n");
        fclose(f);
    }
}

Value CompileRun(Value &source, bool stringiscode)
{
    string fn = stringiscode ? "string" : source.sval()->str();  // fixme: datadir + sanitize?
    SlabAlloc *parentpool = vmpool; vmpool = nullptr;
    VM        *parentvm   = g_vm;   g_vm = nullptr;
    try
    {
        vector<uchar> bytecode;
        Compile(fn.c_str(), stringiscode ? source.sval()->str() : nullptr, bytecode);
        //string s; DisAsm(s, bytecode.data()); Output(OUTPUT_INFO, "%s", s.c_str());
        RunBytecode(fn.c_str(), std::move(bytecode), nullptr, nullptr);
        auto ret = g_vm->evalret;
        delete g_vm;
        assert(!vmpool && !g_vm);
        vmpool = parentpool;
        g_vm = parentvm;
        source.DECRT();
        g_vm->Push(Value(g_vm->NewString(ret)));
        return Value();
    }
    catch (string &s)
    {
        if (g_vm) delete g_vm;
        vmpool = parentpool;
        g_vm = parentvm;
        source.DECRT();
        g_vm->Push(Value());
        return Value(g_vm->NewString(s));
    }
}

void AddCompiler()  // it knows how to call itself!
{
    STARTDECL(compile_run_code) (Value &filename)
    {
        return CompileRun(filename, true);
    }
    ENDDECL1(compile_run_code, "code", "S", "SS?",
        "compiles and runs lobster source, sandboxed from the current program (in its own VM)."
        " the argument is a string of code. returns the return value of the program as a string,"
        " with an error string as second return value, or nil if none. using parse_data(),"
        " two program can communicate more complex data structures even if they don't have the same version"
        " of struct definitions.");

    STARTDECL(compile_run_file) (Value &filename)
    {
        return CompileRun(filename, false);
    }
    ENDDECL1(compile_run_file, "filename", "S", "SS?",
        "same as compile_run_code(), only now you pass a filename.");
}

void RegisterCoreLanguageBuiltins()
{
    extern void AddBuiltins(); RegisterBuiltin("builtin",   AddBuiltins);
    extern void AddCompiler(); RegisterBuiltin("compiler",  AddCompiler);
    extern void AddFile();     RegisterBuiltin("file",      AddFile);
    extern void AddReader();   RegisterBuiltin("parsedata", AddReader);
}

}