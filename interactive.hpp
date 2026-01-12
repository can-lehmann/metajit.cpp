#pragma once

// Copyright 2025 Can Joshua Lehmann
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <ncurses.h>

#include "jitir.hpp"

namespace metajit {
  namespace interactive {
    class View {
    private:
      int _y = 0;
      int _x = 0;
      int _height = 0;
      int _width = 0;
    public:
      View(int y, int x, int height, int width):
        _y(y), _x(x), _height(height), _width(width) {}
      
      void write(int y, int x, char chr) {
        if (x >= 0 && y >= 0 &&
            x < _width && y < _height) {
          ::move(_y + y, _x + x);
          addch(chr);
        }
      }

      void write(int y, int x, const std::string& str) {
        int initial_x = x;
        for (char chr : str) {
          write(y, x, chr);
          if (chr == '\n') {
            y++;
            x = initial_x;
          } else {
            x++;
          }
        }
      }

      void fill(char chr) {
        for (int y = 0; y < _height; y++) {
          for (int x = 0; x < _width; x++) {
            write(y, x, chr);
          }
        }
      }
    };

    class ViewPrettyStream: public PrettyStream {
    private:
      View* _view;
      int _y = 0;
      int _x = 0;
      std::ostringstream _buffer;
      Highlight _current_highlight = Highlight::None;
    public:
      ViewPrettyStream(View* view, int y, int x):
        _view(view), _y(y), _x(x) {}
      
      std::ostream& ostream() override { return _buffer; }

      PrettyStream& operator<<(Highlight highlight) override {
        flush();
        if (_current_highlight != Highlight::None) {
          attroff(COLOR_PAIR((size_t) _current_highlight));
        }
        if (highlight != Highlight::None) {
          attron(COLOR_PAIR((size_t) highlight));
        }
        _current_highlight = highlight;
        return *this;
      }

      PrettyStream& flush() {
        for (char chr : _buffer.str()) {
          _view->write(_y, _x, chr);
          if (chr == '\n') {
            _y++;
            _x = 0;
          } else {
            _x++;
          }
        }
        _buffer = std::ostringstream();
        return *this;
      }

      void move(int y, int x) {
        flush();
        _y = y;
        _x = x;
      }
    };

    class Debugger {
    private:
      Interpreter* _interpreter = nullptr;
      NameMap<int> _lines;
      int _scroll = 0;

      std::string _status;
    public:
      Debugger(Interpreter* interpreter):
          _interpreter(interpreter),
          _lines(interpreter->section()) {
        
        initscr();
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        start_color();
        use_default_colors();

        init_pair((size_t) Highlight::Keyword, COLOR_GREEN, -1);
        init_pair((size_t) Highlight::Comment, COLOR_CYAN, -1);
        init_pair((size_t) Highlight::Constant, COLOR_YELLOW, -1);
        init_pair((size_t) Highlight::Type, COLOR_YELLOW, -1);
        init_pair((size_t) Highlight::Value, -1, -1);
        init_pair((size_t) Highlight::ArgName, COLOR_WHITE, -1);

        redraw();
        refresh();

        int input;
        while (true) {
          input = getch();
          if (input == 'w') {
            break;
          } else if (input == 'q') {
            endwin();
            exit(0);
          }

          switch (input) {
            case 'n':
              step([&]() {
                return _interpreter->step();
              });
            break;
            case 'b':
              step([&]() {
                return _interpreter->run_until(Interpreter::Event::EnterBlock);
              });
            break;
            case 'r':
              step([&]() {
                return _interpreter->run_until(Interpreter::Event::Exit);
              });
            break;
            case KEY_UP:
              _scroll++;
            break;
            case KEY_DOWN:
              _scroll--;
            break;
          }

          redraw();
        }

        endwin();
      }

      void step(std::function<Interpreter::Event()> fn) {
        Interpreter::Event event = fn();
        _status = Interpreter::event_name(event);
        scroll_into_view(_lines[_interpreter->inst()]);
      }

      void scroll_into_view(int line) {
        int y = line + _scroll;
        if (y < 0) {
          _scroll += -y;
        } else if (y >= LINES - 1) {
          // line + _scroll == LINES - 1 - 1
          // => _scroll = LINES - 2 - line
          _scroll = LINES - 2 - line;
        }
      }

      void redraw() {
        erase();
        int y = _scroll;
        View main_view(0, 0, LINES - 1, COLS);
        for (Block* block : *_interpreter->section()) {
          ViewPrettyStream stream(&main_view, y, 0);
          block->write_header(stream);
          stream.flush();
          y++;
          for (Inst* inst : *block) {
            stream.move(y, 0);
            if (inst == _interpreter->inst()) {
              stream << "> ";
            } else {
              stream << "  ";
            }
            inst->write_stmt(stream);
            if (inst->type() != Type::Void &&
                _interpreter->at(inst).type != Type::Void) {
              stream << Highlight::Comment;
              stream << " ; ";
              _interpreter->at(inst).write(stream);
              stream << Highlight::None;
            }
            stream.flush();
            _lines[inst] = y - _scroll;
            y++;
          }

          View status_bar(LINES - 1, 0, 1, COLS);
          attron(A_REVERSE);
          status_bar.fill(' ');
          status_bar.write(0, 0, _status);
          attroff(A_REVERSE);
        }
      }

      static void run(Interpreter* interpreter) {
        Debugger debugger(interpreter);
      }
    };
  }
}
