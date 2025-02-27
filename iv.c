//
// iv.c -- A simple terminal image viewer with vi-style navigation
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <getopt.h>

/* -------------------- CONFIG -------------------- */

#define THUMB_ROWS   5  /* Text cells occupied vertically by each thumbnail */
#define THUMB_COLS   10 /* Text cells occupied horizontally by each thumbnail */
#define SPACING_ROWS 1  /* Blank lines after each thumbnail row */
#define SPACING_COLS 2  /* Blank columns after each thumbnail column */

/*
 * Pixel dimensions for the generated thumbnails (higher = bigger).
 * We use a good downscaling filter (Lanczos).
 */
#define THUMB_PIXEL_WIDTH  180
#define THUMB_PIXEL_HEIGHT 120

/*
 * Size for "focus" mode. Could be dynamic, but we do a naive 800x600.
 */
#define FOCUS_WIDTH  800
#define FOCUS_HEIGHT 600

/*
 * Directory for storing the temporary thumbnails/focus images.
 * In a real program, you might want random filenames, a subfolder, caching, etc.
 */
#define TMP_DIR "/tmp"


/* -------------------- DATA STRUCTURES -------------------- */

typedef struct {
  char* original_path; /* The original image path */
  char* thumb_path;    /* The generated thumbnail path */
  int   generated;     /* 1 if this program created thumb_path => remove on exit */
} ImageEntry;

typedef struct {
  ImageEntry* entries;
  size_t count;
} ImageList;

typedef enum { MODE_GRID, MODE_FOCUS } ViewerMode;


/* -------------------- TERMINAL RAW MODE -------------------- */
static struct termios g_origTermios;

static void
disable_raw_mode(void)
{
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_origTermios);
}

static void
enable_raw_mode(void)
{
  if (tcgetattr(STDIN_FILENO, &g_origTermios) == -1) {
    perror("tcgetattr");
    exit(EXIT_FAILURE);
  }
  struct termios raw = g_origTermios;

  /* Turn off canonical mode and echo, so we get single key presses. */
  raw.c_lflag &= ~(ICANON | ECHO);

  /* Wait for 1 byte, no timeout. */
  raw.c_cc[VMIN]  = 1;
  raw.c_cc[VTIME] = 0;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
    perror("tcsetattr");
    exit(EXIT_FAILURE);
  }

  /* Restore on exit. */
  atexit(disable_raw_mode);
}

/* Read one keystroke (ASCII). If you want arrow keys, you'd parse ESC sequences. */
static int
read_keypress(void)
{
  unsigned char ch;
  int n = read(STDIN_FILENO, &ch, 1);
  if (n == 1)
    return ch;
  return EOF;
}


/* -------------------- IMAGE LOADING -------------------- */

static void
add_image_entry(ImageList* list, const char* path)
{
  list->entries = realloc(list->entries, sizeof(ImageEntry) * (list->count + 1));
  if (!list->entries) {
    fprintf(stderr, "Out of memory.\n");
    exit(EXIT_FAILURE);
  }
  list->entries[list->count].original_path = strdup(path);
  list->entries[list->count].thumb_path    = NULL;
  list->entries[list->count].generated     = 0;
  list->count++;
}

static int
is_directory(const char* path)
{
  struct stat st;
  if (stat(path, &st) != 0)
    return 0;
  return S_ISDIR(st.st_mode);
}

/* Load all regular files from a directory (skip hidden). */
static int
load_images_from_dir(const char* dir_path, ImageList* list)
{
  DIR* d = opendir(dir_path);
  if (!d) {
    perror("opendir");
    return -1;
  }
  struct dirent* de;
  while ((de = readdir(d)) != NULL) {
    if (de->d_name[0] == '.') {
      /* skip hidden, ., .. */
      continue;
    }
    char fullpath[4096];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", dir_path, de->d_name);

    struct stat st;
    if (stat(fullpath, &st) == 0 && S_ISREG(st.st_mode)) {
      add_image_entry(list, fullpath);
    }
  }
  closedir(d);
  return 0;
}

/* Load images from a list of file paths. */
static void
load_images_from_argv(int count, char** paths, ImageList* list)
{
  for (int i = 0; i < count; i++) {
    add_image_entry(list, paths[i]);
  }
}


/* -------------------- THUMBNAIL & FOCUS GENERATION -------------------- */

/* 
 * Generate a thumbnail for 'orig' into /tmp/ with name "iv_<orig>.thumb.png".
 */
static int
generate_thumbnail(const char* orig, char** thumb_out)
{
  const char* fname = strrchr(orig, '/');
  if (!fname)
    fname = orig;
  else
    fname++;

  char tmp[4096];
  snprintf(tmp, sizeof(tmp), "%s/iv_%s.thumb.png", TMP_DIR, fname);

  char cmd[8192];
  snprintf(cmd, sizeof(cmd),
           "magick convert \"%s\" -resize %dx%d -auto-orient -filter Lanczos \"%s\"", orig,
           THUMB_PIXEL_WIDTH, THUMB_PIXEL_HEIGHT, tmp);

  if (system(cmd) != 0) {
    fprintf(stderr, "Failed to create thumbnail for %s\n", orig);
    return -1;
  }

  *thumb_out = strdup(tmp);
  if (!*thumb_out) {
    fprintf(stderr, "Out of memory duplicating thumb path.\n");
    return -1;
  }
  return 0;
}

/* Generate a focus image of size FOCUS_WIDTH x FOCUS_HEIGHT. */
static int
generate_focus(const char* orig, char** focus_out)
{
  const char* fname = strrchr(orig, '/');
  if (!fname)
    fname = orig;
  else
    fname++;

  char tmp[4096];
  snprintf(tmp, sizeof(tmp), "%s/iv_%s.focus.png", TMP_DIR, fname);

  char cmd[8192];
  snprintf(cmd, sizeof(cmd),
           "magick convert \"%s\" -resize %dx%d -auto-orient -filter Lanczos \"%s\"", orig,
           FOCUS_WIDTH, FOCUS_HEIGHT, tmp);

  if (system(cmd) != 0) {
    fprintf(stderr, "Failed to create focus image for %s\n", orig);
    return -1;
  }
  *focus_out = strdup(tmp);
  if (!*focus_out) {
    fprintf(stderr, "Out of memory for focus path.\n");
    return -1;
  }
  return 0;
}


/* -------------------- KITTY PROTOCOL (FILE-BASED) -------------------- */

static char*
b64encode_path(const char* path)
{
  size_t len    = strlen(path);
  size_t outlen = 4 * ((len + 2) / 3);
  char* out     = calloc(outlen + 1, 1);
  if (!out)
    return NULL;

  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  size_t i = 0, j = 0;
  while (i < len) {
    unsigned int v = 0;
    int pad        = 0;
    for (int n = 0; n < 3; n++) {
      v <<= 8;
      if (i < len) {
        v |= (unsigned char)path[i++];
      } else {
        pad++;
      }
    }
    out[j++] = tbl[(v >> 18) & 0x3F];
    out[j++] = tbl[(v >> 12) & 0x3F];
    if (pad < 2) {
      out[j++] = tbl[(v >> 6) & 0x3F];
    } else {
      out[j++] = '=';
    }
    if (pad < 1) {
      out[j++] = tbl[v & 0x3F];
    } else {
      out[j++] = '=';
    }
  }
  out[j] = '\0';
  return out;
}

// 
// Display a PNG in THUMB_ROWS x THUMB_COLS at the *current cursor position*,
// telling kitty not to move the cursor afterwards (C=1).
//
static void
display_thumbnail_kitty(const char* thumb_path)
{
  if (!thumb_path) {
    printf("[?]");
    return;
  }
  char* b64 = b64encode_path(thumb_path);
  if (!b64) {
    printf("[b64-fail]");
    return;
  }

  /* a=T => transmit+display
       f=100 => PNG
       t=f => the data is a path
       c=THUMB_COLS, r=THUMB_ROWS => how many text cells
       C=1 => do not move cursor
    */
  printf("\x1b_Ga=T,f=100,t=f,c=%d,r=%d,C=1;%s\x1b\\", THUMB_COLS, THUMB_ROWS, b64);
  free(b64);
}

/* For focus, we do a naive 80x24. */
static void
display_focus_kitty(const char* focus_path)
{
  if (!focus_path)
    return;

  int c = 80, r = 24; /* could read real TTY size if you want */

  char* b64 = b64encode_path(focus_path);
  if (!b64)
    return;

  printf("\x1b_Ga=T,f=100,t=f,c=%d,r=%d,C=1;%s\x1b\\", c, r, b64);
  free(b64);
}

/* Tells kitty to remove all images from the screen. */
static void
kitty_delete_all(void)
{
  printf("\x1b_Ga=d\x1b\\");
  fflush(stdout);
}


/* -------------------- VERTICAL SCROLLING & GRID RENDER -------------------- */

static int scroll_offset = 0;

/*
 * Makes sure the selected image is visible. If not, adjust scroll_offset.
 * We pass the entire list so we can see list->count.
 */
static void
adjust_scroll_for_selection(const ImageList* list, int selected, int grid_cols)
{
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) {
    ws.ws_row = 24;
  }

  /* Each row is THUMB_ROWS plus SPACING_ROWS. We'll define row_height as: */
  int row_height = THUMB_ROWS + SPACING_ROWS;
  if (row_height < 1)
    row_height = 1;

  /* how many rows fit on screen: */
  int visible_rows = ws.ws_row / row_height;
  if (visible_rows < 1)
    visible_rows = 1;

  int total_rows = (list->count + grid_cols - 1) / grid_cols;
  int sel_row    = selected / grid_cols;

  if (sel_row < scroll_offset) {
    scroll_offset = sel_row;
  } else if (sel_row >= scroll_offset + visible_rows) {
    scroll_offset = sel_row - visible_rows + 1;
  }

  if (scroll_offset < 0)
    scroll_offset = 0;
  if (scroll_offset > total_rows - visible_rows) {
    scroll_offset = total_rows - visible_rows;
  }
  if (scroll_offset < 0)
    scroll_offset = 0;
}

/*
 * Render only the thumbnails that are within the visible rows. 
 * Place them with horizontal spacing, vertical spacing. 
 * Draw a star under the selected image in the spacing row. 
 * Then at the bottom, print "Selected: <filename>".
 */
static void
render_grid(const ImageList* list, int grid_cols, int selected)
{
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) {
    ws.ws_row = 24;
    ws.ws_col = 80;
  }

  int row_height   = THUMB_ROWS + SPACING_ROWS;
  int col_width    = THUMB_COLS + SPACING_COLS;
  int visible_rows = ws.ws_row / row_height;
  if (visible_rows < 1)
    visible_rows = 1;

  int total_rows = (list->count + grid_cols - 1) / grid_cols;
  if (scroll_offset < 0)
    scroll_offset = 0;
  if (scroll_offset > total_rows - visible_rows) {
    scroll_offset = total_rows - visible_rows;
  }
  if (scroll_offset < 0)
    scroll_offset = 0;

  /* Clear screen, go home. */
  printf("\x1b[2J\x1b[H");

  int start_row = scroll_offset;
  int end_row   = scroll_offset + visible_rows;
  if (end_row > total_rows)
    end_row = total_rows;

  /* Draw each row/col of images. */
  for (int row = start_row; row < end_row; row++) {
    for (int col = 0; col < grid_cols; col++) {
      int i = row * grid_cols + col;
      if (i >= (int)list->count)
        break;

      /* Compute the top-left cell in the terminal. */
      int screen_row = (row - start_row) * row_height + 1;
      int screen_col = col * col_width + 1;
      printf("\x1b[%d;%dH", screen_row, screen_col);

      display_thumbnail_kitty(list->entries[i].thumb_path);

      /* If this is the selected image, place a star below the thumbnail in the "spacing" row. */
      if (i == selected) {
        int star_row = screen_row + THUMB_ROWS; /* i.e. in the spacing row below. */
        /* Center the star horizontally, or just place it in the same column. */
        int star_col = screen_col + THUMB_COLS / 2;
        printf("\x1b[%d;%dH*", star_row, star_col);
      }
    }
  }

  /* Now place the selected image name at the *bottom* of the screen. */
  int bottom_line = visible_rows * row_height + 1;
  if (bottom_line < (int)ws.ws_row) {
    /* Move cursor down there. */
    printf("\x1b[%d;1H", bottom_line);
  } else {
    /* If our grid exactly fills the screen, we do one more line. */
    printf("\x1b[%d;1H", ws.ws_row);
  }

  if (selected >= 0 && selected < (int)list->count) {
    printf("Selected: %s\n", list->entries[selected].original_path);
  } else {
    printf("\n");
  }
  /* Next line for help or other info. */
  printf("[h/l/j/k: move | Enter=focus | q=quit]\n");
  fflush(stdout);
}


/* -------------------- FOCUS VIEW -------------------- */

static void
focus_view(const char* orig_path)
{
  char* focus_path = NULL;
  if (generate_focus(orig_path, &focus_path) != 0) {
    return; /* if focus gen fails, just return to the grid */
  }
  /* Clear and display focus. */
  printf("\x1b[2J\x1b[H");
  display_focus_kitty(focus_path);
  fflush(stdout);

  /* Wait for ESC or 'q' or EOF. */
  while (1) {
    int ch = read_keypress();
    if (ch == 27 || ch == 'q' || ch == EOF) {
      break;
    }
  }

  if (focus_path) {
    remove(focus_path);
    free(focus_path);
  }
}


/* -------------------- CLEANUP -------------------- */

static void
free_imagelist(ImageList* list)
{
  if (!list || !list->entries)
    return;
  for (size_t i = 0; i < list->count; i++) {
    free(list->entries[i].original_path);
    free(list->entries[i].thumb_path);
  }
  free(list->entries);
  list->entries = NULL;
  list->count   = 0;
}

/* Remove any thumbnails we generated. */
static void
remove_thumbnails(ImageList* list)
{
  if (!list || !list->entries)
    return;
  for (size_t i = 0; i < list->count; i++) {
    if (list->entries[i].generated && list->entries[i].thumb_path) {
      remove(list->entries[i].thumb_path);
    }
  }
}


/* -------------------- MAIN -------------------- */

int
main(int argc, char** argv)
{
  int grid_cols = 4; /* default columns */

  int opt;
  while ((opt = getopt(argc, argv, "c:")) != -1) {
    switch (opt) {
    case 'c':
      grid_cols = atoi(optarg);
      if (grid_cols < 1)
        grid_cols = 4;
      break;
    default:
      fprintf(stderr, "Usage: %s [-c columns] [directory or imagefiles...]\n", argv[0]);
      exit(EXIT_FAILURE);
    }
  }

  if (optind >= argc) {
    fprintf(stderr, "Usage: %s [-c columns] [directory or imagefiles...]\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  ImageList list;
  list.entries = NULL;
  list.count   = 0;

  /* Load images from either a directory or file list. */
  struct stat st;
  if (stat(argv[optind], &st) == 0 && S_ISDIR(st.st_mode)) {
    if (load_images_from_dir(argv[optind], &list) != 0) {
      fprintf(stderr, "Could not read directory.\n");
      return 1;
    }
  } else {
    /* treat everything as file paths */
    load_images_from_argv(argc - optind, &argv[optind], &list);
  }

  if (list.count == 0) {
    fprintf(stderr, "No images found.\n");
    return 1;
  }

  /* Generate thumbnails for each image. */
  for (size_t i = 0; i < list.count; i++) {
    if (generate_thumbnail(list.entries[i].original_path, &list.entries[i].thumb_path) == 0) {
      list.entries[i].generated = 1;
    }
  }

  enable_raw_mode();

  ViewerMode mode = MODE_GRID;
  int selected    = 0;
  int running     = 1;

  while (running) {
    if (mode == MODE_GRID) {
      adjust_scroll_for_selection(&list, selected, grid_cols);
      render_grid(&list, grid_cols, selected);

      int ch = read_keypress();
      if (ch == EOF) {
        running = 0;
      } else if (ch == 'q') {
        running = 0;
      } else if (ch == 'h') {
        if ((selected % grid_cols) > 0) {
          selected--;
        }
      } else if (ch == 'l') {
        if ((selected + 1) < (int)list.count && (selected % grid_cols) < (grid_cols - 1)) {
          selected++;
        }
      } else if (ch == 'k') {
        if (selected - grid_cols >= 0) {
          selected -= grid_cols;
        }
      } else if (ch == 'j') {
        if (selected + grid_cols < (int)list.count) {
          selected += grid_cols;
        }
      } else if (ch == '\n' || ch == '\r') {
        mode = MODE_FOCUS;
      }
      /* ignore other keys */

    } else if (mode == MODE_FOCUS) {
      // show the large focus view for the selected image
      focus_view(list.entries[selected].original_path);
      // return to grid mode
      mode = MODE_GRID;
    }
  }

  disable_raw_mode();
  /* Remove images from screen. */
  kitty_delete_all();

  /* Remove any temp thumbnails. */
  remove_thumbnails(&list);
  free_imagelist(&list);

  /* Clear screen on exit. */
  printf("\x1b[2J\x1b[H");
  fflush(stdout);

  return 0;
}
