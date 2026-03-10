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

char editorReadKey() {
    int nread = 0;
    char read_buff = '\0';

    while((nread = read(STDIN_FILENO, &read_buff, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read in editorReadKey()");
    }

    return read_buff;
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
void editorProcessKeypress() {
    char kpress = editorReadKey();

    switch (kpress) {
        case CRTL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
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

    abAppend(&ab, "\x1b[H", 3);
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
