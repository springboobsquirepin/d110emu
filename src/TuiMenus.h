#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "TextScreen.h"

// What the terminal interfaces (TuiApp, TranslatorTui) share on top of TextScreen.h: menus drawn in a box over the
// program's screen and rebuilt every frame so their values stay current, a list to choose one of, a line of text to
// type, a file browser; styles from the ANSI colours, wrapped text, the row of key hints.
namespace Tui {

// The 16 ANSI colours, by their numbers (Color::ansiColor, ansiStyle).
namespace Ansi {
enum : int {
    kBlack, kRed, kGreen, kYellow, kBlue, kMagenta, kCyan, kWhite,
    kGrey, kBrightRed, kBrightGreen, kBrightYellow, kBrightBlue, kBrightMagenta, kBrightCyan, kBrightWhite,
};
}  // namespace Ansi

// A style with one of the ANSI colours as its foreground (-1: the terminal's own), and attributes.
Style ansiStyle(int foreground, uint8_t attributes = 0);
// Words to lines of at most `width` columns.
std::vector<std::string> wrapText(const std::string& text, int width);
std::string lowerText(std::string text);
// A name as asked for on the command line: the one it names in full, else the first with it in its name (any case).
std::string findByName(const std::vector<std::string>& names, const std::string& wanted);
// Keys and what they do on the bottom row, as many as fit: the key in reverse video, what it does dimmed.
void drawHints(Screen& screen, const std::vector<std::pair<std::string, std::string>>& hints);

struct MenuItem {
    std::string label;
    std::string value;                // At the right
    std::function<void()> activate;   // Enter or Space
    std::function<void(int)> adjust;  // Left and right: -1 and +1
    bool enabled = true;
    bool heading = false;             // A section's title, not an item to choose
    bool info = false;                // A line of information, not an item to choose
    bool problem = false;             // ...showing what is wrong
    std::string help;                 // Under the menu while the item is selected
};

// The open menus (the innermost shows and takes the keys) and the text input, over whatever the program draws.
class Menus {
public:
    using Build = std::function<void(std::vector<MenuItem>&)>;
    static constexpr int kInfoWidth = 64;  // Lines of information wrap at this width

    // A menu built by `build` every frame, so the values stay current.
    void open(const std::string& title, Build build);
    // A list to choose one of: `current` marked and selected, `choose` called with the index chosen (the list closes).
    void openChoice(const std::string& title, const std::vector<std::string>& labels, int current, std::function<void(int)> choose);
    void openTextInput(const std::string& title, const std::string& prompt, const std::string& text,
                       std::function<void(const std::string&)> accept);
    // The folders and the files `show` lets through at `folder`, to choose one: `chosen` gets it once the menus are
    // closed. `none` says what a folder without such files lacks.
    void openFiles(const std::filesystem::path& folder, std::function<bool(const std::filesystem::path&)> show, const std::string& none,
                   std::function<void(const std::filesystem::path&)> chosen);

    bool active() const { return !menus_.empty() || input_.active; }
    bool menuOpen() const { return !menus_.empty(); }
    void pop() {
        if (!menus_.empty()) menus_.pop_back();
    }
    void closeAll() { menus_.clear(); }
    // Selects an item of the innermost menu.
    void select(int index);

    // The innermost menu and the text input, over the screen.
    void draw(Screen& screen);
    // To the text input while it shows, else to the innermost menu.
    void onKey(const Key& key);

    // For builders: a section's title, lines of information (wrapped at kInfoWidth), an item.
    static void heading(std::vector<MenuItem>& items, const std::string& title);
    static void info(std::vector<MenuItem>& items, const std::string& text, bool problem = false);
    static void item(std::vector<MenuItem>& items, const std::string& label, const std::string& value, std::function<void()> activate,
                     std::function<void(int)> adjust, const std::string& help, bool enabled = true);

private:
    struct Menu {
        std::string title;
        Build build;
        int selected = -1;  // -1: the first item that can be chosen
        int scroll = 0;
    };
    struct TextInput {
        bool active = false;
        std::string title;
        std::string prompt;
        std::u32string text;
        size_t cursor = 0;
        std::function<void(const std::string&)> accept;
    };

    void drawMenu(Screen& screen, Menu& menu);
    void drawTextInput(Screen& screen);
    void onMenuKey(const Key& key);
    void onTextInputKey(const Key& key);

    std::vector<Menu> menus_;  // The innermost last
    TextInput input_;
};

}  // namespace Tui
