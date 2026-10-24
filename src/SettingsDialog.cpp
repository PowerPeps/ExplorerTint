#include "SettingsDialog.h"
#include "Settings.h"
#include "resource.h"
#include <algorithm>
#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace {

const COLORREF kDarkBack = RGB(0x20, 0x20, 0x20);
const COLORREF kDarkEdit = RGB(0x2B, 0x2B, 0x2B);
const COLORREF kDarkText = RGB(0xFF, 0xFF, 0xFF);
const COLORREF kDarkHeader = RGB(0x99, 0xEB, 0xFF);
const COLORREF kLightHeader = RGB(0x00, 0x33, 0x99);

struct Text {
    int id;
    const wchar_t* fr;
    const wchar_t* en;
};

const Text kTexts[] = {
    { IDC_OPTIONS_H, L"Options", L"Options" },
    { IDC_GIT + IDC_LABEL_OFFSET, L"Couleurs Git", L"Git colors" },
    { IDC_SYSLAST + IDC_LABEL_OFFSET, L"Éléments système en fin de liste", L"System items sorted last" },
    { IDC_HIDENAMES + IDC_LABEL_OFFSET, L"Afficher les noms listés comme cachés", L"Show the listed names as hidden" },
    { IDC_CLASSIC + IDC_LABEL_OFFSET, L"Liste classique (nouvelles fenêtres)", L"Classic list (new windows)" },
    { IDC_RIBBON + IDC_LABEL_OFFSET, L"Onglet ExplorerTint dans le ruban (nouvelles fenêtres)", L"ExplorerTint ribbon tab (new windows)" },
    { IDC_REPOICON + IDC_LABEL_OFFSET, L"Icône bleue pour les dossiers de dépôt Git", L"Blue icon for Git repository folders" },
    { IDC_COLORS_H, L"Couleurs", L"Colors" },
    { IDC_TEXT_H, L"Texte", L"Text" },
    { IDC_BACK_H, L"Fond", L"Background" },
    { IDC_STAGED_L, L"Indexé", L"Staged" },
    { IDC_UNTRACKED_L, L"Non suivi", L"Untracked" },
    { IDC_IGNORED_L, L"Ignoré", L"Ignored" },
    { IDC_SYSTEM_L, L"Système", L"System" },
    { IDC_NAMES_L, L"Noms affichés comme cachés (séparés par ;)", L"Names shown as hidden (separated by ;)" },
    { IDC_GITPATH_L, L"Chemin de git.exe (vide : recherche dans le PATH)", L"Path to git.exe (empty: search the PATH)" },
    { IDC_FOLDERICONS_L, L"Icônes de dossier (nom=badge ; badge : nom intégré, \"texte\" ou fichier image/icône)",
                         L"Folder icons (name=badge; badge: built-in name, \"text\" or image/icon file)" },
    { IDC_ICONROOTS_L, L"Emplacements des icônes de dossier (séparés par ; vide : partout)",
                       L"Where folder icons apply (separated by ; empty: everywhere)" },
    { IDC_NOTE, L"Couleurs au format #RRGGBB ou #RRGGBBAA (AA = opacité). Vide : rendu normal. Badges intégrés : android, claude, desktop, fonts, git, hardware, web.",
                L"Colors as #RRGGBB or #RRGGBBAA (AA = opacity). Empty: normal rendering. Built-in badges: android, claude, desktop, fonts, git, hardware, web." },
    { IDC_DEFAULTS, L"Par défaut", L"Defaults" },
    { IDOK, L"OK", L"OK" },
    { IDCANCEL, L"Annuler", L"Cancel" },
    { IDC_APPLY, L"Appliquer", L"Apply" },
};

const int kHeaders[] = { IDC_OPTIONS_H, IDC_COLORS_H };

struct Toggle {
    int id;
    bool Settings::* field;
};

const Toggle kToggles[] = {
    { IDC_GIT, &Settings::git },
    { IDC_SYSLAST, &Settings::systemLast },
    { IDC_HIDENAMES, &Settings::hideNames },
    { IDC_CLASSIC, &Settings::classicList },
    { IDC_RIBBON, &Settings::ribbonTab },
    { IDC_REPOICON, &Settings::repoIcon },
};

Rgba* ColorField(Settings& s, int index) {
    StateStyle* styles[] = { &s.staged, &s.untracked, &s.ignored, &s.system };
    StateStyle* st = styles[index / 2];
    return index % 2 ? &st->back : &st->text;
}

struct State {
    bool dark = false;
    HBRUSH back = nullptr;
    HBRUSH edit = nullptr;
    HFONT bold = nullptr;
};

HWND s_open = nullptr;
COLORREF s_custom[16];

const wchar_t* Tr(const wchar_t* fr, const wchar_t* en) {
    return UiFrench() ? fr : en;
}

std::wstring ItemText(HWND dlg, int id) {
    HWND item = GetDlgItem(dlg, id);
    int n = GetWindowTextLengthW(item);
    std::wstring s(n + 1, L'\0');
    GetWindowTextW(item, &s[0], n + 1);
    s.resize(n);
    return s;
}

void Fill(HWND dlg, const Settings& s) {
    for (auto& t : kToggles) CheckDlgButton(dlg, t.id, s.*t.field ? BST_CHECKED : BST_UNCHECKED);
    Settings copy = s;
    for (int i = 0; i < IDC_COLOR_COUNT; i++)
        SetDlgItemTextW(dlg, IDC_COLOR_FIRST + i, FormatColor(*ColorField(copy, i)).c_str());
    std::wstring names;
    for (auto& n : s.hiddenNames) names += (names.empty() ? L"" : L"; ") + n;
    SetDlgItemTextW(dlg, IDC_NAMES, names.c_str());
    SetDlgItemTextW(dlg, IDC_GITPATH, s.gitPath.c_str());
    SetDlgItemTextW(dlg, IDC_FOLDERICONS, JoinIcons(s.folderIcons, L"; ").c_str());
    std::wstring roots;
    for (auto& r : s.iconRoots) roots += (roots.empty() ? L"" : L"; ") + r;
    SetDlgItemTextW(dlg, IDC_ICONROOTS, roots.c_str());
}

bool Collect(HWND dlg, Settings& s) {
    for (auto& t : kToggles) s.*t.field = IsDlgButtonChecked(dlg, t.id) == BST_CHECKED;
    for (int i = 0; i < IDC_COLOR_COUNT; i++) {
        std::wstring text = ItemText(dlg, IDC_COLOR_FIRST + i);
        size_t a = text.find_first_not_of(L" \t"), b = text.find_last_not_of(L" \t");
        text = a == std::wstring::npos ? L"" : text.substr(a, b - a + 1);
        if (!ParseColor(text, *ColorField(s, i))) {
            std::wstring msg = std::wstring(Tr(L"Couleur invalide : ", L"Invalid color: ")) + text +
                               Tr(L"\nFormats acceptés : #RRGGBB, #RRGGBBAA ou vide.", L"\nAccepted: #RRGGBB, #RRGGBBAA or empty.");
            MessageBoxW(dlg, msg.c_str(), L"ExplorerTint", MB_ICONWARNING);
            HWND edit = GetDlgItem(dlg, IDC_COLOR_FIRST + i);
            SetFocus(edit);
            SendMessageW(edit, EM_SETSEL, 0, -1);
            return false;
        }
    }
    s.hiddenNames = SplitNames(ItemText(dlg, IDC_NAMES));
    s.gitPath = ItemText(dlg, IDC_GITPATH);
    s.folderIcons = SplitIcons(ItemText(dlg, IDC_FOLDERICONS));
    s.iconRoots = SplitRoots(ItemText(dlg, IDC_ICONROOTS));
    return true;
}

bool Apply(HWND dlg) {
    Settings s = *CurrentSettings();
    if (!Collect(dlg, s)) return false;
    if (!SaveSettings(s)) {
        MessageBoxW(dlg, Tr(L"Échec de l'enregistrement des réglages.", L"Settings could not be saved."), L"ExplorerTint",
                    MB_ICONERROR);
        return false;
    }
    return true;
}

void DrawSwatch(HWND dlg, const DRAWITEMSTRUCT* di, const State* st) {
    RECT rc = di->rcItem;
    HDC hdc = di->hDC;
    COLORREF frame = st->dark ? RGB(0x80, 0x80, 0x80) : RGB(0x70, 0x70, 0x70);
    COLORREF under = st->dark ? kDarkBack : GetSysColor(COLOR_WINDOW);
    SetDCBrushColor(hdc, under);
    FillRect(hdc, &rc, (HBRUSH)GetStockObject(DC_BRUSH));
    RECT in = rc;
    InflateRect(&in, -2, -2);
    int cell = (in.bottom - in.top) / 2;
    for (int y = in.top; y < in.bottom; y += cell)
        for (int x = in.left; x < in.right; x += cell) {
            RECT c = { x, y, std::min<LONG>(x + cell, in.right), std::min<LONG>(y + cell, in.bottom) };
            bool odd = ((x - in.left) / cell + (y - in.top) / cell) % 2;
            SetDCBrushColor(hdc, odd ? RGB(0xC0, 0xC0, 0xC0) : RGB(0xFF, 0xFF, 0xFF));
            FillRect(hdc, &c, (HBRUSH)GetStockObject(DC_BRUSH));
        }
    Rgba color;
    bool valid = ParseColor(ItemText(dlg, di->CtlID - IDC_SWATCH_OFFSET), color);
    if (valid && color.set) {
        for (int y = in.top; y < in.bottom; y += cell)
            for (int x = in.left; x < in.right; x += cell) {
                RECT c = { x, y, std::min<LONG>(x + cell, in.right), std::min<LONG>(y + cell, in.bottom) };
                bool odd = ((x - in.left) / cell + (y - in.top) / cell) % 2;
                SetDCBrushColor(hdc, Blend(color, odd ? RGB(0xC0, 0xC0, 0xC0) : RGB(0xFF, 0xFF, 0xFF)));
                FillRect(hdc, &c, (HBRUSH)GetStockObject(DC_BRUSH));
            }
    } else {
        HPEN pen = CreatePen(PS_SOLID, 1, valid ? RGB(0x90, 0x90, 0x90) : RGB(0xE0, 0x30, 0x30));
        HGDIOBJ old = SelectObject(hdc, pen);
        MoveToEx(hdc, in.left, in.bottom - 1, nullptr);
        LineTo(hdc, in.right, in.top - 1);
        if (!valid) {
            MoveToEx(hdc, in.left, in.top, nullptr);
            LineTo(hdc, in.right, in.bottom);
        }
        SelectObject(hdc, old);
        DeleteObject(pen);
    }
    SetDCBrushColor(hdc, frame);
    FrameRect(hdc, &in, (HBRUSH)GetStockObject(DC_BRUSH));
    if (di->itemState & ODS_FOCUS) DrawFocusRect(hdc, &rc);
}

void PickColor(HWND dlg, int editId) {
    Rgba color;
    if (!ParseColor(ItemText(dlg, editId), color) || !color.set) {
        color = Rgba();
        color.a = 0xFF;
    }
    CHOOSECOLORW cc = { sizeof(cc) };
    cc.hwndOwner = dlg;
    cc.rgbResult = RGB(color.r, color.g, color.b);
    cc.lpCustColors = s_custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return;
    color.r = GetRValue(cc.rgbResult);
    color.g = GetGValue(cc.rgbResult);
    color.b = GetBValue(cc.rgbResult);
    color.set = true;
    SetDlgItemTextW(dlg, editId, FormatColor(color).c_str());
}

void Darken(HWND dlg) {
    BOOL on = TRUE;
    DwmSetWindowAttribute(dlg, 20, &on, sizeof(on));  // DWMWA_USE_IMMERSIVE_DARK_MODE
    AllowDarkMode(dlg);
    for (HWND child = GetWindow(dlg, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        wchar_t cls[32];
        GetClassNameW(child, cls, ARRAYSIZE(cls));
        AllowDarkMode(child);
        if (!_wcsicmp(cls, L"Edit")) SetWindowTheme(child, L"DarkMode_CFD", nullptr);
        else if (!_wcsicmp(cls, L"Button")) SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
    }
}

INT_PTR CALLBACK DialogProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto st = (State*)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG: {
        st = new State;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)st);
        st->dark = IsDarkMode();
        st->back = CreateSolidBrush(st->dark ? kDarkBack : GetSysColor(COLOR_BTNFACE));
        st->edit = CreateSolidBrush(st->dark ? kDarkEdit : GetSysColor(COLOR_WINDOW));
        LOGFONTW lf;
        if (GetObjectW((HFONT)SendMessageW(dlg, WM_GETFONT, 0, 0), sizeof(lf), &lf)) {
            lf.lfWeight = FW_SEMIBOLD;
            st->bold = CreateFontIndirectW(&lf);
            for (int id : kHeaders) SendDlgItemMessageW(dlg, id, WM_SETFONT, (WPARAM)st->bold, FALSE);
        }
        for (auto& t : kTexts) SetDlgItemTextW(dlg, t.id, Tr(t.fr, t.en));
        SetWindowTextW(dlg, Tr(L"ExplorerTint — Paramètres", L"ExplorerTint — Settings"));
        for (int i = 0; i < IDC_COLOR_COUNT; i++) SendDlgItemMessageW(dlg, IDC_COLOR_FIRST + i, EM_LIMITTEXT, 9, 0);
        Fill(dlg, *CurrentSettings());
        if (st->dark) Darken(dlg);
        s_open = dlg;
        return TRUE;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORBTN:
        if (st && st->dark) return (INT_PTR)st->back;
        break;
    case WM_CTLCOLORSTATIC: {
        if (!st) break;
        HDC hdc = (HDC)wp;
        int id = GetDlgCtrlID((HWND)lp);
        bool header = id == IDC_OPTIONS_H || id == IDC_COLORS_H;
        if (st->dark) {
            SetTextColor(hdc, header ? kDarkHeader : kDarkText);
            SetBkColor(hdc, kDarkBack);
            return (INT_PTR)st->back;
        }
        if (header) {
            SetTextColor(hdc, kLightHeader);
            SetBkMode(hdc, TRANSPARENT);
            return (INT_PTR)st->back;
        }
        break;
    }
    case WM_CTLCOLOREDIT:
        if (st && st->dark) {
            SetTextColor((HDC)wp, kDarkText);
            SetBkColor((HDC)wp, kDarkEdit);
            return (INT_PTR)st->edit;
        }
        break;
    case WM_DRAWITEM: {
        auto di = (const DRAWITEMSTRUCT*)lp;
        if (st && di->CtlID >= IDC_COLOR_FIRST + IDC_SWATCH_OFFSET && di->CtlID < IDC_COLOR_FIRST + IDC_SWATCH_OFFSET + IDC_COLOR_COUNT) {
            DrawSwatch(dlg, di, st);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id >= IDC_GIT + IDC_LABEL_OFFSET && id <= IDC_REPOICON + IDC_LABEL_OFFSET && code == STN_CLICKED) {
            int box = id - IDC_LABEL_OFFSET;
            CheckDlgButton(dlg, box, IsDlgButtonChecked(dlg, box) == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED);
            return TRUE;
        }
        if (id >= IDC_COLOR_FIRST && id < IDC_COLOR_FIRST + IDC_COLOR_COUNT && code == EN_CHANGE) {
            InvalidateRect(GetDlgItem(dlg, id + IDC_SWATCH_OFFSET), nullptr, FALSE);
            return TRUE;
        }
        if (id >= IDC_COLOR_FIRST + IDC_SWATCH_OFFSET && id < IDC_COLOR_FIRST + IDC_SWATCH_OFFSET + IDC_COLOR_COUNT) {
            PickColor(dlg, id - IDC_SWATCH_OFFSET);
            return TRUE;
        }
        switch (id) {
        case IDOK:
            if (Apply(dlg)) EndDialog(dlg, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        case IDC_APPLY:
            Apply(dlg);
            return TRUE;
        case IDC_DEFAULTS:
            Fill(dlg, DefaultSettings());
            return TRUE;
        }
        break;
    }
    case WM_DESTROY:
        if (st) {
            DeleteObject(st->back);
            DeleteObject(st->edit);
            if (st->bold) DeleteObject(st->bold);
            delete st;
            SetWindowLongPtrW(dlg, DWLP_USER, 0);
        }
        s_open = nullptr;
        break;
    }
    return FALSE;
}

}

void ShowSettingsDialog(HWND owner) {
    if (s_open) {
        SetForegroundWindow(s_open);
        return;
    }
    if (DialogBoxParamW(g_hinst, MAKEINTRESOURCEW(IDD_SETTINGS), owner, DialogProc, 0) < 0)
        Log(L"settings: dialog failed (%lu)", GetLastError());
}
