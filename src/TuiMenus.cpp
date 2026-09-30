#include "TuiMenus.h"

#include <algorithm>
#include <cctype>
#include <memory>

#include "Platform.h"

namespace Tui {

using namespace Ansi;

namespace {

std::u32string toUtf32(const std::string& text) {
    std::u32string out;
    for (size_t i = 0; i < text.size();) out += decodeUtf8(text, i);
    return out;
}

std::string toUtf8(const std::u32string& text) {
    std::string out;
    for (char32_t ch : text) appendUtf8(out, ch);
    return out;
}

}  // namespace

Style ansiStyle(int foreground, uint8_t attributes) {
    Style result;
    if (foreground >= 0) result.fg = Color::ansiColor(foreground);
    result.attributes = attributes;
    return result;
}

std::vector<std::string> wrapText(const std::string& text, int width) {
    std::vector<std::string> lines;
    std::string line;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(' ', start);
        if (end == std::string::npos) end = text.size();
        const std::string word = text.substr(start, end - start);
        if (!line.empty() && textWidth(line) + 1 + textWidth(word) > width) {
            lines.push_back(line);
            line.clear();
        }
        line += (line.empty() ? "" : " ") + word;
        start = end + 1;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

std::string lowerText(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](char c) { return char(std::tolower(uint8_t(c))); });
    return text;
}

std::string findByName(const std::vector<std::string>& names, const std::string& wanted) {
    for (const std::string& name : names) {
        if (name == wanted) return name;
    }
    const std::string part = lowerText(wanted);
    for (const std::string& name : names) {
        if (lowerText(name).find(part) != std::string::npos) return name;
    }
    return std::string();
}

void drawHints(Screen& screen, const std::vector<std::pair<std::string, std::string>>& hints) {
    const int width = screen.columns();
    const int y = screen.rows() - 1;
    int x = 0;
    for (const auto& hint : hints) {
        const int needed = textWidth(hint.first) + 1 + textWidth(hint.second) + 2;
        if (x + needed > width) break;
        x += screen.text(x, y, hint.first, ansiStyle(-1, Reverse | Bold));
        x += screen.text(x, y, " " + hint.second + "  ", ansiStyle(-1, Dim));
    }
}

// ---------------------------------------------------------------------------------------------
// Opening

void Menus::open(const std::string& title, Build build) {
    Menu menu;
    menu.title = title;
    menu.build = std::move(build);
    menus_.push_back(std::move(menu));
}

void Menus::openChoice(const std::string& title, const std::vector<std::string>& labels, int current, std::function<void(int)> choose) {
    auto chosen = std::make_shared<std::function<void(int)>>(std::move(choose));
    open(title, [this, labels, current, chosen](std::vector<MenuItem>& items) {
        for (int i = 0; i < int(labels.size()); i++) {
            MenuItem item;
            item.label = (i == current ? "● " : "  ") + labels[size_t(i)];
            item.activate = [this, i, chosen] {
                menus_.pop_back();
                (*chosen)(i);
            };
            items.push_back(item);
        }
    });
    menus_.back().selected = std::max(current, 0);
}

void Menus::openTextInput(const std::string& title, const std::string& prompt, const std::string& text,
                          std::function<void(const std::string&)> accept) {
    input_.active = true;
    input_.title = title;
    input_.prompt = prompt;
    input_.text = toUtf32(text);
    input_.cursor = input_.text.size();
    input_.accept = std::move(accept);
}

void Menus::openFiles(const std::filesystem::path& folder, std::function<bool(const std::filesystem::path&)> show, const std::string& none,
                      std::function<void(const std::filesystem::path&)> chosen) {
    struct Entry {
        std::string name;
        std::filesystem::path path;
        bool folder = false;
    };
    auto entries = std::make_shared<std::vector<Entry>>();
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(folder, ec).lexically_normal();
    std::string error;
    for (std::filesystem::directory_iterator it(absolute, std::filesystem::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        const std::string name = Platform::toUtf8(it->path().filename());
        if (name.empty() || name[0] == '.') continue;
        std::error_code typeError;
        const bool isFolder = it->is_directory(typeError);
        if (!isFolder && !show(it->path())) continue;
        entries->push_back({name, it->path(), isFolder});
    }
    if (ec) error = "Cannot read " + Platform::toUtf8(absolute) + ": " + ec.message();
    std::sort(entries->begin(), entries->end(), [](const Entry& a, const Entry& b) {
        if (a.folder != b.folder) return a.folder;
        return lowerText(a.name) < lowerText(b.name);
    });
    const std::filesystem::path parent = absolute.has_relative_path() ? absolute.parent_path() : absolute;
    open(Platform::toUtf8(absolute), [this, entries, parent, absolute, error, show, none, chosen](std::vector<MenuItem>& items) {
        if (!error.empty()) {
            MenuItem item;
            item.label = error;
            item.info = true;
            item.problem = true;
            items.push_back(item);
        }
        if (parent != absolute) {
            MenuItem up;
            up.label = "../";
            up.value = "the folder above";
            up.activate = [this, parent, show, none, chosen] {
                menus_.pop_back();
                openFiles(parent, show, none, chosen);
            };
            items.push_back(up);
        }
        for (const Entry& entry : *entries) {
            MenuItem item;
            item.label = entry.name + (entry.folder ? "/" : "");
            const std::filesystem::path path = entry.path;
            if (entry.folder) {
                item.activate = [this, path, show, none, chosen] {
                    menus_.pop_back();
                    openFiles(path, show, none, chosen);
                };
            } else {
                item.activate = [this, path, chosen] {
                    closeAll();
                    chosen(path);
                };
            }
            items.push_back(item);
        }
        if (entries->empty() && error.empty()) {
            MenuItem item;
            item.label = none;
            item.info = true;
            items.push_back(item);
        }
    });
}

void Menus::select(int index) {
    if (!menus_.empty()) menus_.back().selected = index;
}

void Menus::heading(std::vector<MenuItem>& items, const std::string& title) {
    MenuItem item;
    item.label = title;
    item.heading = true;
    items.push_back(item);
}

void Menus::info(std::vector<MenuItem>& items, const std::string& text, bool problem) {
    for (const std::string& line : wrapText(text, kInfoWidth)) {
        MenuItem item;
        item.label = line;
        item.info = true;
        item.problem = problem;
        items.push_back(item);
    }
}

void Menus::item(std::vector<MenuItem>& items, const std::string& label, const std::string& value, std::function<void()> activate,
                 std::function<void(int)> adjust, const std::string& help, bool enabled) {
    MenuItem item;
    item.label = label;
    item.value = value;
    item.activate = std::move(activate);
    item.adjust = std::move(adjust);
    item.help = help;
    item.enabled = enabled;
    items.push_back(item);
}

// ---------------------------------------------------------------------------------------------
// Drawing

void Menus::draw(Screen& screen) {
    if (!menus_.empty()) drawMenu(screen, menus_.back());
    if (input_.active) drawTextInput(screen);
}

void Menus::drawMenu(Screen& screen, Menu& menu) {
    std::vector<MenuItem> items;
    menu.build(items);
    auto choosable = [&](int i) { return i >= 0 && i < int(items.size()) && !items[size_t(i)].heading && !items[size_t(i)].info; };
    if (!choosable(menu.selected)) {
        menu.selected = -1;
        for (int i = 0; i < int(items.size()) && menu.selected < 0; i++) {
            if (choosable(i)) menu.selected = i;
        }
    }
    const int screenWidth = screen.columns();
    const int screenHeight = screen.rows();
    int labelWidth = 8;
    int valueWidth = 0;
    int infoWidth = 0;
    bool anyHelp = false;
    for (const MenuItem& item : items) {
        if (item.info) infoWidth = std::max(infoWidth, textWidth(item.label));
        if (item.heading || item.info) continue;
        labelWidth = std::max(labelWidth, std::min(textWidth(item.label), 48));
        valueWidth = std::max(valueWidth, std::min(textWidth(item.value), 40));
        anyHelp = anyHelp || !item.help.empty();
    }
    const int width = std::clamp(std::max(labelWidth + valueWidth + 10, infoWidth + 6), 50, screenWidth - 4);
    const int helpRows = anyHelp ? 3 : 0;
    const int listRows = std::max(1, std::min(int(items.size()), screenHeight - 4 - 2 - (helpRows > 0 ? helpRows + 1 : 0)));
    const int height = listRows + 2 + (helpRows > 0 ? helpRows + 1 : 0);
    const int x = (screenWidth - width) / 2;
    const int y = std::max(1, (screenHeight - height) / 2);
    screen.fill(x, y, width, height, U' ', Style());
    screen.box(x, y, width, height, Style(), menu.title);
    screen.text(x + width - 36, y + height - 1, " Enter choose  ←→ change  Esc back ", ansiStyle(-1, Dim));

    if (menu.selected >= 0) {
        if (menu.selected < menu.scroll) menu.scroll = menu.selected;
        if (menu.selected >= menu.scroll + listRows) menu.scroll = menu.selected - listRows + 1;
    }
    menu.scroll = std::clamp(menu.scroll, 0, std::max(0, int(items.size()) - listRows));
    const int valueX = x + 4 + labelWidth + 2;
    for (int row = 0; row < listRows && menu.scroll + row < int(items.size()); row++) {
        const int index = menu.scroll + row;
        const MenuItem& item = items[size_t(index)];
        const int line = y + 1 + row;
        if (item.heading) {
            screen.text(x + 2, line, item.label, ansiStyle(kCyan, Bold), width - 4);
            continue;
        }
        if (item.info) {
            screen.text(x + 4, line, item.label, item.problem ? ansiStyle(kRed, Bold) : ansiStyle(-1, Dim), width - 6);
            continue;
        }
        Style itemStyle = item.enabled ? Style() : ansiStyle(-1, Dim);
        if (index == menu.selected) {
            itemStyle.attributes |= Reverse;
            screen.fill(x + 1, line, width - 2, 1, U' ', itemStyle);
        }
        screen.text(x + 4, line, item.label, itemStyle, labelWidth);
        if (!item.value.empty()) screen.text(valueX, line, item.value, itemStyle, x + width - 2 - valueX);
    }
    if (menu.scroll > 0) screen.set(x + width - 1, y + 1, U'▲', Style());
    if (menu.scroll + listRows < int(items.size())) screen.set(x + width - 1, y + listRows, U'▼', Style());
    if (helpRows > 0) {
        const int separator = y + 1 + listRows;
        for (int column = x + 1; column < x + width - 1; column++) screen.set(column, separator, U'─', Style());
        if (menu.selected >= 0) {
            const std::vector<std::string> lines = wrapText(items[size_t(menu.selected)].help, width - 4);
            for (int i = 0; i < helpRows && i < int(lines.size()); i++) screen.text(x + 2, separator + 1 + i, lines[size_t(i)], ansiStyle(-1, Dim));
        }
    }
}

void Menus::drawTextInput(Screen& screen) {
    const int width = std::min(screen.columns() - 4, std::max(60, int(input_.text.size()) + 8));
    const int height = 6;
    const int x = (screen.columns() - width) / 2;
    const int y = std::max(1, (screen.rows() - height) / 2);
    screen.fill(x, y, width, height, U' ', Style());
    screen.box(x, y, width, height, Style(), input_.title);
    screen.text(x + 2, y + 1, input_.prompt, Style(), width - 4);
    // The field, scrolled so the cursor shows.
    const int fieldWidth = width - 4;
    const int start = std::max(0, int(input_.cursor) - fieldWidth + 1);
    const Style field = ansiStyle(-1, Underline);
    screen.fill(x + 2, y + 3, fieldWidth, 1, U' ', field);
    for (int i = 0; i < fieldWidth && size_t(start + i) < input_.text.size(); i++) screen.set(x + 2 + i, y + 3, input_.text[size_t(start + i)], field);
    const int cursorX = x + 2 + int(input_.cursor) - start;
    const char32_t under = input_.cursor < input_.text.size() ? input_.text[input_.cursor] : U' ';
    screen.set(cursorX, y + 3, under, ansiStyle(-1, Reverse));
    screen.text(x + width - 27, y + height - 1, " Enter OK   Esc cancel ", ansiStyle(-1, Dim));
}

// ---------------------------------------------------------------------------------------------
// Keys

void Menus::onKey(const Key& key) {
    if (input_.active) {
        onTextInputKey(key);
    } else if (!menus_.empty()) {
        onMenuKey(key);
    }
}

void Menus::onMenuKey(const Key& key) {
    Menu& menu = menus_.back();
    std::vector<MenuItem> items;
    menu.build(items);
    const int count = int(items.size());
    auto choosable = [&](int i) { return i >= 0 && i < count && !items[size_t(i)].heading && !items[size_t(i)].info; };
    auto move = [&](int from, int direction, int steps) {
        int at = from;
        for (int moved = 0; moved < steps; moved++) {
            int next = at + direction;
            while (next >= 0 && next < count && !choosable(next)) next += direction;
            if (next < 0 || next >= count) break;
            at = next;
        }
        return at;
    };
    if (!choosable(menu.selected)) menu.selected = move(-1, 1, 1);
    const int selected = menu.selected;
    switch (key.code) {
    case Key::Up: menu.selected = move(selected, -1, 1); return;
    case Key::Down: menu.selected = move(selected, 1, 1); return;
    case Key::PageUp: menu.selected = move(selected, -1, 10); return;
    case Key::PageDown: menu.selected = move(selected, 1, 10); return;
    case Key::Home: menu.selected = move(-1, 1, 1); return;
    case Key::End: menu.selected = move(count, -1, 1); return;
    case Key::Escape:
    case Key::Backspace:
        menus_.pop_back();
        return;
    default: break;
    }
    if (key.code == Key::Char && !key.ctrl && (key.ch == U'q' || key.ch == U'Q')) {
        menus_.pop_back();
        return;
    }
    if (!choosable(selected) || !items[size_t(selected)].enabled) return;
    // Copies: what they do may close this menu or open another.
    if (key.code == Key::Left || key.code == Key::Right) {
        const std::function<void(int)> adjust = items[size_t(selected)].adjust;
        if (adjust) adjust(key.code == Key::Left ? -1 : 1);
        return;
    }
    if (key.code == Key::Enter || (key.code == Key::Char && key.ch == U' ')) {
        const std::function<void()> activate = items[size_t(selected)].activate;
        if (activate) {
            activate();
        } else if (items[size_t(selected)].adjust) {
            const std::function<void(int)> adjust = items[size_t(selected)].adjust;
            adjust(1);
        }
    }
}

void Menus::onTextInputKey(const Key& key) {
    switch (key.code) {
    case Key::Escape:
        input_.active = false;
        return;
    case Key::Enter: {
        input_.active = false;
        const std::function<void(const std::string&)> accept = input_.accept;
        if (accept) accept(toUtf8(input_.text));
        return;
    }
    case Key::Left: input_.cursor = input_.cursor > 0 ? input_.cursor - 1 : 0; return;
    case Key::Right: input_.cursor = std::min(input_.cursor + 1, input_.text.size()); return;
    case Key::Home: input_.cursor = 0; return;
    case Key::End: input_.cursor = input_.text.size(); return;
    case Key::Backspace:
        if (input_.cursor > 0) input_.text.erase(--input_.cursor, 1);
        return;
    case Key::Delete:
        if (input_.cursor < input_.text.size()) input_.text.erase(input_.cursor, 1);
        return;
    case Key::Char:
        if (key.ctrl) {
            if (key.ch == U'u') {  // As in shells: clear the line
                input_.text.clear();
                input_.cursor = 0;
            }
            return;
        }
        input_.text.insert(input_.cursor++, 1, key.ch);
        return;
    default:
        return;
    }
}

}  // namespace Tui
