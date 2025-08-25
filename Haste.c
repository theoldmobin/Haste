// "HASTE"
//
// ASCII-ART TEXT-BASED ADVENTURE RPG GAME
// BY MOBIN - 2025
//
// Extended by assistant: boss 360° burst, staggered bullets, safer boss damage,
// boss teleport validation, XP drops, tuning defines.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <math.h>

#define ROWS 20
#define COLS 40
#define MAX_ENEMIES 26
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

// Small helper color aliases used in UI
#define LGREEN GREEN
#define LYELLOW YELLOW
#define LRED RED

#define CLR()   printf("\033[H")     // cursor to home
#define CLS()   printf("\033[2J\033[H") // clear once at start

// ------------------ TUNING / DEFINES ------------------
// Change these to tune boss & bullet behaviour

// Boss teleport points (row,col) - edit to set your 4 coordinates
#define BOSS_TP_R0 2
#define BOSS_TP_C0 30
#define BOSS_TP_R1 4
#define BOSS_TP_C1 7
#define BOSS_TP_R2 16
#define BOSS_TP_C2 30
#define BOSS_TP_R3 17
#define BOSS_TP_C3 7

// Teleport timing (microseconds)
#define BOSS_TELEPORT_BASE_DELAY_US 7000000  /* base wait between sequences (7s) */
#define BOSS_TELEPORT_VARIANCE_US   2000000  /* random variance added (0..2s) */

// Shots per sequence (waves)
#define BOSS_SHOT_WAVES_MIN 3
#define BOSS_SHOT_WAVES_MAX 7

// Shots per wave (360 resolution) - number of directions (e.g. 8 or 16)
#define BOSS_SHOT_DIRECTIONS 32

// Stagger between bullets (microseconds) - small delay between each bullet in burst
#define BOSS_SHOT_STAGGER_US 85000  /* 85ms between bullets -> visual spread */

// Bullet tuning
#define BOSS_BULLET_BASE_SPEED 1.5    /* higher -> move more frequently */
#define BOSS_BULLET_LIFETIME 40     /* moves before disappearing */
#define BOSS_BULLET_DAMAGE_DIV   2  /* boss->dmg divided by this to avoid one-shot */

// XP drop ranges
#define XP_NORMAL_MIN 10
#define XP_NORMAL_MAX 20
#define XP_ELITE_MIN 50
#define XP_ELITE_MAX 80
#define XP_BOSS_MIN 250
#define XP_BOSS_MAX 350

// -------------------------------------------------------

 // ---------- Globals ----------
static int player_hp_global = 0; // made global so attack functions can call draw_world_with_hud
static int current_level = 1;    // current level (1..3)
static int player_xp = 0;        // XP currency for upgrades
static int player_total_xp = 0;  // cumulative XP

// ---------- Timing helpers (monotonic) ----------
static inline long now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000L;
}

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
    int max_hp;
    int dmg;
    int speed;         // how often they move (higher = faster)
    int row, col;      // top-left cell
    char shape[4];     // single char + '\0' for normal/elite, boss 'N'
    int is_elite;
    int is_boss;
    long last_move;    // µs timestamp for movement cooldown
    long last_hit;
    long contact_time;
    long attack_state_until;
    int attack_state;
    int alive;
    int width;

    int aggro;

    // boss fields
    long boss_next_action;
    int boss_phase;
    int boss_shot_seq;
} Enemy;

static Enemy enemies[MAX_ENEMIES];
static int enemy_count = 0;

// Player facing direction (last WASD pressed)
static char last_dir = 'd';

// ---------- Bullets (boss projectiles) ----------
#define MAX_BULLETS 256
typedef struct {
    int alive;
    int row, col;
    int dr, dc;          // direction (-1,0,1)
    int damage;
    int speed;           // higher = moves more often
    long last_move;      // µs timestamp used for move cadence and start delay
    int lifetime;        // in moves remaining
    long homing_until;   // µs until which homing is active (0=no homing)
    char char_repr;      // ASCII char for drawing
} Bullet;
static Bullet bullets[MAX_BULLETS];

// ---------- Utility prototypes ----------
int kbhit(void);
void draw_world_with_hud(char world[ROWS][COLS+1], int hp);
int find_player(char world[ROWS][COLS+1], int *pr, int *pc);
int find_enemy_at(int r, int c);
void apply_damage_at(char world[ROWS][COLS+1], int r, int c, int dmg);
int find_next_step_bfs(char world[ROWS][COLS+1], const Enemy *e, int pr, int pc, int *out_r, int *out_c);
void place_enemy_on_world(char world[ROWS][COLS+1], const Enemy *e);
void remove_enemy_from_world(char world[ROWS][COLS+1], const Enemy *e);

// spawn bullet: added start_delay_us param to stagger visuals
int spawn_bullet(int r, int c, int dr, int dc, int damage, int speed, int lifetime, long start_delay_us, long homing_until_us, char repr);

// ---------- Simple damage calc ----------
int calc_damage(const char *cls, Stats s, ClassModifiers m) {
    int base = 0;
    if (strcmp(cls,"Sorcerer")==0) base = 3 + s.INT / 4;
    else base = 3 + s.STR / 4;
    return (int)(base * m.dmg_mult);
}

// ---------- Timing helpers continued ----------
int movement_delay(Stats s, float mult) {
    return (int)(20000 / (s.SPD > 0 ? s.SPD : 1) / mult);
}
int attack_delay(Stats s, float mult) {
    return (int)(10000 / (s.SPD > 0 ? s.SPD : 1) / mult);
}
int enemy_move_delay(const Enemy *e) { return 1500000 / (e->speed > 0 ? e->speed : 1); }
int enemy_hit_delay (const Enemy *e) { return 700000; }
int enemy_attack_flash_time(const Enemy *e) { return 230000; }

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
void fill_world_with_walls(char world[ROWS][COLS+1]) {
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) world[r][c] = '#';
        world[r][COLS] = '\0';
    }
}

// Map generator (same as earlier)
void generate_map(char world[ROWS][COLS+1], int *out_pr, int *out_pc) {
    fill_world_with_walls(world);

    int room_centers[8][2];
    int room_count = 0;
    int max_rooms = 6;

    for (int r = 0; r < ROWS; ++r) for (int c = 0; c < COLS; ++c) world[r][c] = '#';

    for (int i = 0; i < max_rooms; ++i) {
        int rw = 4 + rand()%8;  // width
        int rh = 3 + rand()%4;  // height
        int rx = 1 + rand() % (COLS - rw - 2);
        int ry = 1 + rand() % (ROWS - rh - 2);

        for (int y = ry; y < ry + rh; ++y)
            for (int x = rx; x < rx + rw; ++x)
                world[y][x] = ' ';

        room_centers[room_count][0] = ry + rh/2;
        room_centers[room_count][1] = rx + rw/2;
        room_count++;
    }

    for (int i = 1; i < room_count; ++i) {
        int y1 = room_centers[i-1][0], x1 = room_centers[i-1][1];
        int y2 = room_centers[i][0], x2 = room_centers[i][1];

        int cx = x1;
        while (cx != x2) { world[y1][cx] = ' '; if (x2 > cx) cx++; else cx--; }
        int cy = y1;
        while (cy != y2) { world[cy][x2] = ' '; if (y2 > cy) cy++; else cy--; }
    }

    if (room_count > 0) {
        int pr = room_centers[0][0], pc = room_centers[0][1];
        world[pr][pc] = 'P';
        *out_pr = pr; *out_pc = pc;
    } else {
        int pr = ROWS/2, pc = COLS/2;
        world[pr][pc] = 'P';
        *out_pr = pr; *out_pc = pc;
    }
}

void draw_world(char world[ROWS][COLS+1]) {
    CLR();
    for (int r = 0; r < ROWS; r++) puts(world[r]);
}

// Helper: find boss index
int find_boss_index(void) {
    for (int i = 0; i < enemy_count; ++i) if (enemies[i].alive && enemies[i].is_boss) return i;
    return -1;
}

// Render world + HUD + overlay bullets (boss projectiles).
void draw_world_with_hud(char world[ROWS][COLS+1], int hp) {
    CLR();
    int boss_idx = find_boss_index();
    long tnow = now_us();
    for (int r = 0; r < ROWS; ++r) {
        for (int c = 0; c < COLS; ++c) {
            // If a bullet occupies this tile, render it (bullets drawn above everything)
            int bullet_here = 0;
            for (int b = 0; b < MAX_BULLETS; ++b) {
                if (bullets[b].alive) {
                    // hide bullets until their start time, so delayed spawns don't sit & draw at old positions
                    if (tnow < bullets[b].last_move) continue;
                    if (bullets[b].row == r && bullets[b].col == c) {
                        printf(YELLOW "%c" RESET, bullets[b].char_repr);
                        bullet_here = 1;
                        break;
                    }
                }
            }
            if (bullet_here) continue;

            char tile = world[r][c];
            // projectile / attack symbols from player -> YELLOW (player projectiles are written into world)
            if (tile == '^' || tile == 'v' || tile == '<' || tile == '>' ||
                tile == '|' || tile == '-' || tile == '*' || tile == '0') {
                printf(YELLOW "%c" RESET, tile);
            } else if (tile == 'P') {
                printf(BOLD BBLUE "P" RESET NORMAL);
            } else if (tile >= 'a' && tile <= 'z') { // normal enemy
                printf(BRED "%c" RESET, tile);
            } else if (tile >= 'A' && tile <= 'Z' && tile != 'P') { // elite or boss letter
                printf(MAGENTA "%c" RESET, tile);
            } else if (tile == '#') {
                printf(GREY "#" RESET);
            } else {
                printf("%c", tile);
            }
        }
        if (r == 0) {
            printf(BOLD RED "   ♡ HP: %d" RESET, hp);
            printf("  " BOLD BYELLOW "❇️ XP: %d" RESET, player_xp);
            if (boss_idx != -1) {
                int bhp = enemies[boss_idx].hp;
                printf("  " BOLD MAGENTA "✴️ Boss HP: %d/%d" RESET, bhp, enemies[boss_idx].max_hp);
            }
        }
        printf("\n");
    }
}

// ---------- Find / placement helpers ----------
int find_player(char world[ROWS][COLS+1], int *pr, int *pc) {
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) if (world[r][c] == 'P') { *pr = r; *pc = c; return 1; }
    return 0;
}

int is_empty_run(char world[ROWS][COLS+1], int r, int c, int w) {
    if (r < 0 || r >= ROWS) return 0;
    if (c < 0 || (c + w - 1) >= COLS) return 0;
    for (int i = 0; i < w; i++) if (world[r][c+i] != ' ') return 0;
    return 1;
}

int is_traversable_for_pathfinding(char world[ROWS][COLS+1], int r, int c, int w) {
    if (r < 0 || r >= ROWS) return 0;
    if (c < 0 || (c + w - 1) >= COLS) return 0;
    for (int i = 0; i < w; i++) if (world[r][c+i] == '#') return 0;
    return 1;
}

void place_enemy_on_world(char world[ROWS][COLS+1], const Enemy *e) {
    if (!e->alive) return;
    for (int i = 0; i < e->width; ++i) {
        int cc = e->col + i;
        if (e->row >= 0 && e->row < ROWS && cc >= 0 && cc < COLS) {
            char ch = e->shape[i];
            // keep boss glyph stable; don't swap to x/X for boss
            if (!e->is_boss) {
                if (e->attack_state == 1) ch = 'x';
                else if (e->attack_state == 2) ch = 'X';
            }
            world[e->row][cc] = ch;
        }
    }
}
void remove_enemy_from_world(char world[ROWS][COLS+1], const Enemy *e) {
    for (int i = 0; i < e->width; ++i) {
        int cc = e->col + i;
        if (e->row >= 0 && e->row < ROWS && cc >= 0 && cc < COLS)
            if (world[e->row][cc] != '#') world[e->row][cc] = ' ';
    }
}

// Return index of enemy occupying (r,c) or -1 if none
int find_enemy_at(int r, int c) {
    for (int i = 0; i < enemy_count; i++) {
        Enemy *e = &enemies[i];
        if (!e->alive) continue;
        if (e->row == r && c >= e->col && c < e->col + e->width) return i;
    }
    return -1;
}

// Apply damage to enemy at (r,c). If it dies, remove it and award XP.
void apply_damage_at(char world[ROWS][COLS+1], int r, int c, int dmg) {
    int idx = find_enemy_at(r, c);
    if (idx < 0) return;
    enemies[idx].hp -= dmg;

    if (enemies[idx].hp <= 0) {
        int xp_gain = 0;
        if (enemies[idx].is_boss) {
            xp_gain = XP_BOSS_MIN + (rand() % (XP_BOSS_MAX - XP_BOSS_MIN + 1));
        } else if (enemies[idx].is_elite) {
            xp_gain = XP_ELITE_MIN + (rand() % (XP_ELITE_MAX - XP_ELITE_MIN + 1));
        } else {
            xp_gain = XP_NORMAL_MIN + (rand() % (XP_NORMAL_MAX - XP_NORMAL_MIN + 1));
        }
        player_xp += xp_gain;
        player_total_xp += xp_gain;

        enemies[idx].alive = 0;
        remove_enemy_from_world(world, &enemies[idx]);
    } else {
        remove_enemy_from_world(world, &enemies[idx]);
        place_enemy_on_world(world, &enemies[idx]);
    }
}

// ---------- Pathfinding (BFS) ----------
int find_next_step_bfs(char world[ROWS][COLS+1], const Enemy *e, int pr, int pc, int *out_r, int *out_c) {
    int sr = e->row, sc = e->col;
    int manh = abs(sr - pr) + abs(sc - pc);
    if (manh > DETECTION_RANGE && !e->is_boss && !e->is_elite) return 0; /* small enemies may be less eager */
    if (e->is_elite && manh > DETECTION_RANGE*2) return 0;

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
        if (d > DETECTION_RANGE*2 && !e->is_boss) continue;

        if (abs(r - pr) + abs(c - pc) <= 1) { goal_r = r; goal_c = c; break; }

        const int dr[4] = {-1,1,0,0};
        const int dc[4] = {0,0,-1,1};
        for (int k = 0; k < 4; k++) {
            int nr = r + dr[k], nc = c + dc[k];
            if (nr < 0 || nr >= ROWS || nc < 0 || nc >= COLS) continue;
            if (seen[nr][nc]) continue;

            if (nr == pr && nc == pc) {
                if (e->width == 1) {
                    seen[nr][nc] = 1;
                    parent[nr * COLS + nc] = r * COLS + c;
                    depth[nr * COLS + nc] = d + 1;
                    q_r[tail] = nr; q_c[tail] = nc; tail++;
                }
                continue;
            }

            if (!is_traversable_for_pathfinding(world, nr, nc, e->width)) continue;

            seen[nr][nc] = 1;
            parent[nr * COLS + nc] = r * COLS + c;
            depth[nr * COLS + nc] = d + 1;
            q_r[tail] = nr; q_c[tail] = nc; tail++;
        }
    }

    if (goal_r == -1) return 0;

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

// ---------- Player attacks (unchanged except they render using draw_world_with_hud) ----------
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

        // no per-step redraw/sleep to avoid frame hitch during boss spam
        char prev = world[r][c];
        world[r][c] = proj;

        apply_damage_at(world, r, c, dmg);

        int idx = find_enemy_at(r, c);
        if (idx >= 0 && enemies[idx].alive) {
            world[r][c] = enemies[idx].shape[c - enemies[idx].col];
            break;
        } else {
            world[r][c] = prev == ' ' ? ' ' : prev;
        }
    }
}

void gun_attack(char world[ROWS][COLS+1], int pr, int pc, char last_dir, int dmg) {
    int dr = 0, dc = 0; char proj = '*';
    if (last_dir == 'w') { dr = -1; }
    else if (last_dir == 's') { dr =  1; }
    else if (last_dir == 'a') { dc = -1; }
    else if (last_dir == 'd') { dc =  1; }

    int r = pr, c = pc;
    for (int step = 0; step < 6; step++) {
        r += dr; c += dc;
        if (r < 0 || r >= ROWS || c < 0 || c >= COLS) break;
        if (world[r][c] == '#') break;

        char prev = world[r][c];
        world[r][c] = proj;

        apply_damage_at(world, r, c, dmg);

        int idx = find_enemy_at(r, c);
        if (idx >= 0 && enemies[idx].alive) {
            world[r][c] = enemies[idx].shape[c - enemies[idx].col];
            break;
        } else {
            world[r][c] = prev == ' ' ? ' ' : prev;
        }
    }
}

void can_attack(char world[ROWS][COLS+1], int pr, int pc, char last_dir, int dmg) {
    int dr = 0, dc = 0; char proj = '0';
    if (last_dir == 'w') { dr = -1; }
    else if (last_dir == 's') { dr =  1; }
    else if (last_dir == 'a') { dc = -1; }
    else if (last_dir == 'd') { dc =  1; }

    int r = pr, c = pc;
    for (int step = 0; step < 6; step++) {
        r += dr; c += dc;
        if (r < 0 || r >= ROWS || c < 0 || c >= COLS) break;
        if (world[r][c] == '#') break;

        char prev = world[r][c];
        world[r][c] = proj;

        apply_damage_at(world, r, c, dmg);

        int idx = find_enemy_at(r, c);
        if (idx >= 0 && enemies[idx].alive) {
            world[r][c] = enemies[idx].shape[c - enemies[idx].col];
            break;
        } else {
            world[r][c] = prev == ' ' ? ' ' : prev;
        }
    }
}

// ---------- Enemy spawning: new character system (a..z normals, A..Z elites). ----------
Enemy spawn_enemy(char world[ROWS][COLS+1], int is_elite, int is_boss, int pr, int pc) {
    Enemy e = {0};
    e.is_elite = is_elite;
    e.is_boss  = is_boss;
    e.alive    = 1;
    e.last_move = e.last_hit = e.contact_time = e.attack_state_until = e.boss_next_action = 0;
    e.attack_state = 0;
    e.aggro = 0;
    e.boss_phase = 1;

    if (is_boss) {
        e.max_hp = e.hp = 220 + current_level * 80;
        e.dmg = 12 + current_level * 4;
        e.speed = 3;
        strcpy(e.shape, "N"); // boss visual 'N' (distinct)
    } else {
        // Strength rank 0..25
        int rank_base = (current_level - 1) * 6; // level scales
        if (!is_elite) {
            int rank = rank_base + rand()%6; if (rank < 0) rank = 0; if (rank > 25) rank = 25;
            e.max_hp = e.hp = 5 + rank*2 + current_level; // HP grows with rank
            e.dmg = 1 + rank/6 + current_level/2;
            e.speed = 2 + (rank/10) + (rand()%2);
            char ch = 'a' + rank;
            e.shape[0] = ch; e.shape[1] = '\0';
        } else {
            int rank = rank_base + 3 + rand()%6; if (rank < 0) rank = 0; if (rank > 25) rank = 25;
            e.max_hp = e.hp = 18 + rank*3 + current_level*2;
            e.dmg = 4 + rank/4 + current_level;
            e.speed = 3 + (rank/12);
            char ch = 'A' + rank;
            e.shape[0] = ch; e.shape[1] = '\0';
            e.is_elite = 1;
        }
    }
    e.width = (int)strlen(e.shape);

    // Place enemy away from player
    int tries = 400;
    do {
        e.row = rand() % ROWS;
        e.col = rand() % (COLS - e.width);
    } while (--tries &&
             (!is_empty_run(world, e.row, e.col, e.width) ||
              (abs(e.row - pr) + abs(e.col - pc) < 4)));
    if (!tries) e.alive = 0;
    return e;
}

// ---------- Enemy AI ----------
void update_enemy_ai(Enemy *e, char world[ROWS][COLS+1], int pr, int pc) {
    if (!e->alive) return;
    long t = now_us();

    if (e->is_boss) return;

    if ((t - e->last_move) < enemy_move_delay(e)) return;

    if (abs(e->row - pr) + abs(e->col - pc) <= 1) {
        e->last_move = t;
        return;
    }

    int dist = abs(e->row - pr) + abs(e->col - pc);
    int effective_detection = DETECTION_RANGE;
    if (e->is_elite) effective_detection = DETECTION_RANGE * 2;
    if (!e->is_elite && !e->aggro && dist > effective_detection) {
        e->last_move = t;
        return;
    }

    if (dist <= effective_detection || e->is_elite) e->aggro = 1;

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

    int best_r = e->row, best_c = e->col;
    int best_dist = abs(e->row - pr) + abs(e->col - pc);
    const int drs[4] = {-1,1,0,0};
    const int dcs[4] = {0,0,-1,1};
    for (int i = 0; i < 4; ++i) {
        int rr = e->row + drs[i], rc = e->col + dcs[i];
        if (!is_empty_run(world, rr, rc, e->width)) continue;
        int dd = abs(rr - pr) + abs(rc - pc);
        if (dd < best_dist) { best_dist = dd; best_r = rr; best_c = rc; }
    }
    if (!(best_r == e->row && best_c == e->col)) {
        remove_enemy_from_world(world, e);
        e->row = best_r; e->col = best_c;
        place_enemy_on_world(world, e);
        e->last_move = t;
    }
}

// Enemy attack logic
int enemy_try_attack(Enemy *e, int pr, int pc, int *player_hp, char world[ROWS][COLS+1]) {
    if (!e->alive) return 0;
    int adj = (abs(e->row - pr) + abs(e->col - pc) <= 1);
    long t = now_us();

    if (e->attack_state != 0 && t >= e->attack_state_until) {
        e->attack_state = 0;
        remove_enemy_from_world(world, e);
        place_enemy_on_world(world, e);
    }

    if (!adj) { e->contact_time = 0; return 0; }

    if (e->contact_time == 0) {
        e->contact_time = t;
        e->attack_state = 1;
        e->attack_state_until = t + enemy_hit_delay(e);
        remove_enemy_from_world(world, e);
        place_enemy_on_world(world, e);
        return 0;
    }

    if ((t - e->contact_time) >= enemy_hit_delay(e)) {
        int applied = e->is_boss ? (e->dmg * 3) : (e->dmg * 2);
        *player_hp -= applied;
        e->attack_state = 2;
        e->attack_state_until = t + enemy_attack_flash_time(e);
        e->contact_time = t;
        remove_enemy_from_world(world, e);
        place_enemy_on_world(world, e);
        return 1;
    }
    return 0;
}

// ---------- Boss and bullets behavior ----------

void bullets_init(void) {
    for (int i = 0; i < MAX_BULLETS; ++i) bullets[i].alive = 0;
}

// spawn_bullet with start_delay_us (can be 0) and homing_until_us (absolute µs timestamp or 0)
int spawn_bullet(int r, int c, int dr, int dc, int damage, int speed, int lifetime, long start_delay_us, long homing_until_us, char repr) {
    for (int i = 0; i < MAX_BULLETS; ++i) {
        if (!bullets[i].alive) {
            bullets[i].alive = 1;
            bullets[i].row = r;
            bullets[i].col = c;
            bullets[i].dr = dr; bullets[i].dc = dc;
            bullets[i].damage = damage;
            bullets[i].speed = speed;
            bullets[i].last_move = now_us() + start_delay_us; // delayed start (also used as "visible after")
            bullets[i].lifetime = lifetime;
            bullets[i].homing_until = homing_until_us;
            bullets[i].char_repr = repr;
            return 1;
        }
    }
    return 0;
}

// Move bullets: return 1 if player hit
int update_bullets(char world[ROWS][COLS+1], int *player_hp, int pr, int pc) {
    int player_hit = 0;
    long t = now_us();
    for (int i = 0; i < MAX_BULLETS; ++i) {
        if (!bullets[i].alive) continue;

        // respect start delay: don't move until visible-after time arrives
        if (t < bullets[i].last_move) continue;

        int move_delay = 200000 / (bullets[i].speed > 0 ? bullets[i].speed : 1);
        if ((t - bullets[i].last_move) < move_delay) continue;

        bullets[i].last_move = t;
        if (bullets[i].homing_until && t <= bullets[i].homing_until) {
            int vr = pr - bullets[i].row;
            int vc = pc - bullets[i].col;
            if (vr < 0) bullets[i].dr = -1; else if (vr > 0) bullets[i].dr = 1; else bullets[i].dr = 0;
            if (vc < 0) bullets[i].dc = -1; else if (vc > 0) bullets[i].dc = 1; else bullets[i].dc = 0;
        }

        int nr = bullets[i].row + bullets[i].dr;
        int nc = bullets[i].col + bullets[i].dc;

        bullets[i].lifetime--;
        if (bullets[i].lifetime <= 0) { bullets[i].alive = 0; continue; }

        if (nr < 0 || nr >= ROWS || nc < 0 || nc >= COLS) { bullets[i].alive = 0; continue; }
        if (world[nr][nc] == '#') { bullets[i].alive = 0; continue; }

        if (nr == pr && nc == pc) {
            *player_hp -= bullets[i].damage;
            bullets[i].alive = 0;
            player_hit = 1;
            continue;
        }

        bullets[i].row = nr; bullets[i].col = nc;
    }
    return player_hit;
}

// helper: check whether a straight ray from (r,c) in direction (dr,dc) is clear for 'dist' tiles (no walls)
int is_clear_dir(const char world[ROWS][COLS+1], int r, int c, int dr, int dc, int dist) {
    for (int i = 1; i <= dist; ++i) {
        int rr = r + dr * i;
        int cc = c + dc * i;
        if (rr < 0 || rr >= ROWS || cc < 0 || cc >= COLS) return 0;
        if (world[rr][cc] == '#') return 0;
    }
    return 1;
}

// Find a valid teleport among the configured 4 teleport coords that has at least one direction clear
int pick_valid_teleport_from_table(const char world[ROWS][COLS+1], int pr, int pc, int *out_r, int *out_c) {
    int table[4][2] = {
        { BOSS_TP_R0, BOSS_TP_C0 },
        { BOSS_TP_R1, BOSS_TP_C1 },
        { BOSS_TP_R2, BOSS_TP_C2 },
        { BOSS_TP_R3, BOSS_TP_C3 }
    };
    int clear_dist = 1; // require at least immediate neighbor free so bullets can spawn out
    int best_r = -1, best_c = -1, best_dist = -1;
    for (int i = 0; i < 4; ++i) {
        int rr = table[i][0], cc = table[i][1];
        if (rr <= 0 || rr >= ROWS-1 || cc <= 0 || cc >= COLS-1) continue;
        if (world[rr][cc] == '#') continue;
        int north = is_clear_dir(world, rr, cc, -1, 0, clear_dist);
        int south = is_clear_dir(world, rr, cc, 1, 0, clear_dist);
        int west  = is_clear_dir(world, rr, cc, 0, -1, clear_dist);
        int east  = is_clear_dir(world, rr, cc, 0, 1, clear_dist);
        if (!(north || south || west || east)) continue;
        int d = abs(rr - pr) + abs(cc - pc);
        if (d > best_dist) { best_dist = d; best_r = rr; best_c = cc; }
    }
    if (best_r != -1) { *out_r = best_r; *out_c = best_c; return 1; }
    return 0;
}

// Boss behavior: teleport to one of configured coords (if valid) then shoot 360° in staggered bullets
void update_boss_behavior(Enemy *boss, char world[ROWS][COLS+1], int pr, int pc) {
    if (!boss || !boss->alive) return;
    long t = now_us();

    if (boss->boss_next_action == 0) boss->boss_next_action = t + 800000;

    if (t < boss->boss_next_action) return;

    // pick a teleport coord from table (prefer farthest from player) and validate it
    int chosen_r = -1, chosen_c = -1;
    if (!pick_valid_teleport_from_table(world, pr, pc, &chosen_r, &chosen_c)) {
        // fallback: search whole map for any tile with at least one clear neighbor
        int best_dist = -1;
        for (int rr = 1; rr < ROWS-1; ++rr) {
            for (int cc = 1; cc < COLS-1; ++cc) {
                if (world[rr][cc] == '#') continue;
                if (! (is_clear_dir(world, rr, cc, -1,0,1) || is_clear_dir(world, rr, cc,1,0,1) ||
                       is_clear_dir(world, rr, cc,0,-1,1) || is_clear_dir(world, rr, cc,0,1,1)) ) continue;
                int d = abs(rr - pr) + abs(cc - pc);
                if (d > best_dist) { best_dist = d; chosen_r = rr; chosen_c = cc; }
            }
        }
    }

    if (chosen_r != -1) {
        remove_enemy_from_world(world, boss);
        boss->row = chosen_r;
        boss->col = chosen_c;
        place_enemy_on_world(world, boss);
    }

    // Burst: shot_count waves each containing full-circle bullets (BOSS_SHOT_DIRECTIONS)
    int shot_waves = BOSS_SHOT_WAVES_MIN + (rand() % (BOSS_SHOT_WAVES_MAX - BOSS_SHOT_WAVES_MIN + 1));
    int base_damage = boss->dmg / BOSS_BULLET_DAMAGE_DIV;
    if (base_damage < 1) base_damage = 1;

    int dir_count = BOSS_SHOT_DIRECTIONS;
    double PI = acos(-1.0);
    for (int w = 0; w < shot_waves; ++w) {
        for (int i = 0; i < dir_count; ++i) {
            double angle = 2.0 * PI * ((double)i / (double)dir_count) + (w * 0.1); // small offset per wave
            int dr = (int)round(sin(angle));
            int dc = (int)round(cos(angle));
            if (dr == 0 && dc == 0) dc = 1; // ensure some direction

            // skip if immediate neighbor is blocked
            int sr = boss->row + dr;
            int sc = boss->col + dc;
            if (sr < 0 || sr >= ROWS || sc < 0 || sc >= COLS) continue;
            if (world[sr][sc] == '#') continue;

            // stagger: each bullet delayed by bullet_index * STAGGER, plus wave offset
            long start_delay = (long)((w * dir_count + i) * (long)BOSS_SHOT_STAGGER_US);
            // homing occasionally in phase 2
            long homing_until_abs = 0;
            if (boss->boss_phase >= 2 && (rand()%6)==0) {
                homing_until_abs = now_us() + 800000; // homing active for 0.8s
            }
            // spawn bullet ONE STEP OUT so it doesn't cover the boss glyph
            spawn_bullet(sr, sc, dr, dc, base_damage,
                         BOSS_BULLET_BASE_SPEED + current_level/1, BOSS_BULLET_LIFETIME,
                         start_delay, homing_until_abs, (dr==0?'-':(dc==0?'|':'*')));
        }
    }

    // schedule next teleport/sequence
    boss->boss_next_action = now_us() + BOSS_TELEPORT_BASE_DELAY_US + (rand() % BOSS_TELEPORT_VARIANCE_US);

    // phase change when health is half
    if (boss->hp <= (boss->max_hp/2) && boss->boss_phase == 1) {
        boss->boss_phase = 2;
        boss->speed += 1;
        // tighten next action a bit
        boss->boss_next_action = now_us() + (BOSS_TELEPORT_BASE_DELAY_US / 2);
    }
}

// ---------- Level / map / setup helpers ----------

const char *boss_map[ROWS] = {
    " ###################################### ",
    "##########                          ####",
    "###                                     ##",
    "##                                      #",
    "#                                     ###",
    "#                                    ####",
    "##                                    ###",
    "###                                    ##",
    "####                                   ##",
    "#####           ##    #                 #",
    "#####             ###                   #",
    "#####                                   #",
    "####                                    #",
    "##                                      #",
    "##                                     #",
    "#                                      #",
    "#                                      #",
    "##        P                          ###",
    "###                                #####",
    " ###################################### ",
};

void copy_boss_map(char world[ROWS][COLS+1]) {
    for (int r = 0; r < ROWS; ++r) {
        strncpy(world[r], boss_map[r], COLS);
        world[r][COLS] = '\0';
    }
}

void setup_level(int level, char world[ROWS][COLS+1], int *pr, int *pc, Stats player_stats) {
    for (int i = 0; i < MAX_ENEMIES; ++i) enemies[i].alive = 0;
    enemy_count = 0;
    bullets_init();

    if (level < 3) {
        generate_map(world, pr, pc);

        int base_enemies = (level == 1) ? 6 : 10;
        int elites = (level == 1) ? 1 : 3;

        for (int i = 0; i < base_enemies && enemy_count < MAX_ENEMIES; ++i) {
            Enemy e = spawn_enemy(world, 0, 0, *pr, *pc);
            if (e.alive) { enemies[enemy_count++] = e; place_enemy_on_world(world, &enemies[enemy_count-1]); }
        }
        for (int i = 0; i < elites && enemy_count < MAX_ENEMIES; ++i) {
            Enemy e = spawn_enemy(world, 1, 0, *pr, *pc);
            if (e.alive) { enemies[enemy_count++] = e; place_enemy_on_world(world, &enemies[enemy_count-1]); }
        }
    } else {
        copy_boss_map(world);
        if (!find_player(world, pr, pc)) { *pr = ROWS/2; *pc = COLS/2; world[*pr][*pc] = 'P'; }

        int boss_r = -1, boss_c = -1;
        for (int r = 0; r < ROWS; ++r) for (int c = 0; c < COLS; ++c) if (world[r][c] == '{') { boss_r = r; boss_c = c; break; }

        if (boss_r != -1) {
            Enemy b = {0};
            b.is_boss = 1;
            b.alive = 1;
            b.last_move = b.last_hit = b.contact_time = b.attack_state_until = 0;
            b.attack_state = 0;
            b.boss_next_action = 0;
            b.boss_phase = 1;
            b.max_hp = b.hp = 300 + level * 120;
            b.dmg = 10 + level * 4;
            b.speed = 3;
            strcpy(b.shape, "N");
            b.width = (int)strlen(b.shape);
            b.row = boss_r;
            b.col = boss_c;
            enemies[enemy_count++] = b;
            place_enemy_on_world(world, &enemies[enemy_count-1]);
        } else {
            Enemy e = spawn_enemy(world, 0, 1, *pr, *pc);
            if (e.alive) { enemies[enemy_count++] = e; place_enemy_on_world(world, &enemies[enemy_count-1]); }
        }
    }
}

// ---------- Upgrade / XP UI ----------
void show_upgrade_screen(Stats *player_stats) {
    while (1) {
        system("clear");
        printf(BOLD BYELLOW "Level Complete!  XP: %d   Total XP: %d\n\n" RESET, player_xp, player_total_xp);
        printf(BOLD GREEN"Your stats:\n");
        printf(LGREEN"1) VGR: %d   (Increases max HP by +2 per VGR)\n", player_stats->VGR);
        printf(LGREEN"2) STR: %d   (Increases damage)\n", player_stats->STR);
        printf(LGREEN"3) SPD: %d   (Increases speed)\n", player_stats->SPD);
        printf(LGREEN"4) INT: %d   (Increases mage damage)\n", player_stats->INT);
        printf(LGREEN"5) LCK: %d   (Affects drop/chance)\n\n", player_stats->LCK);

        int cost_vgr = 20 + player_stats->VGR * 2;
        int cost_str = 20 + player_stats->STR * 2;
        int cost_spd = 25 + player_stats->SPD * 3;
        int cost_int = 20 + player_stats->INT * 2;
        int cost_lck = 15 + player_stats->LCK * 1;

        printf(LYELLOW"Upgrade costs (XP):\n");
        printf(LYELLOW" [1] +1 VGR  -> %d XP\n", cost_vgr);
        printf(LYELLOW" [2] +1 STR  -> %d XP\n", cost_str);
        printf(LYELLOW" [3] +1 SPD  -> %d XP\n", cost_spd);
        printf(LYELLOW" [4] +1 INT  -> %d XP\n", cost_int);
        printf(LYELLOW" [5] +1 LCK  -> %d XP\n", cost_lck);
        printf(LYELLOW"\n [c] Continue to next level (or press q to quit)\n");
        printf(GREY"Choose upgrade or action: ");

        char choice = '\0';
        while (kbhit()) (void)getchar();
        scanf(" %c", &choice);

        if (choice == 'q') break;
        else if (choice == 'c') break;
        else if (choice == '1') {
            if (player_xp >= cost_vgr) { player_xp -= cost_vgr; player_stats->VGR += 1; player_hp_global = 30 + player_stats->VGR * 2; }
            else { printf(LRED"\nNot enough XP. Press any key..."); getchar(); getchar(); }
        } else if (choice == '2') {
            if (player_xp >= cost_str) { player_xp -= cost_str; player_stats->STR += 1; }
            else { printf(LRED"\nNot enough XP. Press any key..."); getchar(); getchar(); }
        } else if (choice == '3') {
            if (player_xp >= cost_spd) { player_xp -= cost_spd; player_stats->SPD += 1; }
            else { printf(LRED"\nNot enough XP. Press any key..."); getchar(); getchar(); }
        } else if (choice == '4') {
            if (player_xp >= cost_int) { player_xp -= cost_int; player_stats->INT += 1; }
            else { printf(LRED"\nNot enough XP. Press any key..."); getchar(); getchar(); }
        } else if (choice == '5') {
            if (player_xp >= cost_lck) { player_xp -= cost_lck; player_stats->LCK += 1; }
            else { printf(LRED"\nNot enough XP. Press any key..."); getchar(); getchar(); }
        } else {
            printf(LRED"\nInvalid input. Press any key..."); getchar(); getchar();
        }
    }
}

// ---------- MAIN ----------
int main(void) {
    srand((unsigned)time(NULL));

    char world[ROWS][COLS+1];
    int pr, pc;         // player row/col
    int level = 1;      // current level


    char u_input, player_class[25];
    Stats player_stats;

    // Default modifiers (as before)
    mods = (ClassModifiers){1.0f, 1.0f, 1.0f};

    // ASCII banner
    printf(
        BYELLOW
        "\n\n"
        " █████   █████                    █████            \n"
        "░░███   ░░███                    ░░███             \n"
        " ░███    ░███   ██████    █████  ███████    ██████ \n"
        " ░███████████  ░░░░░███  ███░░  ░░░███░    ███░░███\n"
        YELLOW
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
            case '1': strcpy(player_class,"Cannoneer");   player_stats=(Stats){20,20,10, 2, 5};mods = (ClassModifiers){0.8, 1.0, 1.5}; break;
            case '2': strcpy(player_class,"Gunslinger");  player_stats=(Stats){15,15,26, 1, 6};mods = (ClassModifiers){1.3, 1.5, 0.8}; break;
            case '3': strcpy(player_class,"Sorcerer"); player_stats=(Stats){10, 6,15,20, 8};mods = (ClassModifiers){1.0, 1.2, 1.2}; break;
            default:  player_class[0]='\0';
        }
        if (!player_class[0]) puts(RED BOLD"\nInvalid choice, please try again!\n"NORMAL);
    } while (!player_class[0]);

    printf(BOLD GREEN "\nYou have chosen: %s\n\n" NORMAL, player_class);
    printf(NORMAL BGREEN"Your stats:\nVGR: %d | STR: %d | SPD: %d | INT: %d | LCK: %d\n",
           player_stats.VGR, player_stats.STR, player_stats.SPD,
           player_stats.INT, player_stats.LCK);
    printf(NORMAL GREY"\n- Type any key to Continue -");
    scanf(" %c", &u_input);
    system("clear");

    // Setup first level
    current_level = 1;
    setup_level(current_level, world, &pr, &pc, player_stats);

    // Player HP
    player_hp_global = 30 + player_stats.VGR * 2;

    long last_action = 0;
    int action_locked = 0;
    int cooldown_type = 0;

    // Game loop
    while (1) {
        draw_world_with_hud(world, player_hp_global);

        // First: update bullets (boss projectiles)
        if (update_bullets(world, &player_hp_global, pr, pc)) {
            if (player_hp_global <= 0) { system("clear"); puts("YOU DIED!"); return 0; }
        }

        // Enemy phase: boss behavior and others
        for (int i = 0; i < enemy_count; ++i) {
            Enemy *e = &enemies[i];
            if (!e->alive) continue;
            if (e->is_boss) {
                update_boss_behavior(e, world, pr, pc);
            } else {
                enemy_try_attack(e, pr, pc, &player_hp_global, world);
                if (player_hp_global <= 0) { system("clear"); puts("YOU DIED!"); return 0; }
                update_enemy_ai(e, world, pr, pc);
            }
        }

        // Win condition: all dead?
        int any_alive = 0;
        for (int i = 0; i < enemy_count; ++i) if (enemies[i].alive) { any_alive = 1; break; }
        if (!any_alive) {
            show_upgrade_screen(&player_stats);
            player_hp_global = 30 + player_stats.VGR * 2;
            if (current_level < 3) {
                current_level++;
                system("clear");
                printf(BGREEN "Level %d cleared! Preparing Level %d...\n" RESET, current_level-1, current_level);
                usleep(800000);
                setup_level(current_level, world, &pr, &pc, player_stats);
                continue;
            } else {
                system("clear");
                printf(BGREEN "You defeated the Elite Knight! All levels cleared!\n" RESET);
                break;
            }
        }

        // PLAYER PHASE: cooldown & input
        if (action_locked) {
            while (kbhit()) (void)getchar();
            int delay = (cooldown_type == 1) ? movement_delay(player_stats, mods.move_speed_mult) : attack_delay(player_stats, mods.atk_speed_mult);
            if ((now_us() - last_action) > delay) action_locked = 0;
            usleep(50000);
            continue;
        }

        if (kbhit()) {
            char c = getchar();
            if (c == 'q') break;
            long t = now_us();

            if (c == 'w' || c == 'a' || c == 's' || c == 'd') last_dir = c;

            // cheat code
            if (c == 'p') { // cheat: go straight to boss
                printf(GREEN BOLD "\nCHEAT ACTIVATED: Jumping to Boss Level!\n" NORMAL);
                level = 3;
                current_level = 3; // keep global scaling consistent
                setup_level(level, world, &pr, &pc, player_stats);
                continue; // skip rest of this tick
            }

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
                int dmg = calc_damage(player_class, player_stats, mods);
                if (strcmp(player_class, "Sorcerer") == 0) mage_attack(world, pr, pc, last_dir, dmg);
                else if (strcmp(player_class, "Gunslinger") == 0) gun_attack(world, pr, pc, last_dir, dmg);
                else can_attack(world, pr, pc, last_dir, dmg);
                last_action = t; action_locked = 1; cooldown_type = 2;
            }
        }

        usleep(40000); // frame tick
    }

    system("clear");
    printf(BRED "Goodbye, %s.\n" RESET, player_class);
    return 0;
}
