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

#include "lobster/stdafx.h"
#include "lobster/disasm.h"  // Some shared bytecode utilities.
#include "lobster/compiler.h"
#include "lobster/tonative.h"

namespace lobster {

class CPPGenerator : public NativeGenerator {
    ostringstream &ss;
    const int dispatch = VM_DISPATCH_METHOD;
    int current_block_id = -1;

    string_view Block() {
        return dispatch == VM_DISPATCH_TRAMPOLINE ? "block" : "";
    }

    void JumpInsVar() {
        if (dispatch == VM_DISPATCH_TRAMPOLINE) {
            ss << "return (void *)vm.next_call_target;";
        } else if (dispatch == VM_DISPATCH_SWITCH_GOTO) {
            ss << "{ ip = vm.next_call_target; continue; }";
        }
    }

  public:

    explicit CPPGenerator(ostringstream &ss) : ss(ss) {}

    void FileStart() override {
        ss <<
            "#include \"lobster/stdafx.h\"\n"
            "#include \"lobster/vmdata.h\"\n"
            "#include \"lobster/engine.h\"\n"
            "\n"
            "#ifndef VM_COMPILED_CODE_MODE\n"
            "    #error VM_COMPILED_CODE_MODE must be set for the entire code base.\n"
            "#endif\n"
            "\n"
            "#ifdef _WIN32\n"
            "    #pragma warning (disable: 4102)  // Unused label.\n"
            "#endif\n"
            "\n";
    }

    void DeclareBlock(int id) override {
        if (dispatch == VM_DISPATCH_TRAMPOLINE) {
            ss << "static void *block" << id << "(lobster::VM &);\n";
        }
    }

    void BeforeBlocks(int start_id, string_view /*bytecode_buffer*/) override {
        ss << "\n";
        if (dispatch == VM_DISPATCH_SWITCH_GOTO) {
            ss << "static void *one_gigantic_function(lobster::VM &vm) {\n";
            ss << "  lobster::block_t ip = " << start_id;
            ss << ";\n  for(;;) switch(ip) {\n    default: assert(false); continue;\n";
        }
    }

    void FunStart(const bytecode::Function *f) override {
        ss << "\n";
        if (f) ss << "// " << f->name()->string_view() << "\n";
    }

    void BlockStart(int id) override {
        if (dispatch == VM_DISPATCH_TRAMPOLINE) {
            ss << "static void *block" << id << "(lobster::VM &vm) {\n";
        } else if (dispatch == VM_DISPATCH_SWITCH_GOTO) {
            ss << "  case " << id << ": block_label" << id << ":\n";
        }
    }

    void InstStart() override {
        ss << "    { ";
    }

    void EmitJump(int id) override {
        if (dispatch == VM_DISPATCH_TRAMPOLINE) {
            if (id <= current_block_id) {
                // A backwards jump, go via the trampoline to be safe
                // (just in-case the compiler doesn't optimize tail calls).
                ss << "return (void *)block" << id << ";";
            } else {
                // A forwards call, should be safe to tail-call.
                ss << "return block" << id << "(vm);";
            }
        } else if (dispatch == VM_DISPATCH_SWITCH_GOTO) {
            ss << "goto block_label" << id << ";";
        }
    }

    void EmitConditionalJump(int opc, int id) override {
        ss << "if (vm.F_" << ILNames()[opc] << "()) ";
        EmitJump(id);
    }

    void EmitOperands(const char * /*base*/, const int *args, int arity, bool is_vararg) override {
        if (is_vararg && arity) {
            ss << "static int args[] = {";
            for (int i = 0; i < arity; i++) {
                if (i) ss << ", ";
                ss << args[i];
            }
            ss << "}; ";
        }
    }

    void EmitMultiMethodDispatch(const vector<int> &mmtable) override {
        ss << "static lobster::block_t mmtable[] = {";
        for (auto &id : mmtable) {
            ss << Block() << id << ", ";
        }
        ss << "}; vm.next_mm_table = mmtable; ";
    }

    void SetNextCallTarget(int id) override {
        ss << "vm.next_call_target = " << Block() << id << "; ";
    }

    void EmitGenericInst(int opc, const int *args, int arity, bool is_vararg, int target) override {
        ss << "vm.U_" << ILNames()[opc] << "(";
        if (is_vararg) {
            ss << "args";
        } else {
            for (int i = 0; i < arity; i++) {
                if (i) ss << ", ";
                ss << args[i];
            }
        }
        if (target >= 0) {
            if (arity) ss << ", ";
            ss << Block() << target;
        }
        ss << ");";
    }

    void EmitCall(int id) override {
        ss << " ";
        EmitJump(id);
    }

    void EmitCallIndirect() override {
        ss << " ";
        JumpInsVar();
    }

    void EmitCallIndirectNull() override {
        ss << " if (vm.next_call_target) ";
        JumpInsVar();
    }

    void InstEnd() override {
        ss << " }\n";
    }

    void BlockEnd(int id, bool already_returned, bool is_exit) override {
        if (dispatch == VM_DISPATCH_TRAMPOLINE) {
            if (!already_returned) {
                ss << "    { ";
                if (is_exit) JumpInsVar(); else EmitJump(id);
                ss << " }\n";
            }
            ss << "}\n";
        }
    }

    void FileEnd(int start_id, string_view bytecode_buffer) override {
        if (dispatch == VM_DISPATCH_SWITCH_GOTO) {
            ss << "}\n}\n";  // End of gigantic function.
        }
        // FIXME: this obviously does NOT need to include the actual bytecode, just the metadata.
        // in fact, it be nice if those were in readable format in the generated code.
        ss << "\nstatic const int bytecodefb[] =\n{";
        auto bytecode_ints = (const int *)bytecode_buffer.data();
        for (size_t i = 0; i < bytecode_buffer.length() / sizeof(int); i++) {
            if ((i & 0xF) == 0) ss << "\n  ";
            ss << bytecode_ints[i] << ", ";
        }
        ss << "\n};\n\n";
        ss << "int main(int argc, char *argv[]){\n";
        ss << "    return EngineRunCompiledCodeMain(argc, argv, (void *)";
        if (dispatch == VM_DISPATCH_SWITCH_GOTO) {
            ss << "one_gigantic_function";
        } else if (dispatch == VM_DISPATCH_TRAMPOLINE) {
            ss << Block() << start_id;
        }
        ss << ", bytecodefb, " << bytecode_buffer.size() << ");\n}\n";
    }

    void Annotate(string_view comment) override {
        ss << " /* " << comment << " */";
    }
};

string ToCPP(NativeRegistry &natreg, ostringstream &ss, string_view bytecode_buffer) {
    CPPGenerator cppgen(ss);
    return ToNative(natreg, cppgen, bytecode_buffer);
}

}  // namespace lobster

