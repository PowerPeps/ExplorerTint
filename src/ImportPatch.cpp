#include "ImportPatch.h"
#include <cstring>

static void PatchSlot(ULONG_PTR* slot, void* hook) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) return;
    *slot = (ULONG_PTR)hook;
    VirtualProtect(slot, sizeof(*slot), old, &old);
}

static bool Matches(BYTE* base, IMAGE_THUNK_DATA* thunk, const char* name) {
    bool byOrdinal = IMAGE_SNAP_BY_ORDINAL(thunk->u1.Ordinal);
    if (IS_INTRESOURCE(name))
        return byOrdinal && IMAGE_ORDINAL(thunk->u1.Ordinal) == (ULONG_PTR)name;
    if (byOrdinal) return false;
    auto byName = (IMAGE_IMPORT_BY_NAME*)(base + thunk->u1.AddressOfData);
    return strcmp((const char*)byName->Name, name) == 0;
}

static int PatchThunks(BYTE* base, IMAGE_THUNK_DATA* names, IMAGE_THUNK_DATA* slots, const char* name, void* hook) {
    int count = 0;
    for (; names->u1.AddressOfData; names++, slots++) {
        if (!Matches(base, names, name)) continue;
        PatchSlot(&slots->u1.Function, hook);
        count++;
    }
    return count;
}

int PatchImports(HMODULE mod, const char* dll, const char* name, void* hook) {
    auto base = (BYTE*)mod;
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    int count = 0;

    auto& imp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imp.VirtualAddress) {
        for (auto d = (IMAGE_IMPORT_DESCRIPTOR*)(base + imp.VirtualAddress); d->Name; d++) {
            if (!d->OriginalFirstThunk) continue;
            if (dll && _stricmp((const char*)(base + d->Name), dll) != 0) continue;
            count += PatchThunks(base, (IMAGE_THUNK_DATA*)(base + d->OriginalFirstThunk),
                                 (IMAGE_THUNK_DATA*)(base + d->FirstThunk), name, hook);
        }
    }

    auto& dly = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (dly.VirtualAddress) {
        for (auto d = (IMAGE_DELAYLOAD_DESCRIPTOR*)(base + dly.VirtualAddress); d->DllNameRVA; d++) {
            if (!d->Attributes.RvaBased) continue;
            if (dll && _stricmp((const char*)(base + d->DllNameRVA), dll) != 0) continue;
            count += PatchThunks(base, (IMAGE_THUNK_DATA*)(base + d->ImportNameTableRVA),
                                 (IMAGE_THUNK_DATA*)(base + d->ImportAddressTableRVA), name, hook);
        }
    }
    return count;
}
