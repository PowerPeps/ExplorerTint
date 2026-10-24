#include "RibbonPatch.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

namespace RibbonPatch {
namespace {

enum : BYTE { TagProp = 0x01, TagNode = 0x16, TagColl = 0x18, TagRef = 0x3B, TagPtr = 0x3E };

const BYTE kMagic[14] = { 0x00, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 'S', 'C', 'B', 'i', 'n' };

struct Element {
    BYTE tag = 0;
    WORD type = 0;                  // node object type, or collection type
    WORD fix = 0;                   // node second word, or collection flag byte
    std::vector<BYTE> raw;          // prop and ref bytes after the tag
    size_t block = 0;               // pointer: index of the target block
    DWORD key = 0;                  // node identity; pointer: target address while parsing
    std::vector<Element> children;
};

struct Entry {
    WORD id;
    WORD sub;
    DWORD key;
};

struct Model {
    std::vector<BYTE> prelude;      // bytes between the size field and the tree
    Element root;
    std::vector<Element> blocks;
    std::vector<Entry> entries;
    std::set<DWORD> ids;
    DWORD nextKey = 0x80000000;
    Stats stats;
};

[[noreturn]] void Fail(const char* what, size_t at) {
    char text[160];
    sprintf_s(text, "%s at 0x%zx", what, at);
    throw std::runtime_error(text);
}

class Parser {
public:
    Parser(const BYTE* data, size_t size) : m_d(data), m_n(size) {}

    Model Run() {
        Model m;
        m_m = &m;
        if (m_n < 18 || memcmp(m_d, kMagic, sizeof(kMagic)) != 0) Fail("bad magic", 0);
        m_pos = 14;
        if (U32() != m_n) Fail("size field mismatch", 14);

        size_t start = m_pos;
        if (U8() != 0x02) Fail("string section expected", start);
        DWORD len = U32();
        Skip(start + len - m_pos);

        if (U8() != 0x0F) Fail("command section expected", m_pos - 1);
        DWORD count = U32();
        for (DWORD i = 0; i < count; i++) {
            m.ids.insert(U16());
            if (U16() != 0) Fail("command padding", m_pos - 2);
            BYTE k = U8();
            for (BYTE j = 0; j < k; j++) {
                static const BYTE lengths[10] = { 0, 4, 4, 6, 6, 6, 6, 4, 4, 4 };
                BYTE t = U8();
                if (t == 0 || t > 9) Fail("unknown command attribute", m_pos - 1);
                Skip(lengths[t]);
            }
        }

        start = m_pos;
        if (U8() != 0x10) Fail("symbol section expected", start);
        len = U32();
        count = U32();
        for (DWORD i = 0; i < count; i++) {
            m.ids.insert(U16());
            Skip(6);
            Skip(U16());
        }
        if (m_pos != start + len) Fail("symbol section length", m_pos);
        m.prelude.assign(m_d + 18, m_d + m_pos);

        if (U8() != 0x0D || U16() != 3) Fail("tree header expected", m_pos - 3);
        DWORD pointerSection = U32();
        m.root = Tag(0);
        if (m.root.tag != TagNode) Fail("root node expected", 0);

        size_t suppStart = m_pos;
        DWORD suppLen = U32();
        std::map<DWORD, size_t> blockAt;
        while (m_pos < suppStart + suppLen) {
            size_t blockStart = m_pos;
            WORD blockLen = U16();
            blockAt[(DWORD)blockStart] = m.blocks.size();
            m.blocks.push_back(Tag(1));
            if (m_pos != blockStart + blockLen) Fail("block length", blockStart);
        }
        if (m_pos != suppStart + suppLen || m_pos != pointerSection) Fail("block area end", m_pos);

        len = U32();
        if (len < 4 || (len - 4) % 10) Fail("pointer section length", pointerSection);
        for (DWORD i = 0; i < (len - 4) / 10; i++) {
            Entry e;
            e.id = U16();
            e.sub = U16();
            if (U16() != 0) Fail("pointer section padding", m_pos - 2);
            e.key = U32();
            if (!m_nodes.count(e.key)) Fail("pointer section target is not a node", m_pos - 4);
            m.entries.push_back(e);
        }
        if (m_pos != m_n) Fail("trailing bytes", m_pos);

        Resolve(m.root, blockAt);
        for (auto& b : m.blocks) Resolve(b, blockAt);

        m.stats.size = m_n;
        m.stats.blocks = m.blocks.size();
        m.stats.entries = m.entries.size();
        if (Element* tabs = MainTabs(m)) m.stats.tabs = tabs->children.size();
        return m;
    }

    static Element* MainTabs(Model& m) {
        for (auto& c : m.root.children)
            if (c.tag == TagColl && c.type == 0x02) return &c;
        return nullptr;
    }

private:
    const BYTE* m_d;
    size_t m_n;
    size_t m_pos = 0;
    Model* m_m = nullptr;
    std::set<DWORD> m_nodes;

    void Need(size_t k) { if (m_pos + k > m_n) Fail("truncated", m_pos); }
    void Skip(size_t k) { Need(k); m_pos += k; }
    BYTE U8() { Need(1); return m_d[m_pos++]; }
    WORD U16() { Need(2); WORD v; memcpy(&v, m_d + m_pos, 2); m_pos += 2; return v; }
    DWORD U32() { Need(4); DWORD v; memcpy(&v, m_d + m_pos, 4); m_pos += 4; return v; }

    Element Tag(int depth) {
        if (depth > 64) Fail("tree too deep", m_pos);
        Element e;
        size_t start = m_pos;
        e.tag = U8();
        switch (e.tag) {
        case TagNode: {
            e.key = (DWORD)start;
            e.type = U16();
            e.fix = U16();
            WORD len = U16();
            BYTE k = U8();
            for (BYTE i = 0; i < k; i++) e.children.push_back(Tag(depth + 1));
            if (m_pos != start + len) Fail("node length", start);
            m_nodes.insert(e.key);
            m_m->stats.nodes++;
            break;
        }
        case TagColl: {
            e.fix = U8();
            e.type = U8();
            WORD k = U16();
            for (WORD i = 0; i < k; i++) e.children.push_back(Tag(depth + 1));
            m_m->stats.collections++;
            break;
        }
        case TagProp: {
            Need(3);
            BYTE b1 = m_d[m_pos], b2 = m_d[m_pos + 1], b3 = m_d[m_pos + 2];
            size_t len;
            if (b1 == 0x01) len = b3 == 0x02 ? 4 : b3 == 0x03 ? 2 : 1;
            else if (b3 == 0x04) len = 4;
            else len = b1;
            Need(3 + len);
            e.raw.assign(m_d + m_pos, m_d + m_pos + 3 + len);
            if (b1 == 0x01 && b2 == 0x00) {
                DWORD id = 0;
                memcpy(&id, m_d + m_pos + 3, len);
                m_m->ids.insert(id);
            }
            m_pos += 3 + len;
            break;
        }
        case TagRef: {
            BYTE k = U8();
            size_t len = k == 0x09 ? 1 : k == 0x03 ? 2 : k == 0x02 ? 4 : 0;
            if (!len) Fail("unknown reference kind", m_pos - 1);
            Need(len);
            e.raw.push_back(k);
            e.raw.insert(e.raw.end(), m_d + m_pos, m_d + m_pos + len);
            m_pos += len;
            break;
        }
        case TagPtr:
            e.key = U32();
            m_m->stats.pointers++;
            break;
        default:
            Fail("unknown tag", start);
        }
        return e;
    }

    static void Resolve(Element& e, const std::map<DWORD, size_t>& blockAt) {
        if (e.tag == TagPtr) {
            auto it = blockAt.find(e.key);
            if (it == blockAt.end()) Fail("pointer target is not a block", e.key);
            e.block = it->second;
            e.key = 0;
        }
        for (auto& c : e.children) Resolve(c, blockAt);
    }
};

class Writer {
public:
    std::vector<BYTE> Run(Model& m) {
        Put(kMagic, sizeof(kMagic));
        size_t sizePos = Here();
        U32(0);
        Put(m.prelude.data(), m.prelude.size());

        U8(0x0D);
        U16(3);
        size_t pointerPos = Here();
        U32(0);
        Tag(m.root);

        size_t suppPos = Here();
        U32(0);
        std::vector<DWORD> blockAt;
        for (auto& b : m.blocks) {
            size_t start = Here();
            blockAt.push_back((DWORD)start);
            U16(0);
            Tag(b);
            At16(start, Here() - start);
        }
        At32(suppPos, Here() - suppPos);
        At32(pointerPos, Here());

        std::stable_sort(m.entries.begin(), m.entries.end(), [](const Entry& a, const Entry& b) {
            return a.id != b.id ? a.id < b.id : a.sub < b.sub;
        });
        U32((DWORD)(4 + 10 * m.entries.size()));
        for (auto& e : m.entries) {
            auto it = m_nodeAt.find(e.key);
            if (it == m_nodeAt.end()) Fail("entry target missing", e.key);
            U16(e.id);
            U16(e.sub);
            U16(0);
            U32(it->second);
        }

        for (auto& f : m_fixups) {
            if (f.second >= blockAt.size()) Fail("pointer to a missing block", f.first);
            At32(f.first, blockAt[f.second]);
        }
        At32(sizePos, Here());
        return std::move(m_out);
    }

private:
    std::vector<BYTE> m_out;
    std::map<DWORD, DWORD> m_nodeAt;
    std::vector<std::pair<size_t, size_t>> m_fixups;    // (offset, block index)

    size_t Here() const { return m_out.size(); }
    void Put(const BYTE* p, size_t n) { m_out.insert(m_out.end(), p, p + n); }
    void U8(BYTE v) { m_out.push_back(v); }
    void U16(WORD v) { Put((const BYTE*)&v, 2); }
    void U32(DWORD v) { Put((const BYTE*)&v, 4); }
    void At16(size_t at, size_t v) {
        if (v > 0xFFFF) Fail("length overflow", at);
        WORD w = (WORD)v;
        memcpy(&m_out[at], &w, 2);
    }
    void At32(size_t at, size_t v) {
        DWORD d = (DWORD)v;
        memcpy(&m_out[at], &d, 4);
    }

    void Tag(const Element& e) {
        size_t start = Here();
        U8(e.tag);
        switch (e.tag) {
        case TagNode: {
            if (e.children.size() > 0xFF) Fail("too many node children", start);
            if (!m_nodeAt.emplace(e.key, (DWORD)start).second) Fail("duplicate node key", start);
            U16(e.type);
            U16(e.fix);
            size_t lenPos = Here();
            U16(0);
            U8((BYTE)e.children.size());
            for (auto& c : e.children) Tag(c);
            At16(lenPos, Here() - start);
            break;
        }
        case TagColl:
            if (e.children.size() > 0xFFFF) Fail("too many collection items", start);
            U8((BYTE)e.fix);
            U8((BYTE)e.type);
            U16((WORD)e.children.size());
            for (auto& c : e.children) Tag(c);
            break;
        case TagProp:
        case TagRef:
            Put(e.raw.data(), e.raw.size());
            break;
        case TagPtr:
            m_fixups.emplace_back(Here(), e.block);
            U32(0);
            break;
        }
    }
};

bool IsIdProp(const Element& e) {
    return e.tag == TagProp && e.raw.size() == 5 && e.raw[0] == 0x01 && e.raw[1] == 0x00 && e.raw[2] == 0x03;
}

WORD WordAt(const std::vector<BYTE>& raw, size_t at) {
    WORD v;
    memcpy(&v, &raw[at], 2);
    return v;
}

void SetWordAt(std::vector<BYTE>& raw, size_t at, WORD v) {
    memcpy(&raw[at], &v, 2);
}

int NodeId(const Element& node) {
    for (auto& c : node.children)
        if (IsIdProp(c)) return WordAt(c.raw, 3);
    return -1;
}

Element* FindNode(std::vector<Element>& items, WORD id) {
    for (auto& c : items)
        if (c.tag == TagNode && NodeId(c) == id) return &c;
    return nullptr;
}

void CollectControlIds(const Element& e, std::vector<WORD>& ids) {
    for (auto& c : e.children) {
        if (IsIdProp(c) && std::find(ids.begin(), ids.end(), WordAt(c.raw, 3)) == ids.end())
            ids.push_back(WordAt(c.raw, 3));
        CollectControlIds(c, ids);
    }
}

void CheckWordIds(const Element& e) {
    if (e.tag == TagProp && e.raw[0] == 0x01 && e.raw[1] == 0x00 && !IsIdProp(e)) Fail("non-word id in cloned group", 0);
    for (auto& c : e.children) CheckWordIds(c);
}

// Gives every node of the clone a fresh key and replaces the ids found in `ids`.
void Renumber(Element& e, Model& m, const std::map<WORD, WORD>& ids, std::map<DWORD, DWORD>& keys) {
    if (e.tag == TagNode) {
        DWORD fresh = m.nextKey++;
        keys[e.key] = fresh;
        e.key = fresh;
    }
    if (IsIdProp(e) || (e.tag == TagRef && e.raw[0] == 0x03)) {
        size_t at = e.tag == TagRef ? 1 : 3;
        auto it = ids.find(WordAt(e.raw, at));
        if (it != ids.end()) SetWordAt(e.raw, at, it->second);
    }
    for (auto& c : e.children) Renumber(c, m, ids, keys);
}

void AddMirroredEntries(Model& m, const std::map<DWORD, DWORD>& keys, const std::map<WORD, WORD>& ids) {
    std::vector<Entry> added;
    for (auto& e : m.entries) {
        auto k = keys.find(e.key);
        if (k == keys.end()) continue;
        auto i = ids.find(e.id);
        if (i == ids.end()) Fail("indexed node without a remapped id", e.key);
        added.push_back({ i->second, 0, k->second });
    }
    m.entries.insert(m.entries.end(), added.begin(), added.end());
}

template <class F>
bool Guard(std::string* error, F&& f) {
    try {
        f();
        return true;
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    }
}

}

bool Validate(const BYTE* data, size_t size, Stats* stats, std::string* error) {
    return Guard(error, [&] {
        Model m = Parser(data, size).Run();
        if (stats) *stats = m.stats;
    });
}

bool RoundTrip(const BYTE* data, size_t size, std::vector<BYTE>& out, std::string* error) {
    return Guard(error, [&] {
        Model m = Parser(data, size).Run();
        out = Writer().Run(m);
    });
}

bool AddTab(const BYTE* data, size_t size, const NewTab& tab, std::vector<BYTE>& out, std::string* error) {
    return Guard(error, [&] {
        Model m = Parser(data, size).Run();

        std::set<WORD> fresh = { tab.tabId, tab.groupId };
        fresh.insert(tab.controlIds.begin(), tab.controlIds.end());
        if (fresh.size() != 2 + tab.controlIds.size()) Fail("new ids are not distinct", 0);
        for (WORD id : fresh)
            if (id < 0x100 || m.ids.count(id)) Fail("new id already used", id);

        Element* tabs = Parser::MainTabs(m);
        if (!tabs) Fail("main tabs not found", 0);
        Element* source = FindNode(tabs->children, tab.sourceTab);
        if (!source) Fail("source tab not found", tab.sourceTab);
        size_t sourceIndex = source - tabs->children.data();

        const Element* ptr = nullptr;
        for (auto& c : source->children)
            if (c.tag == TagPtr) ptr = &c;
        if (!ptr) Fail("source tab has no block", tab.sourceTab);
        Element& sourceBlock = m.blocks[ptr->block];
        if (sourceBlock.tag != TagColl) Fail("source block is not a collection", ptr->block);
        Element* group = FindNode(sourceBlock.children, tab.sourceGroup);
        if (!group) Fail("source group not found", tab.sourceGroup);

        Element groupCopy = *group;
        CheckWordIds(groupCopy);
        std::vector<WORD> controls;
        for (auto& c : groupCopy.children)
            if (!IsIdProp(c)) CollectControlIds(c, controls);
        if (controls.size() != tab.controlIds.size()) Fail("control count differs from the source group", controls.size());

        std::map<WORD, WORD> ids = { { tab.sourceGroup, tab.groupId }, { tab.sourceTab, tab.tabId } };
        for (size_t i = 0; i < controls.size(); i++) ids[controls[i]] = tab.controlIds[i];

        std::map<DWORD, DWORD> keys;
        Renumber(groupCopy, m, ids, keys);

        Element block;
        block.tag = TagColl;
        block.fix = sourceBlock.fix;
        block.type = sourceBlock.type;
        block.children.push_back(std::move(groupCopy));
        m.blocks.push_back(std::move(block));

        Element tabCopy = *source;
        tabCopy.children.erase(std::remove_if(tabCopy.children.begin(), tabCopy.children.end(),
            [](const Element& c) { return c.tag != TagProp; }), tabCopy.children.end());
        Renumber(tabCopy, m, ids, keys);
        Element link;
        link.tag = TagPtr;
        link.block = m.blocks.size() - 1;
        tabCopy.children.push_back(link);
        if (tabCopy.children.size() != source->children.size()) Fail("unexpected source tab layout", tab.sourceTab);

        AddMirroredEntries(m, keys, ids);
        tabs->children.insert(tabs->children.begin() + sourceIndex + 1, std::move(tabCopy));
        out = Writer().Run(m);
    });
}

}
