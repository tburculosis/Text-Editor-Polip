#include <asm-generic/errno-base.h>
#include <asm-generic/ioctls.h>
#include <ctype.h>
#include <string.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

//***defines***//
#define POLIP_VERSION "0.0.1"
#define CRTL_KEY(k) ((k) & 0x1f)
/////////////////




//***global data***//
struct editorConfig {  
    int cx; //cursor position x
    int cy; //cursor position y
    int term_rows;
    int term_cols;
    struct termios orig_termios;
};

struct editorConfig E;

enum editorKeys {
    ARROW_LEFT = 1000, //enum members after this incrememnt (1001, 1002, etc)
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
};

/////////////////////





//***terminal***// 
void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

void disableRawMode() {
  if ( tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1 ) 
        die("tcsetattr in disableRawMode()");
}

void enableRawMode() {
    if ( tcgetattr(STDIN_FILENO, &E.orig_termios) == -1 )
        die("tsgetattr");
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    //flipping bitflags
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if ( tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1 )
        die("tcsetattr in enableRawMode()");
}

int editorReadKey() { //type coersion? 
    int nread = 0;
    char read_buff = '\0';

    while((nread = read(STDIN_FILENO, &read_buff, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read in editorReadKey()");
    }

    if (read_buff == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
            switch (seq[1]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
    } else {
        return read_buff;
    }
}
int getCursorPosition(int *rows, int *cols) {
    char buff[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buff) - 1) {
        if (read(STDIN_FILENO, &buff[i], 1) != 1) break;
        if (buff[i] == 'R') break;
        i++;
    }

    buff[i] = '\0';

    if (buff[0] != '\x1b' || buff[0] != '[') return -1;
    if (sscanf(&buff[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;

}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) { 
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12 ) return -1;
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}
/////////////////////////

//***append buffer***//
//instead of making a lot of little, seperate write calls
//(which can be expensive and error prone)
//we store each string in a buffer, 
//writing multiple calls to STDOUT at once
struct append_buff {
    char *b;
    int len;
};

//default initialiser
#define APPEND_BUFF_INIT {NULL, 0}

void abAppend(struct append_buff *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct append_buff *ab) {
    free(ab->b);
}
///////////////////////

//***input***//
void editorMoveCursor(int key) {
    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0)
              { E.cx--; }  
            break;
        case ARROW_RIGHT:
            if (E.cx != E.term_cols - 1)
            { E.cx++; }
            break;
        case ARROW_UP:
            if (E.cy != 0)
            { E.cy--; }
            break;
        case ARROW_DOWN:
            if (E.cy != E.term_rows - 1)
            { E.cy++; }
            break;
    }
}

void editorProcessKeypress() {
    int kpress = editorReadKey();

    switch (kpress) {
        case CRTL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            E.cx = E.term_cols - 1;
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
                int times = E.term_rows;
                while (times--) {
                    editorMoveCursor(kpress == PAGE_UP ? ARROW_UP : ARROW_DOWN);
                }
            }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_RIGHT:
        case ARROW_LEFT:
            editorMoveCursor(kpress);
            break;
    }
}


//***output***//
void editorDrawRows(struct append_buff *ab) {
    int y; 
    for (y = 0; y < E.term_rows; y++) {

        if (y == E.term_rows / 3) {
            //weclome text
            char welcome[80];
            int welcomeLen = snprintf(
                welcome,
                sizeof(welcome),
                "Polip Text Editor --version %s", 
                POLIP_VERSION);
            if (welcomeLen > E.term_cols) welcomeLen = E.term_cols;
            //centre welcome text
            int padding = (E.term_cols - welcomeLen) / 2;
            if (padding) {
                abAppend(ab, "<>", 2);
                padding--;
            }
            while(padding--) abAppend(ab, " ", 1);
            abAppend(ab, welcome, welcomeLen);
        } else {
            abAppend(ab, "<>", 2);
        }

        abAppend(ab, "\x1b[K", 3);
        if (y < E.term_rows - 1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

void editorRefreshScreen() {
    struct append_buff ab = APPEND_BUFF_INIT; //no contructors in C, using preprocessor definitions instead

    abAppend(&ab, "\x1b[?25l", 6);
    // abAppend(&ab, "\x1b[2J", 4);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    char buff[32];
    snprintf(buff, sizeof(buff), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
    abAppend(&ab, buff, strlen(buff));

    abAppend(&ab, "\x1b[?25h", 6);

    //write the buffer
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}
////////////////////



//***init***//
void initEditor() {
    E.cx = 0;
    E.cy = 0;
    if (getWindowSize(&E.term_rows, &E.term_cols) == -1)
        die("getWindowSize(int *rows, int *cols) from initEditor()");
}
//////////////







//***entering program***//
int main() {
    enableRawMode();
    initEditor();

    while(1) {
        editorRefreshScreen();
        editorProcessKeypress(); 
    } 

    return 0;
}
//////////////////////////
