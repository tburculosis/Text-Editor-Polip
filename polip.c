#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <stddef.h>
#include <asm-generic/errno-base.h>
#include <asm-generic/ioctls.h>
#include <string.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>

//***defines***//
#define POLIP_VERSION "0.0.1"
#define TAB_LENGTH 8
#define CRTL_KEY(k) ((k) & 0x1f)
#define KILO_QUIT_TIMES 3

/////////////////

//***forward declarations***// 
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));
//***global data***//

typedef struct erow { //editor row 
    int size;
    int rsize;
    char *chars;
    char *render;
} erow;

struct editorConfig {  
    int cx; //cursor position x
    int cy; //cursor position y
    int rx; //cursor position on rendered text 
    int rowOffset; //for horizontal scrolling
    int colOffset; //for vertical scrolling
    int term_rows;
    int term_cols;
    int numrows;
    erow *row;
    int dirty;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios orig_termios;
};

struct editorConfig E;

enum editorKeys {
    BACKSPACE = 127,
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

    if (buff[0] != '\x1b' || buff[1] != '[') return -1;
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


//***row operations***//
int editorRowCxtoRx(erow *row, int cx) {
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++) {
        if (row->chars[j] == '\t') {
            rx += (TAB_LENGTH -1) - (rx % TAB_LENGTH);
        }
        rx++;
    }

    return rx;
}

int editorRowRxToCx(erow *row, int rx) {
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++) {
        if (row->chars[cx] == '\t') {
            cur_rx += (TAB_LENGTH - 1) - (cur_rx % TAB_LENGTH);
        }
        cur_rx++;

        if (cur_rx > rx) return cx;
    }

    return cx;
}

void editorUpdateRow(erow *row) {
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') tabs++;
    }

    free(row->render);
    row->render = malloc(row->size + tabs * (TAB_LENGTH - 1) + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % TAB_LENGTH != 0) row->render[idx++] = ' ';
        } else { 
            row->render[idx++] = row->chars[j]; 
        }
    }

    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorInsertRow(int at, char *s, size_t len) {
    if (at < 0 || at > E.numrows) return;

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    
    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
}

void editorDelRow(int at) {
    if (at < 0 || at >= E.numrows) return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    E.numrows--;
    E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size +2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
    if (at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

//***editor operations***//

void editorInsertChar(int c) {
    if (E.cy == E.numrows) {
        editorInsertRow(E.numrows, "", 0);
    }

    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}   

void editorInsertNewLine() {
    if (E.cx == 0) {
        editorInsertRow(E.cy, "", 0);
    } else {
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    
    E.cy++;
    E.cx = 0;
}

void editorDelChar() {
    if (E.cy == E.numrows) return;
    if (E.cx == 0 && E.cy == 0) return;

    erow *row = &E.row[E.cy];
    if (E.cx > 0) {
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    } else {
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

//***file i/o operations***//

char *editorRowsToString(int *buflen) {
    int totlen = 0;
    int j;
    for (j = 0; j < E.numrows; j++) {
        totlen += E.row[j].size + 1;
    }
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;
    for (j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editorOpen(char *filename) {
    free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen returned NULL");

    char *line = NULL;
    size_t lineCapacity = 0;
    ssize_t lineLen;
    while ((lineLen = getline(&line, &lineCapacity, fp)) != -1) {
        while (lineLen > 0 && (line[lineLen - 1] == '\n' || line[lineLen - 1] == '\r'))
            lineLen--;
    
      editorInsertRow(E.numrows, line, lineLen); 
    }
    
    free(line);
    fclose(fp);
    E.dirty = 0;
}


void editorSave() {
    if (E.filename == NULL) {
        E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
        if (E.filename == NULL) {
            editorSetStatusMessage("Save aborted");
            return;
        }
    }

    int len;
    char *buff = editorRowsToString(&len);

    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buff, len) == len) {
                close(fd);
                free(buff);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buff);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

//***find***//
void editorFindCallback(char *query, int key) {
    if (key == '\r' || key == '\x1b') return;

    int i;
    for ( i = 0; i < E.numrows; i++) {
        erow *row = &E.row[i];
        char *match = strstr(row->render, query);
        if (match) {
            E.cy = i;
            E.cx = editorRowRxToCx(row, match - row->render);
            E.rowOffset = E.numrows;
            break;
        }
    }    
}


void editorFind() {
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_colOffset = E.colOffset;
    int saved_rowOffset = E.rowOffset;

    char *query = editorPrompt("Search: %s (ESC to cancel)", editorFindCallback);

    if (query)  {
        free(query);
    } else {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.colOffset = saved_colOffset;
        E.rowOffset = saved_rowOffset;
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
char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
    size_t buff_size = 128;
    char *buff = malloc(buff_size);

    size_t buff_len = 0;
    buff[0] = '\0';

    while (1) {
        editorSetStatusMessage(prompt, buff);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == DEL_KEY || c == CRTL_KEY('h') || c == BACKSPACE) {
            if (buff_len != 0) buff[--buff_len] = '\0';
        } else if (c == '\x1b') {
            editorSetStatusMessage("");
            if (callback) callback(buff, c);
            free(buff);
            return NULL;
        } else if (c == '\r') {
            if (buff_len != 0) {
                editorSetStatusMessage("");
                if (callback) callback(buff, c);
                return buff;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (buff_len == buff_size - 1) {
                buff_size *= 2;
                buff = realloc(buff, buff_size);
            }
            buff[buff_len++] = c;
            buff[buff_len] = '\0';
        }

        if (callback) callback(buff, c);
    }
}

void editorMoveCursor(int key) {
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size) {
                E.cx++; 
            } else if (row && E.cx == row->size) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0)
            { E.cy--; }
            break;
        case ARROW_DOWN:
            if (E.cy < E.numrows)
            { E.cy++; }
            break;
    }

    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowLen = row ? row->size : 0;
    if (E.cx > rowLen) {
        E.cx = rowLen;
    }
}

void editorProcessKeypress() {
    static int quit_times = KILO_QUIT_TIMES; 

    int kpress = editorReadKey();

    switch (kpress) {
        case '\r':
            editorInsertNewLine(); 
            break;

        case CRTL_KEY('q'):
            if (E.dirty && quit_times > 0) {
                editorSetStatusMessage("WARNING! File has unsaved changes."
                    "Press Ctrl-Q %d more times to quit", quit_times);
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case CRTL_KEY('s'):
           editorSave();
            break;

        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            if (E.cy < E.numrows) {
                E.cx = E.row[E.cy].size;
            } 
            break;

        case CRTL_KEY('f'):
            editorFind();
            break;

        case BACKSPACE:
        case CRTL_KEY('h'):
        case DEL_KEY: 
            if (kpress == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (kpress == PAGE_UP) {
                    E.cy = E.rowOffset;
                } else if (kpress == PAGE_DOWN) {
                    E.cy = E.rowOffset + E.term_rows - 1;
                    if (E.cy > E.numrows) E.cy = E.numrows;
                }

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

        case CRTL_KEY('l'):
        case '\x1b':
            break;

        default:
            editorInsertChar(kpress);
            break;
    }

    quit_times = KILO_QUIT_TIMES;
}


//***output***//

void editorScroll() {
    E.rx = 0;
    if (E.cy < E.numrows) {
        E.rx = editorRowCxtoRx(&E.row[E.cy], E.cx);
    }

    if (E.cy < E.rowOffset) {
        E.rowOffset = E.cy;
    }

    if (E.cy >= E.rowOffset + E.term_rows) {
        E.rowOffset = E.cy - E.term_rows + 1;
    }

    if (E.rx < E.colOffset) {
        E.colOffset = E.rx;
    }

    if (E.rx >= E.colOffset + E.term_cols) {
        E.colOffset = E.rx - E.term_cols + 1;
    }
}

void editorDrawRows(struct append_buff *ab) {
    int y; 
    for (y = 0; y < E.term_rows; y++) {
        int fileRow = y + E.rowOffset;
        if (fileRow >= E.numrows) {  
            if (E.numrows == 0 && y == E.term_rows / 3) {
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
        } else {
            int len = E.row[fileRow].rsize - E.colOffset;
            if (len < 0) len = 0;
            if (len > E.term_cols) len = E.term_cols;
            abAppend(ab, "<> ", 3);
            abAppend(ab, &E.row[fileRow].render[E.colOffset], len);
        }

        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);
        }
}

void editorDrawStatusBar(struct append_buff *ab) {
    abAppend(ab, "\x1b[7m", 4);
    char status[80];
    char rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.filename ? E.filename : "[No name]", E.numrows, E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);
    if (len > E.term_cols) len = E.term_cols;
    abAppend(ab, status, len);
    while (len < E.term_cols) {
        if (E.term_cols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        } else { 
            abAppend(ab, " ", 1);
            len++; 
        }
    }

    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct append_buff *ab) {
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.term_cols) msglen = E.term_cols;
    if (msglen && time(NULL) - E.statusmsg_time < 5) {
        abAppend(ab, E.statusmsg, msglen);
    }
}

void editorRefreshScreen() {
    editorScroll();

    struct append_buff ab = APPEND_BUFF_INIT; //no contructors in C, using preprocessor definitions instead

    abAppend(&ab, "\x1b[?25l", 6);
    // abAppend(&ab, "\x1b[2J", 4);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buff[32];
    snprintf(buff, sizeof(buff), "\x1b[%d;%dH", (E.cy - E.rowOffset) + 1, (E.rx - E.colOffset) + 4);
    abAppend(&ab, buff, strlen(buff));

    abAppend(&ab, "\x1b[?25h", 6);

    //write the buffer
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}
////////////////////



//***init***//
void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowOffset = 0;
    E.colOffset = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    if (getWindowSize(&E.term_rows, &E.term_cols) == -1)
        die("getWindowSize(int *rows, int *cols) from initEditor()");
    E.term_rows -= 2;
}
//////////////







//***entering program***//
int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-Q to Quit | Ctrl-S to save | Ctrl-F to search file");

    while(1) {
        editorRefreshScreen();
        editorProcessKeypress(); 
    } 

    return 0;
}
//////////////////////////
