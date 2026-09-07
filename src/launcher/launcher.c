#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <SDL/SDL.h>
#include <SDL/SDL_ttf.h>

#define SCREEN_WIDTH   320
#define SCREEN_HEIGHT  240
#define MAX_FILES      6

// Structure for a menu entry: display name + command to execute
typedef struct {
    const char *display_name;  // Text to display in the menu
    const char *command;       // Command to execute when selected
    const char *required_path; // Path to check before execution (NULL if no check needed)
} MenuEntry;

// Menu entries: name, command, and required path (if any)
MenuEntry menu_entries[] = {
    {"Quake II", "./sdlquake2"},
    {"The Reckoning", "./sdlquake2 +set game xatrix"},
    {"Ground Zero", "./sdlquake2 +set game rogue"},
    {"Zaero", "./sdlquake2 +set game zaero"},
    {"Slight Mechanical Destruction", "./sdlquake2 +set game smd"},
    {"ThreeWave Capture The Flag", "./sdlquake2 +set game ctf +vid_fullscreen 1"},
};

#define MENU_ENTRIES_COUNT (sizeof(menu_entries) / sizeof(menu_entries[0]))

int file_count = MENU_ENTRIES_COUNT;
int selected_index = 0;
int pending_launch_index = -1;
SDL_Surface *screen = NULL;
TTF_Font *font = NULL;

// Autofire variables
Uint32 key_press_time = 0;
int key_repeat_delay = 350;    // Initial delay before repeating (ms)
int key_repeat_interval = 80;  // Interval between repeats (ms)
int key_held_up = 0;
int key_held_down = 0;

// Error handling variables
int error_state = 0;          // 0 = no error, 1 = error detected
int info_state = 0;           // 0 = no message, 1 = message
char error_message[256] = ""; // Custom error message
char info_message1[256] = ""; // Custom info message 1
char info_message2[256] = ""; // Custom info message 2
char info_message3[256] = ""; // Custom info message 3

// Check if a file or directory exists
int file_or_dir_exists(const char *path) {
    return (access(path, F_OK) == 0);
}

// Initialize SDL and resources
int init_sdl() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init error: %s\n", SDL_GetError());
        return 0;
    }

    screen = SDL_SetVideoMode(SCREEN_WIDTH, SCREEN_HEIGHT, 16, SDL_SWSURFACE);
    if (!screen) {
        fprintf(stderr, "SDL_SetVideoMode error: %s\n", SDL_GetError());
        return 0;
    }

    SDL_ShowCursor(SDL_DISABLE);

    SDL_WM_SetCaption("Launcher", NULL);

    if (TTF_Init() == -1) {
        fprintf(stderr, "TTF_Init error: %s\n", TTF_GetError());
        return 0;
    }

    if (file_or_dir_exists("../../opk/dpquake_.ttf")) {
        font = TTF_OpenFont("../../opk/dpquake_.ttf", 18);
    } else {
        font = TTF_OpenFont("dpquake_.ttf", 18);
    }
    TTF_SetFontHinting(font, TTF_HINTING_LIGHT);
    TTF_SetFontStyle(font, TTF_STYLE_BOLD);
    if (!font) {
        fprintf(stderr, "TTF_OpenFont error: %s\n", TTF_GetError());
        return 0;
    }
    return 1;
}

// Draw text with foreground and background colors
void draw_text(int x, int y, const char *text, SDL_Color fg, SDL_Color bg) {
    SDL_Surface *text_surface = TTF_RenderUTF8_Shaded(font, text, fg, bg);
    if (!text_surface) return;

    SDL_Rect dest = {x, y, 0, 0};
    SDL_BlitSurface(text_surface, NULL, screen, &dest);
    SDL_FreeSurface(text_surface);
}

// Get the x-coordinate to center text horizontally
int get_centered_x(const char *text) {
    int text_width, text_height;
    TTF_SizeText(font, text, &text_width, &text_height);
    return (SCREEN_WIDTH - text_width) / 2;
}

// Render the menu or error screen
void render_files() {
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0, 0, 0));

    SDL_Color white = {255, 255, 255};
    SDL_Color red   = {255, 0, 0};
    SDL_Color black = {0, 0, 0};

    if (error_state) {
        // Display error screen
        draw_text(get_centered_x("Error"), SCREEN_HEIGHT / 2 - 50, "Error", red, black);
        draw_text(get_centered_x(error_message), SCREEN_HEIGHT / 2 - 20, error_message, red, black);
        draw_text(get_centered_x("press b to go back"), SCREEN_HEIGHT / 2 + 20, "press b to go back", white, black);
    } else if (info_state) {
        // Display demo screen
        draw_text(get_centered_x(info_message1), SCREEN_HEIGHT / 2 - 50, info_message1, white, black);
        draw_text(get_centered_x(info_message2), SCREEN_HEIGHT / 2 - 20, info_message2, white, black);
        draw_text(get_centered_x(info_message3), SCREEN_HEIGHT / 2 + 20, info_message3, white, black);
    } else {
        // Display normal menu
        int y = 55;
        draw_text(get_centered_x("Select Your Game"), 20, "Select Your Game", white, black);

        for (int i = 0; i < file_count; i++) {
            if (i == selected_index) {
                draw_text(5, y, "~", white, black);
            }
            draw_text(30, y, menu_entries[i].display_name, white, black);
            y += 25;
        }

        draw_text(30, SCREEN_HEIGHT - 21, "a: launch  b: quit", white, black);
    }

    SDL_Flip(screen);
}

// Launch the selected menu entry after checking required paths
void launch_file(int index) {
    // Demo or full version?
    if (strcmp(menu_entries[index].display_name, "Quake II") == 0) {
        if (!file_or_dir_exists("/usr/local/home/.quake2/baseq2/pak0.pak")) {
            info_state = 1;
            pending_launch_index = index;
            strncpy(info_message1, "Info", sizeof(info_message1) - 1);
            strncpy(info_message2, "baseq2/pak0.pak Not Found", sizeof(info_message2) - 1);
            strncpy(info_message3, "press a to launch demo", sizeof(info_message3) - 1);
            return;
        } else {
            SDL_Quit();
            system(menu_entries[index].command);
            exit(0);
        }
    }

    // Check if the required path exists for those entries
    if (strcmp(menu_entries[index].display_name, "The Reckoning") == 0
        || strcmp(menu_entries[index].display_name, "Ground Zero") == 0
        || strcmp(menu_entries[index].display_name, "Zaero") == 0
        || strcmp(menu_entries[index].display_name, "Slight Mechanical Destruction") == 0
        || strcmp(menu_entries[index].display_name, "ThreeWave Capture The Flag") == 0) {
        if (!file_or_dir_exists("/usr/local/home/.quake2/baseq2/pak0.pak")) {
            error_state = 1;
            strncpy(error_message, "baseq2/pak0.pak Missing!", sizeof(error_message) - 1);
            return;
        }
    }

    // xatrix
    if (strcmp(menu_entries[index].display_name, "The Reckoning") == 0) {
        if (!file_or_dir_exists("/usr/local/home/.quake2/xatrix/pak0.pak")) {
            error_state = 1;
            strncpy(error_message, "xatrix/pak0.pak Missing!", sizeof(error_message) - 1);
            return;
        }
    }

    // rogue
    if (strcmp(menu_entries[index].display_name, "Ground Zero") == 0) {
        if (!file_or_dir_exists("/usr/local/home/.quake2/rogue/pak0.pak")) {
            error_state = 1;
            strncpy(error_message, "rogue/pak0.pak Missing!", sizeof(error_message) - 1);
            return;
        }
    }

    // zaero
    if (strcmp(menu_entries[index].display_name, "Zaero") == 0) {
        if (!file_or_dir_exists("/usr/local/home/.quake2/zaero/pak0.pak")) {
            error_state = 1;
            strncpy(error_message, "zaero/pak0.pak Missing!", sizeof(error_message) - 1);
            return;
        }
    }

    // smd
    if (strcmp(menu_entries[index].display_name, "Slight Mechanical Destruction") == 0) {
        if (!file_or_dir_exists("/usr/local/home/.quake2/smd/pak0.pak")) {
            error_state = 1;
            strncpy(error_message, "smd/pak0.pak Missing!", sizeof(error_message) - 1);
            return;
        }
    }

    // ctf
    if (strcmp(menu_entries[index].display_name, "ThreeWave Capture The Flag") == 0) {
        if (!file_or_dir_exists("/usr/local/home/.quake2/ctf/pak0.pak")) {
            error_state = 1;
            strncpy(error_message, "ctf/pak0.pak Missing!", sizeof(error_message) - 1);
            return;
        }
    }

    // If everything is OK, execute the command
    SDL_Quit();
    system(menu_entries[index].command);
    exit(0);
}

int main(int argc, char *argv[]) {
    if (!init_sdl()) {
        return 1;
    }

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
                    if (error_state) {
                        // In error mode, only B (LALT) is active to return to menu
                        if (event.key.keysym.sym == SDLK_LALT) {
                            error_state = 0;   // Reset error state
                            key_held_up = 0;   // Reset autofire for UP
                            key_held_down = 0; // Reset autofire for DOWN
                        }
                    } else if (info_state) {
                        if (event.key.keysym.sym == SDLK_LCTRL) {
                            // Launch the binary stored in pending_launch_index
                            SDL_Quit();
                            system(menu_entries[pending_launch_index].command);
                            exit(0);
                        /*} else if (event.key.keysym.sym == SDLK_LALT) {
                            // Back to menu if B is pressed
                            info_state = 0;
                            pending_launch_index = -1;*/
                        }
                    } else {
                        // Normal mode
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
                                launch_file(selected_index);
                                break;
                            case SDLK_LALT:
                                running = 0;
                                break;
                        }
                    }
                    break;
                case SDL_KEYUP:
                    if (!error_state) {
                        switch (event.key.keysym.sym) {
                            case SDLK_UP:
                                key_held_up = 0;
                                break;
                            case SDLK_DOWN:
                                key_held_down = 0;
                                break;
                        }
                    }
                    break;
            }
        }

        // Autofire for UP and DOWN keys (only if no error)
        if (!error_state) {
            if (key_held_up) {
                Uint32 held_time = current_time - key_press_time;
                if (held_time > key_repeat_delay &&
                    (held_time - key_repeat_delay) % key_repeat_interval < delta_time) {
                    selected_index = (selected_index == 0) ? file_count - 1 : selected_index - 1;
                }
            }

            if (key_held_down) {
                Uint32 held_time = current_time - key_press_time;
                if (held_time > key_repeat_delay &&
                    (held_time - key_repeat_delay) % key_repeat_interval < delta_time) {
                    selected_index = (selected_index == file_count - 1) ? 0 : selected_index + 1;
                }
            }
        }

        SDL_Delay(16);
    }

    TTF_CloseFont(font);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
