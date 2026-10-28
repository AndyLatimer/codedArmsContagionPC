#pragma once
// The PC settings page, opened from the game's own OPTIONS screen (its fourth row, which the
// port turns into PC SETUP) and drawn in that screen's style over it: rows of translucent
// panels with the label on the left and the value on the right, the selected row in orange,
// a help line and the keys at the bottom. Drawn into the same kind of image as the system
// dialogs (DialogOverlay.h); the front end builds the items and feeds it input while it is open.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ca {

struct MenuItem {
    std::string label;
    bool header = false;                      // a section title (not selectable)
    std::function<std::string()> value;       // shown on the right (optional)
    std::function<void(int dir)> change;      // left (-1) / right (+1)
    std::function<void()> activate;           // Enter / click (default: change(+1))
    std::string note;                         // after the value (e.g. "al reiniciar")
    std::string help;                         // the help line while selected
};

class OptionsMenu {
public:
    void open(std::vector<MenuItem> items);
    void close() { open_ = false; }
    bool isOpen() const { return open_; }

    void move(int dir);        // up (-1) / down (+1)
    void change(int dir);      // left / right
    void activate();
    void wheel(int dir);       // scroll by rows
    // Mouse in image coordinates (kOverlayW x kOverlayH): hover selects, click activates (or
    // changes the value by its side: left half of the value -1, right half +1).
    void hover(int x, int y);
    void click(int x, int y);

    // A message over the menu instead of the list (e.g. "press a key"), empty for none.
    void setPrompt(std::string text) { prompt_ = std::move(text); }
    // Keys line at the bottom (e.g. "<> Cambiar  Enter OK  Esc Volver") and the page's name.
    void setFooter(std::string text) { footer_ = std::move(text); }
    void setTitle(std::string text) { title_ = std::move(text); }

    void paint(std::vector<uint32_t>& rgba) const;

private:
    int rowAt(int x, int y) const;
    void keepVisible();
    bool selectable(int i) const { return i >= 0 && i < (int)items_.size() && !items_[(size_t)i].header; }

    std::vector<MenuItem> items_;
    bool open_ = false;
    int selected_ = 0, first_ = 0;
    std::string prompt_, footer_, title_;
};

} // namespace ca
