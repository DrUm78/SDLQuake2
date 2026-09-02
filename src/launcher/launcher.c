#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <SDL/SDL.h>
#include <SDL/SDL_ttf.h>

#define SCREEN_WIDTH   320
#define SCREEN_HEIGHT  240
#define MAX_FILES      5
#define MAX_EXTENSIONS 4

const char *extensions[MAX_EXTENSIONS] = {".elf", ".dge", ".sh", ".opk"};

typedef struct {
    char name[256];
} FileEntry;

FileEntry files[MAX_FILES];
int file_count = 0;
int selected_index = 0;
SDL_Surface *screen = NULL;
TTF_Font *font = NULL;

// Variables for autofire
Uint32 key_press_time = 0;
int key_repeat_delay = 300;    // Initial delay (ms)
int key_repeat_interval = 100; // Repeat interval (ms)
int key_held_up = 0;
int key_held_down = 0;

int init_sdl() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "Erreur SDL_Init: %s\n", SDL_GetError());
        return 0;
    }

    screen = SDL_SetVideoMode(SCREEN_WIDTH, SCREEN_HEIGHT, 16, SDL_SWSURFACE);
    if (!screen) {
        fprintf(stderr, "Erreur SDL_SetVideoMode: %s\n", SDL_GetError());
        return 0;
    }

    SDL_ShowCursor(SDL_DISABLE);

    SDL_WM_SetCaption("Launcher", NULL);

    if (TTF_Init() == -1) {
        fprintf(stderr, "Erreur TTF_Init: %s\n", TTF_GetError());
        return 0;
    }

    font = TTF_OpenFont("dpquake_.ttf", 16);
    if (!font) {
        fprintf(stderr, "Erreur TTF_OpenFont: %s\n", TTF_GetError());
        return 0;
    }
    return 1;
}

int is_valid_extension(const char *filename) {
    for (int i = 0; i < MAX_EXTENSIONS; i++) {
        size_t len = strlen(extensions[i]);
        size_t filename_len = strlen(filename);
        if (filename_len >= len &&
            strcmp(filename + filename_len - len, extensions[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

void list_files() {
    DIR *dir = opendir(".");
    if (!dir) {
        perror("opendir");
        return;
    }

    struct dirent *entry;
    file_count = 0;
    while ((entry = readdir(dir)) != NULL && file_count < MAX_FILES) {
        if (entry->d_type == DT_REG && is_valid_extension(entry->d_name)) {
            strncpy(files[file_count].name, entry->d_name, sizeof(files[file_count].name) - 1);
            files[file_count].name[sizeof(files[file_count].name) - 1] = '\0';
            file_count++;
        }
    }
    closedir(dir);
}

void draw_text(int x, int y, const char *text, SDL_Color fg, SDL_Color bg) {
    SDL_Surface *text_surface = TTF_RenderUTF8_Shaded(font, text, fg, bg);
    if (!text_surface) return;

    SDL_Rect dest = {x, y, 0, 0};
    SDL_BlitSurface(text_surface, NULL, screen, &dest);
    SDL_FreeSurface(text_surface);
}

int get_centered_x(const char *text) {
    int text_width, text_height;
    TTF_SizeText(font, text, &text_width, &text_height);
    return (SCREEN_WIDTH - text_width) / 2;
}

char* remove_known_extension(char *filename) {
    for (int i = 0; i < MAX_EXTENSIONS; i++) {
        size_t len = strlen(extensions[i]);
        size_t filename_len = strlen(filename);
        if (filename_len >= len &&
            strcmp(filename + filename_len - len, extensions[i]) == 0) {
            filename[filename_len - len] = '\0';
            break;
        }
    }
    return filename;
}

void render_files() {
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0, 0, 0));

    SDL_Color white = {255, 255, 255};
    SDL_Color black = {0, 0, 0};
    int y = 70;

    draw_text(0, 0, "¢", white, black);
    draw_text(get_centered_x("SELECT YOUR GAME"), 20, "SELECT YOUR GAME", white, black);
    draw_text(304, 0, "¢", white, black);

    for (int i = 0; i < file_count; i++) {
        if (i == selected_index) {
            draw_text(10, y, "~", white, black);
        }

        char display_name[256];
        strncpy(display_name, files[i].name, sizeof(display_name) - 1);
        display_name[sizeof(display_name) - 1] = '\0';
        remove_known_extension(display_name);

        draw_text(30, y, display_name, white, black);
        y += 30;
    }

    draw_text(0, SCREEN_HEIGHT - 20, "¢", white, black);
    draw_text(30, SCREEN_HEIGHT - 20, "A: LAUNCH  B: QUIT", white, black);
    draw_text(304, SCREEN_HEIGHT - 20, "¢", white, black);
    SDL_Flip(screen);
}

void launch_file(const char *filename) {
    char full_path[256];
    snprintf(full_path, sizeof(full_path), "./%s", filename);

    SDL_Quit();

    if (strstr(filename, ".opk") != NULL) {
        execlp("opkrun", "opkrun", full_path, (char *)NULL);
    } else {
        execlp(full_path, filename, (char *)NULL);
    }

    exit(1);
}

int main(int argc, char *argv[]) {
    if (!init_sdl()) {
        return 1;
    }

    list_files();
    if (file_count == 0) {
        SDL_Quit();
        return 0;
    }

    int running = 1;
    SDL_Event event;
    Uint32 last_time = SDL_GetTicks();

    while (running) {
        Uint32 current_time = SDL_GetTicks();
        Uint32 delta_time = current_time - last_time;
        last_time = current_time;

        render_files();

        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    running = 0;
                    break;
                case SDL_KEYDOWN:
                    switch (event.key.keysym.sym) {
                        case SDLK_UP:
                            selected_index = (selected_index == 0) ? file_count - 1 : selected_index - 1;
                            key_held_up = 1;
                            key_press_time = current_time;
                            break;
                        case SDLK_DOWN:
                            selected_index = (selected_index == file_count - 1) ? 0 : selected_index + 1;
                            key_held_down = 1;
                            key_press_time = current_time;
                            break;
                        case SDLK_LCTRL:
                            launch_file(files[selected_index].name);
                            break;
                        case SDLK_LALT:
                            running = 0;
                            break;
                    }
                    break;
                case SDL_KEYUP:
                    switch (event.key.keysym.sym) {
                        case SDLK_UP:
                            key_held_up = 0;
                            break;
                        case SDLK_DOWN:
                            key_held_down = 0;
                            break;
                    }
                    break;
            }
        }

        // Autofire for UP d-pad
        if (key_held_up) {
            Uint32 held_time = current_time - key_press_time;
            if (held_time > key_repeat_delay &&
                (held_time - key_repeat_delay) % key_repeat_interval < delta_time) {
                selected_index = (selected_index == 0) ? file_count - 1 : selected_index - 1;
            }
        }

        // Autofire for DOWN d-pad
        if (key_held_down) {
            Uint32 held_time = current_time - key_press_time;
            if (held_time > key_repeat_delay &&
                (held_time - key_repeat_delay) % key_repeat_interval < delta_time) {
                selected_index = (selected_index == file_count - 1) ? 0 : selected_index + 1;
            }
        }

        SDL_Delay(16);
    }

    TTF_CloseFont(font);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
