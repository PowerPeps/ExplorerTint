#pragma once
#include "Common.h"
#include <vector>

// Reader and patcher for the compiled ribbon markup (UIFILE resource, "SCBin" format).
namespace RibbonPatch {

struct Stats {
    size_t size = 0;
    size_t nodes = 0;
    size_t collections = 0;
    size_t pointers = 0;    // 0x3E references to supplementary blocks
    size_t blocks = 0;
    size_t entries = 0;     // id -> node index
    size_t tabs = 0;        // main tabs
};

struct NewTab {
    WORD sourceTab = 0x1003;            // cmdTabView: cloned for its application modes
    WORD sourceGroup = 0x2024;          // cmdChunkShowHide: three checkboxes and a button
    WORD tabId = 0x7E00;
    WORD groupId = 0x7E01;
    std::vector<WORD> controlIds = { 0x7E02, 0x7E03, 0x7E04, 0x7E05 };  // in source control order
};

bool Validate(const BYTE* data, size_t size, Stats* stats, std::string* error);
bool RoundTrip(const BYTE* data, size_t size, std::vector<BYTE>& out, std::string* error);
bool AddTab(const BYTE* data, size_t size, const NewTab& tab, std::vector<BYTE>& out, std::string* error);

}
