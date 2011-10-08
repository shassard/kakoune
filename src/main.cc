#include "window.hh"
#include "buffer.hh"
#include "file.hh"
#include "regex_selector.hh"
#include "command_manager.hh"
#include "buffer_manager.hh"
#include "register_manager.hh"
#include "selectors.hh"
#include "assert.hh"
#include "debug.hh"

#include <unordered_map>
#include <map>
#include <ncurses.h>

using namespace Kakoune;
using namespace std::placeholders;

void set_attribute(int attribute, bool on)
{
    if (on)
        attron(attribute);
    else
        attroff(attribute);
}

int nc_color(Color color)
{
    switch (color)
    {
    case Color::Black:   return COLOR_BLACK;
    case Color::Red:     return COLOR_RED;
    case Color::Green:   return COLOR_GREEN;
    case Color::Yellow:  return COLOR_YELLOW;
    case Color::Blue:    return COLOR_BLUE;
    case Color::Magenta: return COLOR_MAGENTA;
    case Color::Cyan:    return COLOR_CYAN;
    case Color::White:   return COLOR_WHITE;

    case Color::Default:
    default:
        return COLOR_BLACK;
    }
}

void set_color(Color fg_color, Color bg_color)
{
    static std::map<std::pair<Color, Color>, int> colorpairs;
    static int current_pair = -1;
    static int next_pair = 1;

    if (current_pair != -1)
        attroff(COLOR_PAIR(current_pair));

    if (fg_color == Color::Default and bg_color == Color::Default)
        return;

    std::pair<Color, Color> colorpair(fg_color, bg_color);
    auto it = colorpairs.find(colorpair);
      if (it != colorpairs.end())
    {
        current_pair = it->second;
        attron(COLOR_PAIR(it->second));
    }
    else
    {
        init_pair(next_pair, nc_color(fg_color), nc_color(bg_color));
        colorpairs[colorpair] = next_pair;
        current_pair = next_pair;
        attron(COLOR_PAIR(next_pair));
        ++next_pair;
    }
}

void draw_window(Window& window)
{
    int max_x,max_y;
    getmaxyx(stdscr, max_y, max_x);
    max_y -= 1;

    window.set_dimensions(WindowCoord(max_y, max_x));
    window.update_display_buffer();

    WindowCoord position;
    for (const DisplayAtom& atom : window.display_buffer())
    {
        const std::string content = window.buffer().string(atom.begin, atom.end);

        set_attribute(A_UNDERLINE, atom.attribute & Underline);
        set_attribute(A_REVERSE, atom.attribute & Reverse);
        set_attribute(A_BLINK, atom.attribute & Blink);
        set_attribute(A_BOLD, atom.attribute & Bold);

        set_color(atom.fg_color, atom.bg_color);

        size_t pos = 0;
        size_t end;
        while (true)
        {
            move(position.line, position.column);
            clrtoeol();
            end = content.find_first_of('\n', pos);
            std::string line = content.substr(pos, end - pos);
            addstr(line.c_str());

            if (end != std::string::npos)
            {
                addch(' ');
                position.line = position.line + 1;
                position.column = 0;
                pos = end + 1;

                if (position.line >= max_y)
                    break;
            }
            else
            {
                position.column += line.length();
                break;
            }
        }
        if (position.line >= max_y)
            break;
    }
    while (++position.line < max_y)
    {
        move(position.line, 0);
        clrtoeol();
        addch('~');
    }

    set_attribute(A_UNDERLINE, 0);
    set_attribute(A_REVERSE, 0);
    set_attribute(A_BLINK, 0);
    set_attribute(A_BOLD, 0);
    set_color(Color::Cyan, Color::Black);

    std::string status_line = window.status_line();
    move(max_y, max_x - status_line.length());
    clrtoeol();
    addstr(status_line.c_str());

    const WindowCoord& cursor_position = window.cursor_position();
    move(cursor_position.line, cursor_position.column);
}

void init_ncurses()
{
    initscr();
    cbreak();
    noecho();
    nonl();
    intrflush(stdscr, false);
    keypad(stdscr, true);
    curs_set(2);
    start_color();
}

void deinit_ncurses()
{
    endwin();
}

struct prompt_aborted {};

struct NullCompletion
{
    Completions operator() (const std::string&, size_t cursor_pos)
    {
        return Completions(cursor_pos, cursor_pos);
    }
};

std::string prompt(const std::string& text,
                   std::function<Completions (const std::string&, size_t)> completer = NullCompletion())
{
    int max_x, max_y;
    getmaxyx(stdscr, max_y, max_x);
    move(max_y-1, 0);
    addstr(text.c_str());
    clrtoeol();

    std::string result;
    size_t cursor_pos = 0;

    Completions completions;
    int current_completion = -1;
    std::string text_before_completion;

    while(true)
    {
        char c = getch();
        switch (c)
        {
        case '\r':
            return result;
        case 4:
            if (cursor_pos > 0)
                --cursor_pos;
            break;
        case 5:
            if (cursor_pos < result.length())
                ++cursor_pos;
            break;
        case 7:
            if (cursor_pos != 0)
            {
                result = result.substr(0, cursor_pos - 1)
                       + result.substr(cursor_pos, std::string::npos);

                --cursor_pos;
            }

            current_completion = -1;
            break;
        case 27:
            throw prompt_aborted();
        case '\t':
        {
            if (current_completion == -1)
            {
                completions = completer(result, cursor_pos);
                if (completions.candidates.empty())
                    break;

                text_before_completion = result.substr(completions.start,
                                                       completions.end - completions.start);
            }
            ++current_completion;

            std::string completion;
            if (current_completion >= completions.candidates.size())
            {
                if (current_completion == completions.candidates.size() and
                    std::find(completions.candidates.begin(), completions.candidates.end(), text_before_completion) == completions.candidates.end())
                    completion = text_before_completion;
                else
                {
                    current_completion = 0;
                    completion = completions.candidates[0];
                }
            }
            else
                completion = completions.candidates[current_completion];

            move(max_y-1, text.length());
            result = result.substr(0, completions.start) + completion;
            cursor_pos = completions.start + completion.length();
            break;
        }
        default:
            current_completion = -1;
            result = result.substr(0, cursor_pos) + c + result.substr(cursor_pos, std::string::npos);
            ++cursor_pos;
        }

        move(max_y - 1, text.length());
        clrtoeol();
        addstr(result.c_str());
        move(max_y - 1, text.length() + cursor_pos);
        refresh();
    }
    return result;
}

void print_status(const std::string& status)
{
    int x,y;
    getmaxyx(stdscr, y, x);
    move(y-1, 0);
    clrtoeol();
    addstr(status.c_str());
}

struct scoped_status
{
    scoped_status(const std::string& status)
    {
        print_status(status);
        refresh();
    }
    ~scoped_status()
    {
        print_status("");
        refresh();
    }
};

void do_insert(Window& window, IncrementalInserter::Mode mode)
{
    Kakoune::IncrementalInserter inserter(window, mode);
    draw_window(window);
    while(true)
    {
        const WindowCoord& pos = window.cursor_position();
        move(pos.line, pos.column);

        char c = getch();
        switch (c)
        {
        case 27:
            return;

        case 4:
            inserter.move_cursor({0, -1});
            break;
        case 5:
            inserter.move_cursor({0,  1});
            break;

        case 7:
            inserter.erase();
            break;

        case '\r':
            c = '\n';
        default:
            inserter.insert(std::string() + c);
        }
        draw_window(window);
    }
    window.clear_selections();
}

void do_go(Window& window, int count)
{
    BufferIterator target;
    if (count != 0)
        target = window.buffer().iterator_at({count, 0});
    else
    {
        char c = getch();
        switch (c)
        {
        case 'g':
        case 't':
            target = window.buffer().iterator_at({0,0});
            break;
        case 'l':
            target = window.iterator_at(window.cursor_position());
            while (not target.is_end() and *target != '\n')
                ++target;
            --target;
            break;
        case 'h':
            target = window.iterator_at(window.cursor_position());
            while (not target.is_begin() and *target != '\n')
                --target;
            ++target;
            break;
        case 'b':
            target = window.buffer().iterator_at(
                {window.buffer().line_count() - 1, 0});
            break;
            break;
        }
    }
    window.move_cursor_to(window.line_and_column_at(target));
}

Window* current_window;

void edit(const CommandParameters& params)
{
    if (params.size() != 1)
        throw wrong_argument_count();

    std::string filename = params[0];
    Buffer* buffer = NULL;
    try
    {
        buffer = create_buffer_from_file(filename);
    }
    catch (file_not_found& what)
    {
        print_status("new file " + filename);
        buffer = new Buffer(filename, Buffer::Type::File);
    }
    current_window = buffer->get_or_create_window();
}

void write_buffer(const CommandParameters& params)
{
    if (params.size() > 1)
        throw wrong_argument_count();

    Buffer& buffer = current_window->buffer();
    std::string filename = params.empty() ? buffer.name() : params[0];

    write_buffer_to_file(buffer, filename);
    buffer.notify_saved();
}

bool quit_requested = false;

template<bool force>
void quit(const CommandParameters& params)
{
    if (params.size() != 0)
        throw wrong_argument_count();

    if (not force)
    {
        for (auto& buffer : BufferManager::instance())
        {
            if (buffer.type() == Buffer::Type::File and buffer.is_modified())
            {
                print_status("modified buffer remaining");
                return;
            }
        }
    }
    quit_requested = true;
}

void show_buffer(const CommandParameters& params)
{
    if (params.size() != 1)
        throw wrong_argument_count();

    Buffer* buffer = BufferManager::instance().get_buffer(params[0]);
    if (not buffer)
        print_status("buffer " + params[0] + " does not exists");
    else
        current_window = buffer->get_or_create_window();
}

void do_command()
{
    try
    {
        auto cmdline = prompt(":", std::bind(&CommandManager::complete,
                                             &CommandManager::instance(),
                                             _1, _2));

        CommandManager::instance().execute(cmdline);
    }
    catch (prompt_aborted&) {}
}

void do_search(Window& window)
{
    try
    {
        std::string ex = prompt("/");
        if (ex.empty())
            ex = RegisterManager::instance()['/'];
        else
            RegisterManager::instance()['/'] = ex;

        window.select(RegexSelector(ex));
    }
    catch (prompt_aborted&) {}
}

void do_search_next(Window& window)
{
    std::string& ex = RegisterManager::instance()['/'];
    if (not ex.empty())
        window.select(RegexSelector(ex));
    else
        print_status("no search pattern");
}

void do_yank(Window& window, int count)
{
    RegisterManager::instance()['"'] = window.selection_content();
    window.clear_selections();
}

void do_erase(Window& window, int count)
{
    RegisterManager::instance()['"'] = window.selection_content();
    window.erase();
    window.clear_selections();
}

void do_change(Window& window, int count)
{
    RegisterManager::instance()['"'] = window.selection_content();
    do_insert(window, IncrementalInserter::Mode::Change);
    window.clear_selections();
}

template<bool append>
void do_paste(Window& window, int count)
{
    if (append)
        window.append(RegisterManager::instance()['"']);
    else
        window.insert(RegisterManager::instance()['"']);
    window.clear_selections();
}

std::unordered_map<char, std::function<void (Window& window, int count)>> keymap =
{
    { 'h', [](Window& window, int count) { window.move_cursor(WindowCoord(0, -std::max(count,1))); } },
    { 'j', [](Window& window, int count) { window.move_cursor(WindowCoord( std::max(count,1), 0)); } },
    { 'k', [](Window& window, int count) { window.move_cursor(WindowCoord(-std::max(count,1), 0)); } },
    { 'l', [](Window& window, int count) { window.move_cursor(WindowCoord(0,  std::max(count,1))); } },

    { 'H', [](Window& window, int count) { window.move_cursor(WindowCoord(0, -std::max(count,1)), true); } },
    { 'J', [](Window& window, int count) { window.move_cursor(WindowCoord( std::max(count,1), 0), true); } },
    { 'K', [](Window& window, int count) { window.move_cursor(WindowCoord(-std::max(count,1), 0), true); } },
    { 'L', [](Window& window, int count) { window.move_cursor(WindowCoord(0,  std::max(count,1)), true); } },

    { 't', [](Window& window, int count) { window.select(std::bind(select_to, _1, getch(), count, false)); } },
    { 'f', [](Window& window, int count) { window.select(std::bind(select_to, _1, getch(), count, true)); } },
    { 'T', [](Window& window, int count) { window.select(std::bind(select_to, _1, getch(), count, false), true); } },
    { 'F', [](Window& window, int count) { window.select(std::bind(select_to, _1, getch(), count, true), true); } },

    { 'd', do_erase },
    { 'c', do_change },
    { 'i', [](Window& window, int count) { do_insert(window, IncrementalInserter::Mode::Insert); } },
    { 'a', [](Window& window, int count) { do_insert(window, IncrementalInserter::Mode::Append); } },
    { 'o', [](Window& window, int count) { do_insert(window, IncrementalInserter::Mode::OpenLineBelow); } },
    { 'O', [](Window& window, int count) { do_insert(window, IncrementalInserter::Mode::OpenLineAbove); } },

    { 'g', do_go },

    { 'y', do_yank },
    { 'p', do_paste<true> },
    { 'P', do_paste<false> },

    { '%', [](Window& window, int count) { window.select([](const BufferIterator& cursor)
                                                         { return Selection(cursor.buffer().begin(), cursor.buffer().end()-1); }); } },

    { ':', [](Window& window, int count) { do_command(); } },
    { ' ', [](Window& window, int count) { window.clear_selections(); } },
    { 'w', [](Window& window, int count) { do { window.select(select_to_next_word); } while(--count > 0); } },
    { 'e', [](Window& window, int count) { do { window.select(select_to_next_word_end); } while(--count > 0); } },
    { 'b', [](Window& window, int count) { do { window.select(select_to_previous_word); } while(--count > 0); } },
    { 'W', [](Window& window, int count) { do { window.select(select_to_next_word, true); } while(--count > 0); } },
    { 'E', [](Window& window, int count) { do { window.select(select_to_next_word_end, true); } while(--count > 0); } },
    { 'B', [](Window& window, int count) { do { window.select(select_to_previous_word, true); } while(--count > 0); } },
    { '.', [](Window& window, int count) { do { window.select(select_line); } while(--count > 0); } },
    { 'm', [](Window& window, int count) { window.select(select_matching); } },
    { 'M', [](Window& window, int count) { window.select(select_matching, true); } },
    { '/', [](Window& window, int count) { do_search(window); } },
    { 'n', [](Window& window, int count) { do_search_next(window); } },
    { 'u', [](Window& window, int count) { do { if (not window.undo()) { print_status("nothing left to undo"); break; } } while(--count > 0); } },
    { 'U', [](Window& window, int count) { do { if (not window.redo()) { print_status("nothing left to redo"); break; } } while(--count > 0); } },
};

int main(int argc, char* argv[])
{
    init_ncurses();

    CommandManager  command_manager;
    BufferManager   buffer_manager;
    RegisterManager register_manager;

    command_manager.register_command(std::vector<std::string>{ "e", "edit" }, edit,
                                     PerArgumentCommandCompleter{ complete_filename });
    command_manager.register_command(std::vector<std::string>{ "q", "quit" }, quit<false>);
    command_manager.register_command(std::vector<std::string>{ "q!", "quit!" }, quit<true>);
    command_manager.register_command(std::vector<std::string>{ "w", "write" }, write_buffer,
                                     PerArgumentCommandCompleter{ complete_filename });
    command_manager.register_command(std::vector<std::string>{ "b", "buffer" }, show_buffer,
                                     PerArgumentCommandCompleter { complete_buffername });

    try
    {
        auto buffer = (argc > 1) ? create_buffer_from_file(argv[1]) : new Buffer("*scratch*", Buffer::Type::Scratch);
        current_window = buffer->get_or_create_window();

        draw_window(*current_window);
        int count = 0;
        while(not quit_requested)
        {
            try
            {
                char c = getch();
                write_debug(std::string("key ") + c + '\n');

                if (isdigit(c))
                    count = count * 10 + c - '0';
                else
                {
                    if (keymap.find(c) != keymap.end())
                    {
                        keymap[c](*current_window, count);
                        draw_window(*current_window);
                    }
                    count = 0;
                }
            }
            catch (Kakoune::runtime_error& error)
            {
                print_status(error.description());
            }
        }
        deinit_ncurses();
    }
    catch (Kakoune::exception& error)
    {
        deinit_ncurses();
        puts("uncaught exception:\n");
        puts(error.description().c_str());
        return -1;
    }
    catch (...)
    {
        deinit_ncurses();
        throw;
    }
    return 0;
}
