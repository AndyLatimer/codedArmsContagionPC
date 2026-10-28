#pragma once
// Host-facing interface of the PSP system dialogs (sceUtility savedata and message dialogs).
//
// The HLE side (src/psp/ppsspp/PSPDialog.cpp and its subclasses, ported from PPSSPP) runs the
// dialogs' logic exactly like PPSSPP but does not draw anything. Every time the game calls
// sceUtility*Update, the running dialog publishes a DialogView describing what the PSP would show
// on that frame (message text, yes/no or OK choices, the savedata list with titles, details and
// icons, the button legends). A front end renders the current view and feeds the player's
// decisions back with submitDialogInput(); the dialog consumes the input on its next Update, the
// same way PPSSPP consumes a controller button press.
//
// When no front end answers, an optional auto responder (setDialogAutoResponder) decides; the
// headless runner installs one driven by the CA_DIALOG_POLICY environment variable.
//
// Thread safety: all functions may be called from any thread.
#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace psp::host {

enum class DialogKind {
    None,      // nothing on screen
    Message,   // sceUtilityMsgDialog: text (or a formatted error code) with optional Yes/No or OK
    Savedata,  // sceUtilitySavedata: list / confirm / progress / result screens
};

// One savedata slot as the PSP's savedata list shows it.
struct DialogSaveEntry {
    std::string saveName;      // e.g. "000" (the folder is gameName + saveName)
    std::string title;         // PARAM.SFO TITLE
    std::string savedataTitle; // PARAM.SFO SAVEDATA_TITLE
    std::string detail;        // PARAM.SFO SAVEDATA_DETAIL
    int64_t sizeBytes = 0;     // 0 = empty slot ("New Data")
    bool broken = false;       // folder without a readable PARAM.SFO
    std::tm modified{};        // last modification (local time)
    std::vector<uint8_t> iconPng; // ICON0.PNG of the save, or the game's icon for new data
};

struct DialogView {
    uint64_t serial = 0;          // increases whenever any field below changes
    DialogKind kind = DialogKind::None;
    std::string banner;           // savedata screens: "Save", "Load", "Delete" or ""
    std::string text;             // the message (UTF-8, may contain '\n')
    bool yesNo = false;           // shows inline Yes / No choices
    bool ok = false;              // shows an inline OK choice (message dialog V3)
    int yesNoChoice = 1;          // highlighted choice: 1 = Yes, 0 = No
    bool confirmButton = false;   // legend for the confirm button is shown
    bool cancelButton = false;    // legend for the cancel button is shown
    std::string confirmLabel;     // legend text ("Enter" unless the game supplied one)
    std::string cancelLabel;      // legend text ("Back" unless the game supplied one)
    bool crossConfirms = true;    // the dialog's confirm button is CROSS (else CIRCLE)
    bool busy = false;            // progress screen ("Saving...", "Loading..."): takes no input
    std::vector<DialogSaveEntry> entries; // savedata list (list screens), else empty
    int selected = 0;             // highlighted list entry
    bool showsEntry = false;      // a single slot is shown (confirm/result screens) ...
    DialogSaveEntry entry;        // ... this one
    int fade = 255;               // 0..255 opacity of the dialog (fades in/out like the PSP)
};

// The player's decision. Fields left at their defaults change nothing.
struct DialogInput {
    int yesNoChoice = -1; // 1 = Yes, 0 = No: moves the highlight first
    int select = -1;      // list index: moves the list highlight first
    bool confirm = false; // presses the confirm button (accepts the highlighted choice / entry)
    bool cancel = false;  // presses the cancel button
};

// ---- front end -----------------------------------------------------------------------------
// Copies the current view. Returns false when no dialog is on screen.
bool currentDialog(DialogView& out);
// Queues an input for the running dialog; it is applied on the dialog's next Update.
void submitDialogInput(const DialogInput& in);

// Called (on a guest thread) when a dialog waits for input and none is queued: return true and
// fill `in` to answer. `visibleUs` is how long the current screen has been shown.
using DialogAutoResponder = std::function<bool(const DialogView& view, uint64_t visibleUs, DialogInput& in)>;
void setDialogAutoResponder(DialogAutoResponder responder);

// Alternative input mode, exactly PPSSPP's: the dialogs read the emulated PSP controller
// (host::g_ctrl, newly pressed buttons since the previous Update) instead of DialogInput.
void setDialogUsesController(bool enable);
bool dialogUsesController();

// Optional: called whenever a new screen is published (for logging / waking a renderer).
void setDialogObserver(std::function<void(const DialogView&)> observer);

// ---- HLE side ----------------------------------------------------------------------------------
void publishDialog(DialogView view); // serial is assigned here
void closeDialog();
// Takes the queued input (or asks the auto responder when the screen waits for input).
bool takeDialogInput(DialogInput& out);

} // namespace psp::host
