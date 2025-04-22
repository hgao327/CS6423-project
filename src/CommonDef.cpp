#include "CommonDef.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/ADT/StringRef.h"
#include <cstdlib>

using namespace std;
using namespace llvm;

const char *ACCESS_KEY = "array-access";

static ItaniumPartialDemangler Demangler;

static string Blacklists[] = {
  "std"s,
  "__gnu_cxx"s
};

static bool IsVerbose = false;

__attribute__((constructor)) void libInit() {
  char *EnvVal = getenv("VERBOSE");
  try {
    IsVerbose = EnvVal && std::stoi(EnvVal) > 0 ? true : false;
  } catch (exception &E) {
    IsVerbose = false;
  }
}

bool isCxxSTLFunc(StringRef FuncName) {
  bool IsSTL = false;
  string Res = demangle(FuncName.data());
  StringRef DemangledName(Res);
  size_t FirstSRO = DemangledName.find("::");
  if (FirstSRO != StringRef::npos) {
    StringRef Prefix = DemangledName.substr(0, FirstSRO);
    for (string &Elem : Blacklists) {
      if (Prefix.endswith(Elem)) {
        IsSTL = true;
        break;
      }
    }
  }
  return IsSTL;
}

bool isCProgram(Module *M) {
  if (StringRef(M->getSourceFileName()).endswith(".c")) {
    return true;
  } else {
    return false;
  }
}

raw_ostream &verboseOut() {
  return IsVerbose ? errs() : nulls();
}