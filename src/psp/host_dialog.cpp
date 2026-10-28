// Shared state between the HLE dialogs and the front end (see host_dialog.h).
#include "psp/host_dialog.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>

namespace psp::host {

namespace {

std::mutex g_mutex;
bool g_visible = false;
DialogView g_view;
uint64_t g_serial = 0;
std::chrono::steady_clock::time_point g_shownAt;
std::deque<DialogInput> g_inputs;
DialogAutoResponder g_responder;
std::function<void(const DialogView&)> g_observer;
std::atomic<bool> g_useController{false};
uint64_t g_answeredSerial = 0; // the responder answers each screen once

bool sameEntry(const DialogSaveEntry& a, const DialogSaveEntry& b) {
    return a.saveName == b.saveName && a.title == b.title && a.savedataTitle == b.savedataTitle &&
           a.detail == b.detail && a.sizeBytes == b.sizeBytes && a.broken == b.broken &&
           a.iconPng.size() == b.iconPng.size();
}

// Same screen content, ignoring the fade (which changes every frame while fading).
bool sameScreen(const DialogView& a, const DialogView& b) {
    if (a.kind != b.kind || a.banner != b.banner || a.text != b.text || a.yesNo != b.yesNo || a.ok != b.ok ||
        a.yesNoChoice != b.yesNoChoice || a.confirmButton != b.confirmButton || a.cancelButton != b.cancelButton ||
        a.confirmLabel != b.confirmLabel || a.cancelLabel != b.cancelLabel || a.busy != b.busy ||
        a.selected != b.selected || a.showsEntry != b.showsEntry || a.entries.size() != b.entries.size() ||
        a.crossConfirms != b.crossConfirms)
        return false;
    if (a.showsEntry && !sameEntry(a.entry, b.entry)) return false;
    for (size_t i = 0; i < a.entries.size(); ++i)
        if (!sameEntry(a.entries[i], b.entries[i])) return false;
    return true;
}

} // namespace

bool currentDialog(DialogView& out) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_visible) return false;
    out = g_view;
    return true;
}

void submitDialogInput(const DialogInput& in) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_inputs.push_back(in);
}

void setDialogAutoResponder(DialogAutoResponder responder) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_responder = std::move(responder);
}

void setDialogObserver(std::function<void(const DialogView&)> observer) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_observer = std::move(observer);
}

void setDialogUsesController(bool enable) { g_useController = enable; }
bool dialogUsesController() { return g_useController; }

void publishDialog(DialogView view) {
    std::function<void(const DialogView&)> observer;
    DialogView copy;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        bool changed = !g_visible || !sameScreen(view, g_view);
        bool fadeChanged = g_visible && view.fade != g_view.fade;
        if (!changed && !fadeChanged) return;
        view.serial = ++g_serial;
        if (changed) g_shownAt = std::chrono::steady_clock::now();
        g_view = std::move(view);
        g_visible = true;
        if (!changed) return;
        observer = g_observer;
        copy = g_view;
    }
    if (observer) observer(copy);
}

void closeDialog() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_visible) ++g_serial;
    g_visible = false;
    g_view = DialogView{};
    g_inputs.clear();
}

bool takeDialogInput(DialogInput& out) {
    DialogAutoResponder responder;
    DialogView view;
    uint64_t visibleUs = 0;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (!g_inputs.empty()) {
            out = g_inputs.front();
            g_inputs.pop_front();
            return true;
        }
        if (!g_visible || g_view.busy || !g_responder || g_answeredSerial == g_serial) return false;
        responder = g_responder;
        view = g_view;
        visibleUs = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - g_shownAt).count();
    }
    if (!responder(view, visibleUs, out)) return false;
    std::lock_guard<std::mutex> lk(g_mutex);
    g_answeredSerial = view.serial;
    return true;
}

} // namespace psp::host
