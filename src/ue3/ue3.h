#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Read-only access to UE3 reflection: GNames, GObjects, and the object/field layouts of this build.
// Every offset is discovered at runtime on live data (see Init), nothing is hardcoded per build.
namespace ue3 {

struct TArrayRaw {
  uintptr_t data;
  int32_t num;
  int32_t max;
};

struct FNameRaw {
  int32_t index;
  int32_t number;
};

// -1 = not found.
struct Layout {
  uintptr_t gnames = 0;    // address of TArray<FNameEntry*>
  uintptr_t gobjects = 0;  // address of TArray<UObject*>
  int nameEntryStr = -1;   // FNameEntry: offset of the characters
  int nameEntryIndex = -1; // FNameEntry: offset of the index field
  bool nameIndexShifted = false;  // index stored as (i << 1) | unicode
  bool nameWide = false;

  int objIndex = -1, objName = -1, objClass = -1, objOuter = -1;  // UObject
  int fieldNext = -1;                                              // UField
  int structSuper = -1, structChildren = -1, structSize = -1, structScript = -1;  // UStruct
  int propArrayDim = -1, propElementSize = -1, propFlags = -1, propOffset = -1;  // UProperty
  int propExtra = -1;  // first member of UProperty subclasses (bitmask / Struct / Inner / Enum / class)
  int enumNames = -1;  // UEnum: TArray<FName>
  int funcFlags = -1, funcNative = -1, funcFriendlyName = -1, funcNumParms = -1, funcParmsSize = -1,
      funcRetOffset = -1, funcFunc = -1;  // UFunction (funcFunc = native thunk pointer)
  int classDefaultObject = -1;  // UClass
};

const Layout& L();

// Blocking, idempotent, logs every step. background=true: cheap retry mode for the auto-init loop - no
// slow full-scan fallback, and a layout with missing parts counts as "not yet" (packages still loading).
bool Init(bool background = false);
// Starts the auto-init thread (once): retries Init every ~1 s from game start until GNames/GObjects are valid,
// logs "ue3: ready (...)" or, after a long wait, why it is still not ready.
void StartAutoInit();
void Reset();               // forget results so the next Init rescans
bool Ready();               // GNames + GObjects + UObject layout found
std::string StatusText();   // one line for the menu
std::string LayoutReport(); // multi-line, for log/dumps
std::string SignaturesReport();  // code references to GNames/GObjects as byte patterns

// --- names ---
int NumNames();
std::string Name(int index);
std::string NameOf(const FNameRaw& n);  // adds _N suffix for Number > 0
std::string ReadFName(uintptr_t addr);
int FindNameIndex(const char* s);  // -1 if absent

// --- objects ---
int NumObjects();
uintptr_t Object(int i);
bool IsObject(uintptr_t p);
int ObjNameIndex(uintptr_t o);
std::string ObjName(uintptr_t o);
uintptr_t ObjClass(uintptr_t o);
uintptr_t ObjOuter(uintptr_t o);
int ObjIndexOf(uintptr_t o);
std::string ClassNameOf(uintptr_t o);
std::string PathName(uintptr_t o);
std::string FullName(uintptr_t o);
// className/outerName may be null (= any).
uintptr_t FindObject(const char* className, const char* name, const char* outerName = nullptr);
bool IsChildOf(uintptr_t structObj, const char* baseName);  // walks SuperField
bool IsA(uintptr_t o, const char* className);

// --- fields ---
uintptr_t FieldNext(uintptr_t f);
uintptr_t StructSuper(uintptr_t s);
uintptr_t StructChildren(uintptr_t s);
int StructSize(uintptr_t s);
int PropArrayDim(uintptr_t p);
int PropElementSize(uintptr_t p);
uint64_t PropFlags(uintptr_t p);
int PropOffset(uintptr_t p);
uintptr_t PropExtra(uintptr_t p, int slot = 0);  // pointer-sized members after UProperty
uint32_t PropExtraU32(uintptr_t p);
std::vector<std::string> EnumNames(uintptr_t e);
uint32_t FuncFlags(uintptr_t f);
uint16_t FuncNative(uintptr_t f);
int FuncNumParms(uintptr_t f);
int FuncParmsSize(uintptr_t f);
uintptr_t ClassDefaultObject(uintptr_t cls);
bool StructScript(uintptr_t s, TArrayRaw& out);  // bytecode of a UStruct/UFunction
uintptr_t FuncNativePtr(uintptr_t f);             // UFunction::Func (native implementation)

// First match of "8B 0D ?? ..." in the exe's code sections (0 = none).
uintptr_t SigScan(const char* sig);
std::vector<uintptr_t> SigScanAll(const char* sig, size_t maxHits);  // every match, up to maxHits
// Signatures of the UE3 globals: the TArray address is read at match + addrOffset.
struct SigInfo {
  const char* name;
  const char* pattern;
  int addrOffset;
};
std::vector<SigInfo> Signatures();

}  // namespace ue3
