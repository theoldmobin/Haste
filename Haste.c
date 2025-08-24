// "HASTE"
//
// ASCII-ART TEXT-BASED ADVENTURE RPG GAME
// BY MOBIN - 2025


// Fixed: 
//  - attack rendering no longer flashes whole screen grey (uses draw_world_with_hud)
//  - projectiles/bullets are colored yellow


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h> // multi threading (left included as in original)

#define ROWS 20
#define COLS 40
#define MAX_ENEMIES 32
#define DETECTION_RANGE 9   /* how far enemies will try to pathfind */

#define NORMAL "\033[0m"
#define BOLD "\033[1m"
#define GREY "\033[90m"
#define RESET   "\033[0m"
#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define BLUE    "\033[34m"
#define MAGENTA "\033[35m"
#define CYAN    "\033[36m"
#define WHITE   "\033[37m"
// Bright versions:
#define BRED    "\033[91m"
#define BGREEN  "\033[92m"
#define BYELLOW "\033[93m"
#define BBLUE   "\033[94m"

#define CLR()   printf("\033[H")     // cursor to home
#define CLS()   printf("\033[2J\033[H") // clear once at start

// ---------- (Optional) Soundtrack (commented in original) ----------
/*
void play_note(int freq, int duration_ms) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "beep -f %d -l %d >/dev/null 2>&1", freq, duration_ms);
    system(cmd);
}
void* soundtrack(void* arg) {
    while (1) {
        play_note(440, 200);
        usleep(300000);
        play_note(523, 200);
        usleep(300000);
    }
    return NULL;
}
*/

// ---------- Globals ----------
static int player_hp_global = 0; // made global so attack functions can call draw_world_with_hud

// ---------- Stats ----------
typedef struct {
    int VGR, STR, SPD, INT, LCK;
} Stats;

// --------- Stats Modifiers ----------
typedef struct {
    float move_speed_mult;   // affects movement delay
    float atk_speed_mult;    // affects attack cooldown
    float dmg_mult;          // affects attack damage
} ClassModifiers;

ClassModifiers mods;

// ---------- Enemies ----------
typedef struct {
    int hp;
    int dmg;
    int speed;         // how often they move (higher = faster)
    int row, col;      // top-left cell
    char shape[3];
    int is_elite;
    int is_boss;
    long last_move;    // µs timestamp for movement cooldown
    long last_hit;     // µs timestamp for last damage tick against player (unused now)
    long contact_time; // µs timestamp when first touched player (used to delay first hit & repeat)
    long attack_state_until; // µs when attack visual state should revert
    int attack_state;  // 0=idle,1=windup('x'),2=attack('X')
    int alive;
    int width;         // strlen(shape)

    int aggro;   // whether enemy has spotted the player
} Enemy;

int calc_damage(const char *cls, Stats s, ClassModifiers m) {
    int base = 0;
    if (strcmp(cls,"Sorcerer")==0) base = 3 + s.INT / 4;
    else base = 3 + s.STR / 4;
    return (int)(base * m.dmg_mult);
}

static Enemy enemies[MAX_ENEMIES];
static int enemy_count = 0;

// Player facing direction (last WASD pressed)
static char last_dir = 'd';

// ---------- Timing helpers (monotonic) ----------
static inline long now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000L;
}

// Convert SPD into movement/attack delays (player) in microseconds
int movement_delay(Stats s, float mult) {
    return (int)(20000 / (s.SPD > 0 ? s.SPD : 1) / mult);
}

int attack_delay(Stats s, float mult) {
    return (int)(10000 / (s.SPD > 0 ? s.SPD : 1) / mult);
}

// Enemy timing (in µs). Tuned to ~0.8s - 2s range:
int enemy_move_delay(const Enemy *e) { return 1500000 / (e->speed > 0 ? e->speed : 1); }
int enemy_hit_delay (const Enemy *e) { return 500000; } // 0.5s between enemy hits
int enemy_attack_flash_time(const Enemy *e) { return 200000; } // 0.2s attack flash

// ---------- Realtime input (Linux) ----------
int kbhit(void) {
    struct termios oldt, newt;
    int ch, oldf;

    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    // Disable canonical and echo to read single chars
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);

    ch = getchar();

    // Restore terminal state
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    fcntl(STDIN_FILENO, F_SETFL, oldf);

    if (ch != EOF) { ungetc(ch, stdin); return 1; }
    return 0;
}

// ---------- World helpers ----------
void draw_world(char world[ROWS][COLS+1]) {
    CLR();
    for (int r = 0; r < ROWS; r++) puts(world[r]);
}

// Draw frame with HUD and colors. Projectiles/bullets are printed in YELLOW.
void draw_world_with_hud(char world[ROWS][COLS+1], int hp) {
    CLR();
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            char tile = world[r][c];
            // projectile / attack symbols -> YELLOW
            if (tile == '^' || tile == 'v' || tile == '<' || tile == '>' ||
                tile == '|' || tile == '-' || tile == '*' || tile == '0') {
                printf(YELLOW "%c" RESET, tile);
            }
            else if (tile == 'P') {
                printf(BBLUE "P" RESET); // player lblue
            } else if (tile == 'M' || tile == 'Z' || tile == 'X' || tile == 'Y') {
                printf(BRED "%c" RESET, tile);  // normal enemies l red
            } else if (tile == 'E') {
                printf(RED "E" RESET);    // elite
            } else if (tile == 'B') {
                printf(MAGENTA "B" RESET);   // boss MAGENTA
            } else if (tile == '#') {
                printf(GREY "#" RESET);          // walls grey
            } else {
                printf("%c", tile);
            }
        }
        if (r == 0) printf(BOLD RED "   ♡ HP: %d", hp);
        printf("\n");
    }
}

// Find player on the map
int find_player(char world[ROWS][COLS+1], int *pr, int *pc) {
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            if (world[r][c] == 'P') { *pr = r; *pc = c; return 1; }
    return 0;
}

// Check if a horizontal run width 'w' at (r,c) is empty (no walls / enemies)
int is_empty_run(char world[ROWS][COLS+1], int r, int c, int w) {
    if (r < 0 || r >= ROWS) return 0;
    if (c < 0 || (c + w - 1) >= COLS) return 0;
    for (int i = 0; i < w; i++) if (world[r][c+i] != ' ') return 0;
    return 1;
}

// Place or remove enemy characters on the world grid (safe bounds-checked).
// Placement respects the enemy->attack_state: if windup show 'x', if attack show 'X'.
void place_enemy_on_world(char world[ROWS][COLS+1], const Enemy *e) {
    if (!e->alive) return;
    for (int i = 0; i < e->width; i++) {
        int cc = e->col + i;
        if (e->row >= 0 && e->row < ROWS && cc >= 0 && cc < COLS) {
            char ch = e->shape[i];
            if (e->attack_state == 1) ch = 'x';
            else if (e->attack_state == 2) ch = 'X';
            world[e->row][cc] = ch;
        }
    }
}
void remove_enemy_from_world(char world[ROWS][COLS+1], const Enemy *e) {
    for (int i = 0; i < e->width; i++) {
        int cc = e->col + i;
        if (e->row >= 0 && e->row < ROWS && cc >= 0 && cc < COLS)
            if (world[e->row][cc] != '#') world[e->row][cc] = ' ';
    }
}

// ---------- Enemy helpers ----------
// Return index of enemy occupying (r,c) or -1 if none
int find_enemy_at(int r, int c) {
    for (int i = 0; i < enemy_count; i++) {
        Enemy *e = &enemies[i];
        if (!e->alive) continue;
        if (e->row == r && c >= e->col && c < e->col + e->width) return i;
    }
    return -1;
}

// Apply damage to enemy at (r,c). If it dies, remove it from the world.
void apply_damage_at(char world[ROWS][COLS+1], int r, int c, int dmg) {
    int idx = find_enemy_at(r, c);
    if (idx < 0) return;
    enemies[idx].hp -= dmg;
    if (enemies[idx].hp <= 0) {
        enemies[idx].alive = 0;
        remove_enemy_from_world(world, &enemies[idx]);
    } else {
        // refresh world char(s) so damage flash will show correctly
        remove_enemy_from_world(world, &enemies[idx]);
        place_enemy_on_world(world, &enemies[idx]);
    }
}

/* ---------- Pathfinding (BFS) ----------
   Find shortest path from (sr,sc) to ANY cell adjacent to player (pr,pc).
   Limits BFS depth to DETECTION_RANGE to avoid searching whole map.
   Returns 1 and writes the first step into *out_r,*out_c.
   Returns 0 if no path found or player too far.
*/
int find_next_step_bfs(char world[ROWS][COLS+1], const Enemy *e, int pr, int pc, int *out_r, int *out_c) {
    int sr = e->row, sc = e->col;
    int manh = abs(sr - pr) + abs(sc - pc);
    if (manh > DETECTION_RANGE) return 0; /* too far, don't BFS */

    int total = ROWS * COLS;
    int q_r[ROWS*COLS], q_c[ROWS*COLS];
    int parent[ROWS*COLS];
    int depth[ROWS*COLS];
    char seen[ROWS][COLS];
    for (int i = 0; i < ROWS; ++i) for (int j = 0; j < COLS; ++j) seen[i][j] = 0;
    for (int i = 0; i < total; ++i) { parent[i] = -1; depth[i] = 0; }

    int head = 0, tail = 0;
    q_r[tail] = sr; q_c[tail] = sc; tail++;
    seen[sr][sc] = 1;
    parent[sr * COLS + sc] = -1;
    depth[sr * COLS + sc] = 0;

    int goal_r = -1, goal_c = -1;
    while (head < tail) {
        int r = q_r[head], c = q_c[head]; head++;
        int d = depth[r * COLS + c];
        if (d > DETECTION_RANGE) continue; /* depth exceeded */

        /* If cell adjacent to player, it's a goal */
        if (abs(r - pr) + abs(c - pc) <= 1) { goal_r = r; goal_c = c; break; }

        /* Explore 4 neighbors */
        const int dr[4] = {-1,1,0,0};
        const int dc[4] = {0,0,-1,1};
        for (int k = 0; k < 4; k++) {
            int nr = r + dr[k], nc = c + dc[k];
            if (nr < 0 || nr >= ROWS || nc < 0 || nc >= COLS) continue;
            if (seen[nr][nc]) continue;

            /* allow stepping into player's cell only if width==1 (fallback) */
            if (nr == pr && nc == pc) {
                if (e->width == 1) {
                    seen[nr][nc] = 1;
                    parent[nr * COLS + nc] = r * COLS + c;
                    depth[nr * COLS + nc] = d + 1;
                    q_r[tail] = nr; q_c[tail] = nc; tail++;
                }
                continue;
            }

            /* require the horizontal run (width) to be free for placement */
            if (!is_empty_run(world, nr, nc, e->width)) continue;

            seen[nr][nc] = 1;
            parent[nr * COLS + nc] = r * COLS + c;
            depth[nr * COLS + nc] = d + 1;
            q_r[tail] = nr; q_c[tail] = nc; tail++;
        }
    }

    if (goal_r == -1) return 0; /* no reachable adjacent cell */

    /* Reconstruct path: walk parent links from goal back to start */
    int cur_idx = goal_r * COLS + goal_c;
    int start_idx = sr * COLS + sc;

    if (cur_idx == start_idx) { *out_r = sr; *out_c = sc; return 0; }

    int prev = parent[cur_idx];
    while (prev != -1 && prev != start_idx) {
        cur_idx = prev;
        prev = parent[cur_idx];
    }
    if (prev == -1) return 0;

    *out_r = cur_idx / COLS;
    *out_c = cur_idx % COLS;
    return 1;
}

// ---------- Player attacks ----------
// NOTE: these functions now call draw_world_with_hud(..., player_hp_global)
// so we can keep colors during attack animations.

// Mage projectile: travels, damages enemies, stops on first hit
void mage_attack(char world[ROWS][COLS+1], int pr, int pc, char last_dir, int dmg) {
    int dr = 0, dc = 0; char proj = '?';
    if (last_dir == 'w') { dr = -1; proj = '|'; }
    else if (last_dir == 's') { dr =  1; proj = '|'; }
    else if (last_dir == 'a') { dc = -1; proj = '-'; }
    else if (last_dir == 'd') { dc =  1; proj = '-'; }

    int r = pr, c = pc;
    for (int step = 0; step < 6; step++) {
        r += dr; c += dc;
        if (r < 0 || r >= ROWS || c < 0 || c >= COLS) break;
        if (world[r][c] == '#') break;

        char prev = world[r][c];
        world[r][c] = proj;
        draw_world_with_hud(world, player_hp_global);

        // Damage any enemy at this position
        apply_damage_at(world, r, c, dmg);
        usleep(80000);

        // If an enemy still occupies the cell after damage, restore its char and stop
        int idx = find_enemy_at(r, c);
        if (idx >= 0 && enemies[idx].alive) {
            world[r][c] = enemies[idx].shape[c - enemies[idx].col];
            break;
        } else {
            world[r][c] = ' ';
        }
    }
}

// Gun projectile: travels, damages enemies, stops on first hit
void gun_attack(char world[ROWS][COLS+1], int pr, int pc, char last_dir, int dmg) {
    int dr = 0, dc = 0; char proj = '?';
    if (last_dir == 'w') { dr = -1; proj = '*'; }
    else if (last_dir == 's') { dr =  1; proj = '*'; }
    else if (last_dir == 'a') { dc = -1; proj = '*'; }
    else if (last_dir == 'd') { dc =  1; proj = '*'; }

    int r = pr, c = pc;
    for (int step = 0; step < 6; step++) {
        r += dr; c += dc;
        if (r < 0 || r >= ROWS || c < 0 || c >= COLS) break;
        if (world[r][c] == '#') break;

        char prev = world[r][c];
        world[r][c] = proj;
        draw_world_with_hud(world, player_hp_global);

        // Damage any enemy at this position
        apply_damage_at(world, r, c, dmg);
        usleep(80000);

        // If an enemy still occupies the cell after damage, restore its char and stop
        int idx = find_enemy_at(r, c);
        if (idx >= 0 && enemies[idx].alive) {
            world[r][c] = enemies[idx].shape[c - enemies[idx].col];
            break;
        } else {
            world[r][c] = ' ';
        }
    }
}

// Cannon projectile: travels, damages enemies, stops on first hit
void can_attack(char world[ROWS][COLS+1], int pr, int pc, char last_dir, int dmg) {
    int dr = 0, dc = 0; char proj = '?';
    if (last_dir == 'w') { dr = -1; proj = '0'; }
    else if (last_dir == 's') { dr =  1; proj = '0'; }
    else if (last_dir == 'a') { dc = -1; proj = '0'; }
    else if (last_dir == 'd') { dc =  1; proj = '0'; }

    int r = pr, c = pc;
    for (int step = 0; step < 6; step++) {
        r += dr; c += dc;
        if (r < 0 || r >= ROWS || c < 0 || c >= COLS) break;
        if (world[r][c] == '#') break;

        char prev = world[r][c];
        world[r][c] = proj;
        draw_world_with_hud(world, player_hp_global);

        // Damage any enemy at this position
        apply_damage_at(world, r, c, dmg);
        usleep(80000);

        // If an enemy still occupies the cell after damage, restore its char and stop
        int idx = find_enemy_at(r, c);
        if (idx >= 0 && enemies[idx].alive) {
            world[r][c] = enemies[idx].shape[c - enemies[idx].col];
            break;
        } else {
            world[r][c] = ' ';
        }
    }
}

// ---------- Enemies ----------
// Create enemy with randomized stats/shapes based on type
Enemy spawn_enemy(char world[ROWS][COLS+1], int is_elite, int is_boss, int pr, int pc) {
    Enemy e = {0};
    e.is_elite = is_elite;
    e.is_boss  = is_boss;
    e.alive    = 1;
    e.last_move = e.last_hit = e.contact_time = e.attack_state_until = 0;
    e.attack_state = 0;

    if (is_boss) {
        e.hp = 60; e.dmg = 10; e.speed = 5; strcpy(e.shape, "B");
    } else if (is_elite) {
        e.hp = 20 + rand()%15;
        e.dmg = 5 + rand()%3;
        e.speed = 3 + rand()%2;
        strcpy(e.shape, "EE");   // two-char elite
    } else {
        e.hp = 8 + rand()%8;
        e.dmg = 2 + rand()%2;
        e.speed = 2 + rand()%3;
        e.shape[0] = "MZXY"[rand()%4]; e.shape[1] = '\0';
    }
    e.width = (int)strlen(e.shape);

    // Place enemy away from player on empty run
    int tries = 200;
    do {
        e.row = rand() % ROWS;
        e.col = rand() % (COLS - e.width);
    } while (--tries &&
             (!is_empty_run(world, e.row, e.col, e.width) ||
              (abs(e.row - pr) + abs(e.col - pc) < 6)));

    if (!tries) e.alive = 0; // placement failed
    return e;
}

// Update enemy AI: uses BFS to find path to nearest cell adjacent to the player
void update_enemy_ai(Enemy *e, char world[ROWS][COLS+1], int pr, int pc) {
    if (!e->alive) return;
    long t = now_us();

    /* Move only when movement cooldown elapsed */
    if ((t - e->last_move) < enemy_move_delay(e)) return;

    /* If already adjacent to player, don't try to move (attack handled elsewhere) */
    if (abs(e->row - pr) + abs(e->col - pc) <= 1) {
        e->last_move = t;
        return;
    }

    // do not attempt to move when player is too far
    int dist = abs(e->row - pr) + abs(e->col - pc);
    if (!e->aggro && dist > DETECTION_RANGE) {
        e->last_move = t;
        return; // not aggro yet, ignore player
    }

    // If in range once, set aggro
    if (dist <= DETECTION_RANGE) {
        e->aggro = 1;
    }

    /* Small random wiggle for elites so they look 'strange' but not broken */
    if (e->is_elite && (rand() % 4) == 0) {
        /* try a random adjacent step (if empty) */
        const int drs[4] = {-1,1,0,0}, dcs[4] = {0,0,-1,1};
        int k = rand() % 4;
        int rr = e->row + drs[k], rc = e->col + dcs[k];
        if (is_empty_run(world, rr, rc, e->width)) {
            remove_enemy_from_world(world, e);
            e->row = rr; e->col = rc;
            place_enemy_on_world(world, e);
            e->last_move = t;
            return;
        }
        /* if random step blocked, fall through to normal pathing */
    }

    /* Prefer BFS pathfinding if player near */
    int nr, nc;
    if (find_next_step_bfs(world, e, pr, pc, &nr, &nc)) {
        if (!(nr == e->row && nc == e->col) && is_empty_run(world, nr, nc, e->width)) {
            remove_enemy_from_world(world, e);
            e->row = nr; e->col = nc;
            place_enemy_on_world(world, e);
            e->last_move = t;
            return;
        }
    }

    /* BFS failed or not used: fallback to greedy single-step (vertical then horizontal) */
    int candidates[2][2] = {
        { (pr < e->row) ? e->row - 1 : (pr > e->row) ? e->row + 1 : e->row, e->col },
        { e->row, (pc < e->col) ? e->col - 1 : (pc > e->col) ? e->col + 1 : e->col }
    };

    for (int i = 0; i < 2; i++) {
        int xr = candidates[i][0], xc = candidates[i][1];
        if (xr == e->row && xc == e->col) continue;
        if (is_empty_run(world, xr, xc, e->width)) {
            remove_enemy_from_world(world, e);
            e->row = xr; e->col = xc;
            place_enemy_on_world(world, e);
            e->last_move = t;
            break;
        }
    }
}

// Enemy attempts to damage the player with an initial delay when contact starts.
// Also manages attack_state visual animation: windup('x') and attack('X').
// Returns 1 if damage was applied this tick, 0 otherwise.
int enemy_try_attack(Enemy *e, int pr, int pc, int *player_hp, char world[ROWS][COLS+1]) {
    if (!e->alive) return 0;
    int adj = (abs(e->row - pr) + abs(e->col - pc) <= 1);
    long t = now_us();

    // First, clear visual attack state if its timer expired
    if (e->attack_state != 0 && t >= e->attack_state_until) {
        e->attack_state = 0;
        // refresh map char
        remove_enemy_from_world(world, e);
        place_enemy_on_world(world, e);
    }

    if (!adj) { // reset contact_time if no longer touching; clear pending attack-state
        e->contact_time = 0;
        return 0;
    }

    // If first contact, set contact_time and set windup visual
    if (e->contact_time == 0) {
        e->contact_time = t;
        e->attack_state = 1; // windup 'x'
        e->attack_state_until = t + enemy_hit_delay(e); // windup lasts until hit window
        // show windup on map
        remove_enemy_from_world(world, e);
        place_enemy_on_world(world, e);
        return 0;
    }

    // If enough time passed since contact_time, apply damage and show attack flash
    if ((t - e->contact_time) >= enemy_hit_delay(e)) {
        // apply damage
        *player_hp -= (e->dmg)*2; //Double damage - change '2' to adjust the amount of base damage
        // set attack-state flash
        e->attack_state = 2; // 'X'
        e->attack_state_until = t + enemy_attack_flash_time(e);
        // update contact_time to now so next hit will be after hit_delay
        e->contact_time = t;
        // update map to show 'X'
        remove_enemy_from_world(world, e);
        place_enemy_on_world(world, e);
        return 1;
    }

    return 0;
}

// ---------- MAIN ----------
int main(void) {

    srand((unsigned)time(NULL));

    // ---------- Map (ASCII art intact) ----------
    char world[ROWS][COLS+1] = {
        " ###################################### ",
        "##########         ###########      ####",
        "###                    ####           ##",
        "##                                     #",
        "#     ###                            ###",
        "#      #######      ####           #####",
        "##        ####     ##                ###",
        "###                #                  ##",
        "####                                  ##",
        "#####                         ######   #",
        "#####         ###             ##       #",
        "#####      #####              #        #",
        "####                                   #",
        "##    ###                #####         #",
        "##  #                   ###            #",
        "#   #    ######         #             ##",
        "##                                   ###",
        "###       P   ####            ###   ####",
        "####            #        ###############",
        " ###################################### ",
    };

    // ---- Class select ----
    char u_input, player_class[25];
    Stats player_stats;

    // ASCII banner
    printf(
        BYELLOW
        "\n\n"
        " █████   █████                    █████            \n"
        "░░███   ░░███                    ░░███             \n"
        " ░███    ░███   ██████    █████  ███████    ██████ \n"
        " ░███████████  ░░░░░███  ███░░  ░░░███░    ███░░███\n"
        " ░███░░░░░███   ███████ ░░█████   ░███    ░███████ \n"
        " ░███    ░███  ███░░███  ░░░░███  ░███ ███░███░░░  \n"
        " █████   █████░░████████ ██████   ░░█████ ░░██████ \n"
        "░░░░░   ░░░░░  ░░░░░░░░ ░░░░░░     ░░░░░   ░░░░░░  \n\n"
        "                       By Mobin                    \n\n\n\n"
    );

    printf(GREY "- Type any key to start -");
    getchar();
    system("clear");

    // Class selection loop
    do {
        printf(
            BOLD GREEN"Choose your class:\n"
            NORMAL BGREEN"\t1) Cannoneer   - Heavy melee class\n"
            "\t2) Gunslinger  - Light melee class\n"
            "\t3) Sorcerer - Mage class\n\n"

            GREY "Write class number to select: "
        );
        scanf(" %c", &u_input);
        switch (u_input) {
            case '1': strcpy(player_class,"Cannoneer");   player_stats=(Stats){20,20,10, 2, 5};mods = (ClassModifiers){0.8, 1.0, 1.5}; /*slower, but hits hard*/; break;
            case '2': strcpy(player_class,"Gunslinger");  player_stats=(Stats){15,15,26, 1, 6};mods = (ClassModifiers){1.3, 1.5, 0.8}; /*fast, low damage*/; break;
            case '3': strcpy(player_class,"Sorcerer"); player_stats=(Stats){10, 6,15,20, 8};mods = (ClassModifiers){1.0, 1.2, 1.2}; /*balanced, mid damage*/; break;
            default:  player_class[0]='\0';
        }
        if (!player_class[0]) puts(RED BOLD"\nInvalid choice, please try again!\n"NORMAL);
    } while (!player_class[0]);

    // Show chosen class & stats
    printf(BOLD GREEN "\nYou have chosen: %s\n\n" NORMAL, player_class);
    printf(NORMAL BGREEN"Your stats:\nVGR: %d | STR: %d | SPD: %d | INT: %d | LCK: %d\n",
           player_stats.VGR, player_stats.STR, player_stats.SPD,
           player_stats.INT, player_stats.LCK);
    printf(NORMAL GREY"\n- Type any key to Continue -");
    scanf(" %c", &u_input);
    system("clear");

    // ---- Find player ----
    int pr = 0, pc = 0;
    if (!find_player(world, &pr, &pc)) { fprintf(stderr, "No 'P' on the map!\n"); return 1; }

    // ---- Player HP ----
    player_hp_global = 30 + player_stats.VGR * 2;

    // ---- Spawn enemies (luck affects difficulty) ----
    int base_enemies = 8;
    int elites = (player_stats.LCK >= 8) ? 1 : (player_stats.LCK >= 5) ? 2 : 3;
    enemy_count = 0;

    for (int i = 0; i < base_enemies && enemy_count < MAX_ENEMIES; i++) {
        Enemy e = spawn_enemy(world, 0, 0, pr, pc);
        if (e.alive) { enemies[enemy_count++] = e; place_enemy_on_world(world, &enemies[enemy_count-1]); }
    }
    for (int i = 0; i < elites && enemy_count < MAX_ENEMIES; i++) {
        Enemy e = spawn_enemy(world, 1, 0, pr, pc);
        if (e.alive) { enemies[enemy_count++] = e; place_enemy_on_world(world, &enemies[enemy_count-1]); }
    }

    // ---- Cooldowns (player) ----
    long last_action = 0;
    int action_locked = 0;   // 0=ready, 1=cooldown active
    int cooldown_type = 0;   // 1=move, 2=attack

    // ---- Game loop ----
    while (1) {
        // Draw frame with HUD
        draw_world_with_hud(world, player_hp_global);

        // ENEMY PHASE
        for (int i = 0; i < enemy_count; i++) {
            Enemy *e = &enemies[i];
            if (!e->alive) continue;

            // Try to attack if touching player (attack logic delays the first hit and shows visuals)
            enemy_try_attack(e, pr, pc, &player_hp_global, world);
            if (player_hp_global <= 0) { system("clear"); puts("YOU DIED!"); return 0; }

            // Move toward player (BFS pathfinding + fallback greedy)
            update_enemy_ai(e, world, pr, pc);
        }

        // Win condition: all dead?
        int any_alive = 0; for (int i = 0; i < enemy_count; i++) if (enemies[i].alive) { any_alive = 1; break; }
        if (!any_alive) { system("clear"); puts("All enemies defeated!"); break; }

        // PLAYER PHASE: handle cooldown & input
        if (action_locked) {
            while (kbhit()) (void)getchar(); // drain buffer
            int delay = (cooldown_type == 1) ?
              movement_delay(player_stats, mods.move_speed_mult) :
              attack_delay(player_stats, mods.atk_speed_mult);
            if ((now_us() - last_action) > delay) action_locked = 0;
            usleep(50000);
            continue;
        }

        if (kbhit()) {
            char c = getchar();
            if (c == 'q') break;
            long t = now_us();

            // update facing immediately
            if (c == 'w' || c == 'a' || c == 's' || c == 'd') last_dir = c;

            // Movement
            if (c == 'w' || c == 'a' || c == 's' || c == 'd') {
                int nr = pr, nc = pc;
                if (c == 'w') nr--; else if (c == 's') nr++; else if (c == 'a') nc--; else if (c == 'd') nc++;
                if (nr >= 0 && nr < ROWS && nc >= 0 && nc < COLS && world[nr][nc] == ' ') {
                    world[pr][pc] = ' ';
                    pr = nr; pc = nc;
                    world[pr][pc] = 'P';
                }
                last_action = t; action_locked = 1; cooldown_type = 1;
            }
            // Attack
            else if (c == 'k' || c == 'K') {
                if (strcmp(player_class, "Sorcerer") == 0) {
                    int dmg = calc_damage(player_class, player_stats, mods);
                    mage_attack(world, pr, pc, last_dir, dmg);
                }
                else if (strcmp(player_class, "Gunslinger") == 0) {
                    int dmg = calc_damage(player_class, player_stats, mods);
                    gun_attack(world, pr, pc, last_dir, dmg);
                }
                else{
                    int dmg = calc_damage(player_class, player_stats, mods);
                    can_attack(world, pr, pc, last_dir, dmg);
                }
                last_action = t; action_locked = 1; cooldown_type = 2;
            }
        }

        usleep(40000); // frame tick
    }

    system("clear");
    printf(BRED "Goodbye, %s.\n", player_class);
    return 0;
}
