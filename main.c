#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <tlhelp32.h>
#include <commdlg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#define APP_TITLE "Syobon Stage Patcher"
#define GAME_EXE "OpenSyobonAction.exe"

#define MAP_ROWS 17
#define MAP_COLS 64
#define GAME_MAP_STRIDE 1001
#define GAME_MAP_BASE 0x0043dec0u
#define GAME_STATE 0x00430000u
#define GAME_THEME 0x00430004u
#define GAME_PLAYER_X 0x00434a00u
#define GAME_PLAYER_Y 0x00434a04u
#define GAME_CAMERA_X 0x0043de94u
#define GAME_CAMERA_Y 0x0043de98u
#define GAME_MAP_ROWS_VALUE 0x004345ccu
#define GAME_MAP_COLS_VALUE 0x004345d0u
#define GAME_STAGE_INIT_FUNC 0x0040fee4u
#define GAME_STAGE_INIT_STOLEN 6
#define GAME_SENTINEL 0xff76abc0
#define GAME_DEAD_SENTINEL 0xfff3cb00

#define STATIC_TILE_CAP 641
#define STATIC_TILE_X 0x00434aa0u
#define STATIC_TILE_Y 0x004354c0u
#define STATIC_TILE_A 0x00435ee0u
#define STATIC_TILE_B 0x00436900u
#define STATIC_TILE_TYPE 0x00437d40u
#define STATIC_TILE_FLAGS_A 0x00438760u
#define STATIC_TILE_FLAGS_B 0x00439180u
#define STATIC_TILE_COUNT 0x00434a9cu

#define ENEMY_CAP 41
#define ENEMY_X 0x0043d1e0u
#define ENEMY_Y 0x0043d2a0u
#define ENEMY_A 0x0043d360u
#define ENEMY_B 0x0043d420u
#define ENEMY_TYPE 0x0043d4e0u
#define ENEMY_DRAW_X 0x0043d5a0u
#define ENEMY_DRAW_Y 0x0043d660u
#define ENEMY_FLAGS 0x0043d720u
#define ENEMY_COUNT 0x0043d1c8u

#define CELL 22
#define GRID_X 12
#define GRID_Y 76
#define BTN_LAUNCH 1001
#define BTN_CLEAR 1002
#define BTN_BROWSE 1003
#define EDIT_PATH 1004
#define BTN_IMPORT 1005
#define BTN_SAVE 1006
#define BTN_LOAD 1007
#define TILE_BASE 1100
#define WM_PATCH_DONE (WM_APP + 1)

#define STAGE_FILE_MAGIC "SYOBONPATCHER_STAGE"
#define STAGE_FILE_VERSION 1

typedef struct {
    int id;
    const char *name;
    COLORREF color;
} TileDef;

static const TileDef g_tiles[] = {
    {0, "Erase", RGB(28, 34, 42)},
    {1, "Ground", RGB(100, 72, 48)},
    {4, "Block", RGB(184, 118, 44)},
    {5, "Brick", RGB(206, 151, 73)},
    {7, "Hard", RGB(112, 112, 120)},
    {80, "Enemy 0", RGB(201, 78, 66)},
    {81, "Enemy 1", RGB(226, 126, 58)},
};

static HWND g_hwnd;
static HWND g_path_edit;
static HWND g_launch_button;
static uint8_t g_map[MAP_ROWS][MAP_COLS];
static int g_selected_tile = 1;
static char g_game_path[MAX_PATH] = GAME_EXE;
static volatile LONG g_patch_running = 0;

typedef struct {
    DWORD pid;
    HANDLE process;
    HANDLE main_thread;
    bool started_suspended;
} GameTarget;

typedef struct {
    bool ok;
    char message[256];
} PatchResult;

static uintptr_t rva_from_absolute(uintptr_t address)
{
    return address - 0x00400000u;
}

static uintptr_t game_address(uintptr_t module_base, uintptr_t absolute_address)
{
    return module_base + rva_from_absolute(absolute_address);
}

static void init_default_stage(void)
{
    ZeroMemory(g_map, sizeof(g_map));

    for (int x = 0; x < MAP_COLS; ++x) {
        g_map[15][x] = 5;
        g_map[16][x] = 5;
    }

    for (int x = 10; x < 16; ++x) g_map[11][x] = 4;
    for (int x = 22; x < 29; ++x) g_map[9][x] = 4;
    for (int x = 39; x < 47; ++x) g_map[12][x] = 7;

    g_map[14][34] = 1;
    g_map[13][34] = 1;
    g_map[14][35] = 1;
    g_map[13][35] = 1;
    g_map[14][54] = 80;
}

static bool write_remote(HANDLE process, uintptr_t address, const void *data, SIZE_T size)
{
    SIZE_T written = 0;
    return WriteProcessMemory(process, (LPVOID)address, data, size, &written) && written == size;
}

static bool write_remote_i32(HANDLE process, uintptr_t address, int32_t value)
{
    return write_remote(process, address, &value, sizeof(value));
}

static bool read_remote(HANDLE process, uintptr_t address, void *data, SIZE_T size)
{
    SIZE_T bytes_read = 0;
    return ReadProcessMemory(process, (LPCVOID)address, data, size, &bytes_read) &&
           bytes_read == size;
}

static bool get_full_path(const char *path, char *out, DWORD out_size)
{
    DWORD len = GetFullPathNameA(path, out_size, out, NULL);
    return len > 0 && len < out_size;
}

static const char *path_basename(const char *path)
{
    const char *slash = strrchr(path, '\\');
    const char *fwd_slash = strrchr(path, '/');
    if (fwd_slash != NULL && (slash == NULL || fwd_slash > slash)) {
        slash = fwd_slash;
    }
    return slash != NULL ? slash + 1 : path;
}

static bool is_existing_file(const char *path)
{
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static bool is_process_running(HANDLE process)
{
    DWORD code = 0;
    return GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
}

static bool query_process_image_path(DWORD pid, char *out, DWORD out_size)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == NULL) return false;

    DWORD size = out_size;
    bool ok = QueryFullProcessImageNameA(process, 0, out, &size) != 0;
    CloseHandle(process);
    return ok;
}

static DWORD find_process_id_by_path(const char *exe_name, const char *expected_path)
{
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);

    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, exe_name) != 0) continue;

            char image_path[MAX_PATH];
            if (query_process_image_path(pe.th32ProcessID, image_path, sizeof(image_path)) &&
                _stricmp(image_path, expected_path) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }

    CloseHandle(snap);
    return pid;
}

static bool open_running_game(GameTarget *target, char *error, size_t error_size)
{
    ZeroMemory(target, sizeof(*target));

    char game_full_path[MAX_PATH];
    if (!get_full_path(g_game_path, game_full_path, sizeof(game_full_path)) ||
        !is_existing_file(game_full_path)) {
        snprintf(error, error_size, "Game executable was not found.");
        return false;
    }

    if (_stricmp(path_basename(game_full_path), GAME_EXE) != 0) {
        snprintf(error, error_size, "Select %s.", GAME_EXE);
        return false;
    }

    DWORD pid = find_process_id_by_path(GAME_EXE, game_full_path);
    if (pid == 0) {
        snprintf(error, error_size, "Start %s before importing.", GAME_EXE);
        return false;
    }

    target->process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (target->process == NULL) {
        snprintf(error, error_size, "OpenProcess failed. Try running as administrator.");
        return false;
    }

    target->pid = pid;
    return true;
}

static uintptr_t find_module_base(DWORD pid, const char *module_name)
{
    uintptr_t base = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    MODULEENTRY32 me;
    ZeroMemory(&me, sizeof(me));
    me.dwSize = sizeof(me);

    if (Module32First(snap, &me)) {
        do {
            if (_stricmp(me.szModule, module_name) == 0) {
                base = (uintptr_t)me.modBaseAddr;
                break;
            }
        } while (Module32Next(snap, &me));
    }

    CloseHandle(snap);
    return base;
}

static bool launch_or_find_game(GameTarget *target, char *error, size_t error_size)
{
    ZeroMemory(target, sizeof(*target));

    char game_full_path[MAX_PATH];
    if (!get_full_path(g_game_path, game_full_path, sizeof(game_full_path)) ||
        !is_existing_file(game_full_path)) {
        snprintf(error, error_size, "Game executable was not found.");
        return false;
    }

    if (_stricmp(path_basename(game_full_path), GAME_EXE) != 0) {
        snprintf(error, error_size, "Select %s.", GAME_EXE);
        return false;
    }

    DWORD pid = find_process_id_by_path(GAME_EXE, game_full_path);
    if (pid != 0) {
        target->process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                      PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid);
        if (target->process == NULL) {
            snprintf(error, error_size, "OpenProcess failed. Try running as administrator.");
            return false;
        }

        target->pid = pid;
        return true;
    }

    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    char command[MAX_PATH + 4];
    char workdir[MAX_PATH];
    char *last_slash;

    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    snprintf(command, sizeof(command), "\"%s\"", game_full_path);
    lstrcpynA(workdir, game_full_path, sizeof(workdir));
    last_slash = strrchr(workdir, '\\');
    if (last_slash != NULL) {
        *last_slash = '\0';
    } else {
        lstrcpynA(workdir, ".", sizeof(workdir));
    }

    if (!CreateProcessA(NULL, command, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, workdir, &si, &pi)) {
        snprintf(error, error_size, "Could not start %s.", GAME_EXE);
        return false;
    }

    target->pid = pi.dwProcessId;
    target->process = pi.hProcess;
    target->main_thread = pi.hThread;
    target->started_suspended = true;
    return true;
}

static void build_full_game_map(uint8_t *out)
{
    memset(out, 0, MAP_ROWS * GAME_MAP_STRIDE);

    for (int y = 0; y < MAP_ROWS; ++y) {
        for (int x = 0; x < MAP_COLS; ++x) {
            out[y * GAME_MAP_STRIDE + x] = g_map[y][x];
        }
    }
}

static bool is_static_tile(int tile)
{
    return 1 <= tile && tile < 20 && tile != 9;
}

static void clear_i32_array(int32_t *array, int count, int32_t value)
{
    for (int i = 0; i < count; ++i) {
        array[i] = value;
    }
}

static void build_static_tiles(int32_t *x, int32_t *y, int32_t *a, int32_t *b,
                               int32_t *type, int32_t *flags_a, int32_t *flags_b,
                               int *count)
{
    clear_i32_array(x, STATIC_TILE_CAP, GAME_SENTINEL);
    clear_i32_array(y, STATIC_TILE_CAP, 1);
    clear_i32_array(a, STATIC_TILE_CAP, 1);
    clear_i32_array(b, STATIC_TILE_CAP, 1);
    clear_i32_array(type, STATIC_TILE_CAP, 1);
    clear_i32_array(flags_a, STATIC_TILE_CAP, 0);
    clear_i32_array(flags_b, STATIC_TILE_CAP, 0);

    int out = 0;
    for (int col = 0; col < MAP_COLS && out < STATIC_TILE_CAP; ++col) {
        for (int row = 0; row < MAP_ROWS && out < STATIC_TILE_CAP; ++row) {
            int tile = g_map[row][col];
            if (!is_static_tile(tile)) continue;

            x[out] = col * 2900;
            y[out] = -1200 + row * 2900;
            type[out] = tile;
            ++out;
        }
    }

    *count = out;
}

static void build_enemies(int32_t *x, int32_t *y, int32_t *a, int32_t *b,
                          int32_t *type, int32_t *draw_x, int32_t *draw_y,
                          int32_t *flags, int *count)
{
    clear_i32_array(x, ENEMY_CAP, GAME_SENTINEL);
    clear_i32_array(y, ENEMY_CAP, 1);
    clear_i32_array(a, ENEMY_CAP, 1);
    clear_i32_array(b, ENEMY_CAP, 1);
    clear_i32_array(type, ENEMY_CAP, 0);
    clear_i32_array(draw_x, ENEMY_CAP, 1);
    clear_i32_array(draw_y, ENEMY_CAP, 1);
    clear_i32_array(flags, ENEMY_CAP, 0);

    int out = 0;
    for (int col = 0; col < MAP_COLS && out < ENEMY_CAP; ++col) {
        for (int row = 0; row < MAP_ROWS && out < ENEMY_CAP; ++row) {
            int tile = g_map[row][col];
            if (tile < 0x50 || tile >= 0x5a) continue;

            x[out] = col * 2900;
            y[out] = -1200 + row * 2900;
            type[out] = tile - 0x50;
            ++out;
        }
    }

    *count = out;
}

static void emit_u32(uint8_t *code, size_t *pos, uint32_t value)
{
    memcpy(code + *pos, &value, sizeof(value));
    *pos += sizeof(value);
}

static void emit_copy(uint8_t *code, size_t *pos, uint32_t dst, uint32_t src, uint32_t size)
{
    code[(*pos)++] = 0xbf; emit_u32(code, pos, dst);  /* mov edi, dst */
    code[(*pos)++] = 0xbe; emit_u32(code, pos, src);  /* mov esi, src */
    code[(*pos)++] = 0xb9; emit_u32(code, pos, size); /* mov ecx, size */
    code[(*pos)++] = 0xf3; code[(*pos)++] = 0xa4;     /* rep movsb */
}

static void emit_write_i32(uint8_t *code, size_t *pos, uint32_t dst, int32_t value)
{
    code[(*pos)++] = 0xc7;
    code[(*pos)++] = 0x05;
    emit_u32(code, pos, dst);
    emit_u32(code, pos, (uint32_t)value);
}

static bool install_stage_init_hook(HANDLE process, uintptr_t base,
                                    const uint8_t *map, size_t map_size,
                                    char *error, size_t error_size)
{
    uintptr_t hook_site = game_address(base, GAME_STAGE_INIT_FUNC);
    uintptr_t resume_at = hook_site + GAME_STAGE_INIT_STOLEN;

    uint8_t old_bytes[GAME_STAGE_INIT_STOLEN];
    SIZE_T bytes_read = 0;
    if (!ReadProcessMemory(process, (LPCVOID)hook_site, old_bytes, sizeof(old_bytes), &bytes_read) ||
        bytes_read != sizeof(old_bytes)) {
        snprintf(error, error_size, "Could not read stage init hook site.");
        return false;
    }

    if (old_bytes[0] == 0x68 && old_bytes[5] == 0xc3) {
        return true;
    }

    if (!(old_bytes[0] == 0x55 && old_bytes[1] == 0x89 && old_bytes[2] == 0xe5)) {
        snprintf(error, error_size, "Unexpected bytes at stage init hook site.");
        return false;
    }

    int32_t static_x[STATIC_TILE_CAP], static_y[STATIC_TILE_CAP];
    int32_t static_a[STATIC_TILE_CAP], static_b[STATIC_TILE_CAP];
    int32_t static_type[STATIC_TILE_CAP], static_flags_a[STATIC_TILE_CAP], static_flags_b[STATIC_TILE_CAP];
    int32_t enemy_x[ENEMY_CAP], enemy_y[ENEMY_CAP], enemy_a[ENEMY_CAP], enemy_b[ENEMY_CAP];
    int32_t enemy_type[ENEMY_CAP], enemy_draw_x[ENEMY_CAP], enemy_draw_y[ENEMY_CAP], enemy_flags[ENEMY_CAP];
    int static_count = 0;
    int enemy_count = 0;

    build_static_tiles(static_x, static_y, static_a, static_b,
                       static_type, static_flags_a, static_flags_b, &static_count);
    build_enemies(enemy_x, enemy_y, enemy_a, enemy_b, enemy_type,
                  enemy_draw_x, enemy_draw_y, enemy_flags, &enemy_count);

    const size_t trampoline_size = GAME_STAGE_INIT_STOLEN + 6;
    const size_t code_cap = 512;
    const size_t data_size =
        map_size +
        sizeof(static_x) + sizeof(static_y) + sizeof(static_a) + sizeof(static_b) +
        sizeof(static_type) + sizeof(static_flags_a) + sizeof(static_flags_b) +
        sizeof(enemy_x) + sizeof(enemy_y) + sizeof(enemy_a) + sizeof(enemy_b) +
        sizeof(enemy_type) + sizeof(enemy_draw_x) + sizeof(enemy_draw_y) + sizeof(enemy_flags);
    size_t payload_size = trampoline_size + code_cap + data_size;
    uint8_t *payload = (uint8_t *)malloc(payload_size);
    if (payload == NULL) {
        snprintf(error, error_size, "Could not allocate hook payload.");
        return false;
    }

    void *remote = VirtualAllocEx(process, NULL, payload_size, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_EXECUTE_READWRITE);
    if (remote == NULL || (uintptr_t)remote > 0xffffffffu) {
        free(payload);
        snprintf(error, error_size, "Could not allocate 32-bit hook memory.");
        return false;
    }

    uintptr_t remote_addr = (uintptr_t)remote;
    uintptr_t remote_trampoline = remote_addr;
    uintptr_t remote_code = remote_addr + trampoline_size;
    uintptr_t data_cursor = remote_code + code_cap;

    size_t p = 0;

    memcpy(payload + p, old_bytes, GAME_STAGE_INIT_STOLEN);
    p += GAME_STAGE_INIT_STOLEN;
    payload[p++] = 0x68; emit_u32(payload, &p, (uint32_t)resume_at); /* push resume_at */
    payload[p++] = 0xc3;                                             /* ret */

    size_t c = trampoline_size;
    payload[c++] = 0xb8; emit_u32(payload, &c, (uint32_t)remote_trampoline); /* mov eax, trampoline */
    payload[c++] = 0xff; payload[c++] = 0xd0;                              /* call eax */
    payload[c++] = 0x9c;                                                    /* pushfd */
    payload[c++] = 0x60;                                                    /* pushad */

#define ADD_DATA_COPY(dst_abs, src_ptr, sz) do { \
        uintptr_t src_remote = data_cursor; \
        memcpy(payload + (src_remote - remote_addr), (src_ptr), (sz)); \
        emit_copy(payload, &c, (uint32_t)game_address(base, (dst_abs)), (uint32_t)src_remote, (uint32_t)(sz)); \
        data_cursor += (sz); \
    } while (0)

    ADD_DATA_COPY(GAME_MAP_BASE, map, map_size);
    ADD_DATA_COPY(STATIC_TILE_X, static_x, sizeof(static_x));
    ADD_DATA_COPY(STATIC_TILE_Y, static_y, sizeof(static_y));
    ADD_DATA_COPY(STATIC_TILE_A, static_a, sizeof(static_a));
    ADD_DATA_COPY(STATIC_TILE_B, static_b, sizeof(static_b));
    ADD_DATA_COPY(STATIC_TILE_TYPE, static_type, sizeof(static_type));
    ADD_DATA_COPY(STATIC_TILE_FLAGS_A, static_flags_a, sizeof(static_flags_a));
    ADD_DATA_COPY(STATIC_TILE_FLAGS_B, static_flags_b, sizeof(static_flags_b));
    ADD_DATA_COPY(ENEMY_X, enemy_x, sizeof(enemy_x));
    ADD_DATA_COPY(ENEMY_Y, enemy_y, sizeof(enemy_y));
    ADD_DATA_COPY(ENEMY_A, enemy_a, sizeof(enemy_a));
    ADD_DATA_COPY(ENEMY_B, enemy_b, sizeof(enemy_b));
    ADD_DATA_COPY(ENEMY_TYPE, enemy_type, sizeof(enemy_type));
    ADD_DATA_COPY(ENEMY_DRAW_X, enemy_draw_x, sizeof(enemy_draw_x));
    ADD_DATA_COPY(ENEMY_DRAW_Y, enemy_draw_y, sizeof(enemy_draw_y));
    ADD_DATA_COPY(ENEMY_FLAGS, enemy_flags, sizeof(enemy_flags));

#undef ADD_DATA_COPY

    emit_write_i32(payload, &c, (uint32_t)game_address(base, STATIC_TILE_COUNT), static_count);
    emit_write_i32(payload, &c, (uint32_t)game_address(base, ENEMY_COUNT), enemy_count);
    emit_write_i32(payload, &c, (uint32_t)game_address(base, GAME_MAP_ROWS_VALUE), MAP_ROWS);
    emit_write_i32(payload, &c, (uint32_t)game_address(base, GAME_MAP_COLS_VALUE), GAME_MAP_STRIDE);
    emit_write_i32(payload, &c, (uint32_t)game_address(base, GAME_THEME), 1);
    emit_write_i32(payload, &c, (uint32_t)game_address(base, GAME_PLAYER_X), 1800);
    emit_write_i32(payload, &c, (uint32_t)game_address(base, GAME_PLAYER_Y), 36000);
    emit_write_i32(payload, &c, (uint32_t)game_address(base, GAME_CAMERA_X), 0);
    emit_write_i32(payload, &c, (uint32_t)game_address(base, GAME_CAMERA_Y), 0);
    emit_write_i32(payload, &c, (uint32_t)game_address(base, GAME_STATE), 1);
    payload[c++] = 0x61; /* popad */
    payload[c++] = 0x9d; /* popfd */
    payload[c++] = 0xc3; /* ret */

    if (c > trampoline_size + code_cap || data_cursor != remote_addr + payload_size) {
        free(payload);
        snprintf(error, error_size, "Internal hook payload size error.");
        return false;
    }

    SIZE_T written = 0;
    bool ok = WriteProcessMemory(process, remote, payload, payload_size, &written) &&
              written == payload_size;
    free(payload);
    if (!ok) {
        snprintf(error, error_size, "Could not write hook payload.");
        return false;
    }

    uint8_t hook[6];
    hook[0] = 0x68; memcpy(hook + 1, &remote_code, 4); hook[5] = 0xc3; /* push remote_code; ret */

    DWORD old_protect = 0;
    if (!VirtualProtectEx(process, (LPVOID)hook_site, sizeof(hook), PAGE_EXECUTE_READWRITE,
                          &old_protect)) {
        snprintf(error, error_size, "Could not make hook site writable.");
        return false;
    }

    written = 0;
    ok = WriteProcessMemory(process, (LPVOID)hook_site, hook, sizeof(hook), &written) &&
         written == sizeof(hook);

    DWORD ignored = 0;
    VirtualProtectEx(process, (LPVOID)hook_site, sizeof(hook), old_protect, &ignored);
    FlushInstructionCache(process, (LPCVOID)hook_site, sizeof(hook));

    if (!ok) {
        snprintf(error, error_size, "Could not write stage init hook.");
        return false;
    }

    return true;
}

static bool apply_stage_patch(HANDLE process, uintptr_t base)
{
    uint8_t full_map[MAP_ROWS * GAME_MAP_STRIDE];
    build_full_game_map(full_map);

    int32_t static_x[STATIC_TILE_CAP], static_y[STATIC_TILE_CAP];
    int32_t static_a[STATIC_TILE_CAP], static_b[STATIC_TILE_CAP];
    int32_t static_type[STATIC_TILE_CAP], static_flags_a[STATIC_TILE_CAP], static_flags_b[STATIC_TILE_CAP];
    int32_t enemy_x[ENEMY_CAP], enemy_y[ENEMY_CAP], enemy_a[ENEMY_CAP], enemy_b[ENEMY_CAP];
    int32_t enemy_type[ENEMY_CAP], enemy_draw_x[ENEMY_CAP], enemy_draw_y[ENEMY_CAP], enemy_flags[ENEMY_CAP];
    int static_count = 0;
    int enemy_count = 0;

    build_static_tiles(static_x, static_y, static_a, static_b,
                       static_type, static_flags_a, static_flags_b, &static_count);
    build_enemies(enemy_x, enemy_y, enemy_a, enemy_b, enemy_type,
                  enemy_draw_x, enemy_draw_y, enemy_flags, &enemy_count);

    bool ok = true;
    ok = ok && write_remote(process, game_address(base, GAME_MAP_BASE), full_map, sizeof(full_map));
    ok = ok && write_remote(process, game_address(base, STATIC_TILE_X), static_x, sizeof(static_x));
    ok = ok && write_remote(process, game_address(base, STATIC_TILE_Y), static_y, sizeof(static_y));
    ok = ok && write_remote(process, game_address(base, STATIC_TILE_A), static_a, sizeof(static_a));
    ok = ok && write_remote(process, game_address(base, STATIC_TILE_B), static_b, sizeof(static_b));
    ok = ok && write_remote(process, game_address(base, STATIC_TILE_TYPE), static_type, sizeof(static_type));
    ok = ok && write_remote(process, game_address(base, STATIC_TILE_FLAGS_A), static_flags_a, sizeof(static_flags_a));
    ok = ok && write_remote(process, game_address(base, STATIC_TILE_FLAGS_B), static_flags_b, sizeof(static_flags_b));
    ok = ok && write_remote_i32(process, game_address(base, STATIC_TILE_COUNT), static_count);
    ok = ok && write_remote(process, game_address(base, ENEMY_X), enemy_x, sizeof(enemy_x));
    ok = ok && write_remote(process, game_address(base, ENEMY_Y), enemy_y, sizeof(enemy_y));
    ok = ok && write_remote(process, game_address(base, ENEMY_A), enemy_a, sizeof(enemy_a));
    ok = ok && write_remote(process, game_address(base, ENEMY_B), enemy_b, sizeof(enemy_b));
    ok = ok && write_remote(process, game_address(base, ENEMY_TYPE), enemy_type, sizeof(enemy_type));
    ok = ok && write_remote(process, game_address(base, ENEMY_DRAW_X), enemy_draw_x, sizeof(enemy_draw_x));
    ok = ok && write_remote(process, game_address(base, ENEMY_DRAW_Y), enemy_draw_y, sizeof(enemy_draw_y));
    ok = ok && write_remote(process, game_address(base, ENEMY_FLAGS), enemy_flags, sizeof(enemy_flags));
    ok = ok && write_remote_i32(process, game_address(base, ENEMY_COUNT), enemy_count);
    ok = ok && write_remote_i32(process, game_address(base, GAME_MAP_ROWS_VALUE), MAP_ROWS);
    ok = ok && write_remote_i32(process, game_address(base, GAME_MAP_COLS_VALUE), GAME_MAP_STRIDE);
    ok = ok && write_remote_i32(process, game_address(base, GAME_THEME), 1);
    ok = ok && write_remote_i32(process, game_address(base, GAME_PLAYER_X), 1800);
    ok = ok && write_remote_i32(process, game_address(base, GAME_PLAYER_Y), 36000);
    ok = ok && write_remote_i32(process, game_address(base, GAME_CAMERA_X), 0);
    ok = ok && write_remote_i32(process, game_address(base, GAME_CAMERA_Y), 0);
    ok = ok && write_remote_i32(process, game_address(base, GAME_STATE), 1);

    if (ok) {
        uint8_t check_map[MAP_ROWS * GAME_MAP_STRIDE];
        int32_t check_static_count = -1;
        int32_t check_enemy_count = -1;
        SIZE_T bytes_read = 0;

        ok = ReadProcessMemory(process, (LPCVOID)game_address(base, GAME_MAP_BASE),
                               check_map, sizeof(check_map), &bytes_read) &&
             bytes_read == sizeof(check_map) &&
             memcmp(check_map, full_map, sizeof(full_map)) == 0;

        bytes_read = 0;
        ok = ok &&
             ReadProcessMemory(process, (LPCVOID)game_address(base, STATIC_TILE_COUNT),
                               &check_static_count, sizeof(check_static_count), &bytes_read) &&
             bytes_read == sizeof(check_static_count) &&
             check_static_count == static_count;

        bytes_read = 0;
        ok = ok &&
             ReadProcessMemory(process, (LPCVOID)game_address(base, ENEMY_COUNT),
                               &check_enemy_count, sizeof(check_enemy_count), &bytes_read) &&
             bytes_read == sizeof(check_enemy_count) &&
             check_enemy_count == enemy_count;
    }

    return ok;
}

static void map_cell_from_world(int32_t world_x, int32_t world_y, uint8_t tile)
{
    if (world_x == (int32_t)GAME_SENTINEL || world_x == (int32_t)GAME_DEAD_SENTINEL) return;

    int col = world_x / 2900;
    int row = (world_y + 1200) / 2900;
    if (col < 0 || col >= MAP_COLS || row < 0 || row >= MAP_ROWS) return;

    g_map[row][col] = tile;
}

static bool import_stage_from_game(char *error, size_t error_size)
{
    GetWindowTextA(g_path_edit, g_game_path, sizeof(g_game_path));

    GameTarget target;
    if (!open_running_game(&target, error, error_size)) {
        return false;
    }

    uintptr_t base = find_module_base(target.pid, GAME_EXE);
    if (base == 0) {
        CloseHandle(target.process);
        snprintf(error, error_size, "Could not find %s module.", GAME_EXE);
        return false;
    }

    uint8_t full_map[MAP_ROWS * GAME_MAP_STRIDE];
    int32_t static_x[STATIC_TILE_CAP], static_y[STATIC_TILE_CAP], static_type[STATIC_TILE_CAP];
    int32_t enemy_x[ENEMY_CAP], enemy_y[ENEMY_CAP], enemy_type[ENEMY_CAP];
    int32_t static_count = 0;
    int32_t enemy_count = 0;

    bool ok = true;
    ok = ok && read_remote(target.process, game_address(base, GAME_MAP_BASE), full_map, sizeof(full_map));
    ok = ok && read_remote(target.process, game_address(base, STATIC_TILE_X), static_x, sizeof(static_x));
    ok = ok && read_remote(target.process, game_address(base, STATIC_TILE_Y), static_y, sizeof(static_y));
    ok = ok && read_remote(target.process, game_address(base, STATIC_TILE_TYPE), static_type, sizeof(static_type));
    ok = ok && read_remote(target.process, game_address(base, STATIC_TILE_COUNT), &static_count, sizeof(static_count));
    ok = ok && read_remote(target.process, game_address(base, ENEMY_X), enemy_x, sizeof(enemy_x));
    ok = ok && read_remote(target.process, game_address(base, ENEMY_Y), enemy_y, sizeof(enemy_y));
    ok = ok && read_remote(target.process, game_address(base, ENEMY_TYPE), enemy_type, sizeof(enemy_type));
    ok = ok && read_remote(target.process, game_address(base, ENEMY_COUNT), &enemy_count, sizeof(enemy_count));

    CloseHandle(target.process);

    if (!ok) {
        snprintf(error, error_size, "Could not read stage data from the game.");
        return false;
    }

    if (static_count < 0) static_count = 0;
    if (static_count > STATIC_TILE_CAP) static_count = STATIC_TILE_CAP;
    if (enemy_count < 0) enemy_count = 0;
    if (enemy_count > ENEMY_CAP) enemy_count = ENEMY_CAP;

    ZeroMemory(g_map, sizeof(g_map));
    for (int y = 0; y < MAP_ROWS; ++y) {
        for (int x = 0; x < MAP_COLS; ++x) {
            g_map[y][x] = full_map[y * GAME_MAP_STRIDE + x];
        }
    }

    for (int i = 0; i < static_count; ++i) {
        if (is_static_tile(static_type[i])) {
            map_cell_from_world(static_x[i], static_y[i], (uint8_t)static_type[i]);
        }
    }

    for (int i = 0; i < enemy_count; ++i) {
        if (enemy_type[i] >= 0 && enemy_type[i] < 10) {
            map_cell_from_world(enemy_x[i], enemy_y[i], (uint8_t)(enemy_type[i] + 0x50));
        }
    }

    return true;
}

static bool save_stage_file(HWND hwnd)
{
    OPENFILENAMEA ofn;
    char file[MAX_PATH] = "stage.ssp";

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "Syobon stage files\0*.ssp\0Text files\0*.txt\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.lpstrDefExt = "ssp";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

    if (!GetSaveFileNameA(&ofn)) {
        return false;
    }

    FILE *fp = fopen(file, "w");
    if (fp == NULL) {
        MessageBoxA(hwnd, "Could not save the stage file.", APP_TITLE, MB_OK | MB_ICONERROR);
        return false;
    }

    fprintf(fp, "%s %d\n", STAGE_FILE_MAGIC, STAGE_FILE_VERSION);
    fprintf(fp, "%d %d\n", MAP_ROWS, MAP_COLS);
    for (int y = 0; y < MAP_ROWS; ++y) {
        for (int x = 0; x < MAP_COLS; ++x) {
            fprintf(fp, "%u%s", (unsigned)g_map[y][x], x + 1 == MAP_COLS ? "\n" : " ");
        }
    }

    bool ok = ferror(fp) == 0;
    fclose(fp);

    if (!ok) {
        MessageBoxA(hwnd, "Could not finish writing the stage file.", APP_TITLE, MB_OK | MB_ICONERROR);
        return false;
    }

    MessageBoxA(hwnd, "Stage saved.", APP_TITLE, MB_OK | MB_ICONINFORMATION);
    return true;
}

static bool load_stage_file(HWND hwnd)
{
    OPENFILENAMEA ofn;
    char file[MAX_PATH] = "";

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "Syobon stage files\0*.ssp\0Text files\0*.txt\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (!GetOpenFileNameA(&ofn)) {
        return false;
    }

    FILE *fp = fopen(file, "r");
    if (fp == NULL) {
        MessageBoxA(hwnd, "Could not open the stage file.", APP_TITLE, MB_OK | MB_ICONERROR);
        return false;
    }

    char magic[64];
    int version = 0;
    int rows = 0;
    int cols = 0;
    uint8_t loaded[MAP_ROWS][MAP_COLS];
    bool ok = fscanf(fp, "%63s %d", magic, &version) == 2 &&
              strcmp(magic, STAGE_FILE_MAGIC) == 0 &&
              version == STAGE_FILE_VERSION &&
              fscanf(fp, "%d %d", &rows, &cols) == 2 &&
              rows == MAP_ROWS &&
              cols == MAP_COLS;

    for (int y = 0; ok && y < MAP_ROWS; ++y) {
        for (int x = 0; x < MAP_COLS; ++x) {
            unsigned value = 0;
            if (fscanf(fp, "%u", &value) != 1 || value > 255) {
                ok = false;
                break;
            }
            loaded[y][x] = (uint8_t)value;
        }
    }

    fclose(fp);

    if (!ok) {
        MessageBoxA(hwnd, "This is not a valid Syobon stage file.", APP_TITLE, MB_OK | MB_ICONERROR);
        return false;
    }

    memcpy(g_map, loaded, sizeof(g_map));
    InvalidateRect(hwnd, NULL, FALSE);
    MessageBoxA(hwnd, "Stage loaded.", APP_TITLE, MB_OK | MB_ICONINFORMATION);
    return true;
}

static bool patch_game(char *error, size_t error_size)
{
    GameTarget target;
    if (!launch_or_find_game(&target, error, error_size)) {
        return false;
    }

    uintptr_t base = 0;
    for (int i = 0; i < 10 && base == 0; ++i) {
        if (!is_process_running(target.process)) {
            snprintf(error, error_size, "Game process exited before patching.");
            if (target.started_suspended) {
                CloseHandle(target.main_thread);
            }
            CloseHandle(target.process);
            return false;
        }
        base = find_module_base(target.pid, GAME_EXE);
        if (base == 0) Sleep(200);
    }

    if (base == 0) {
        snprintf(error, error_size, "Could not find %s module.", GAME_EXE);
        if (target.started_suspended) {
            TerminateProcess(target.process, 1);
            CloseHandle(target.main_thread);
        }
        CloseHandle(target.process);
        return false;
    }

    uint8_t full_map[MAP_ROWS * GAME_MAP_STRIDE];
    build_full_game_map(full_map);

    bool ok = true;
    bool hook_ok = install_stage_init_hook(target.process, base, full_map, sizeof(full_map),
                                           error, error_size);
    ok = hook_ok;

    if (target.started_suspended) {
        ok = hook_ok;
        if (hook_ok) {
            ResumeThread(target.main_thread);
            WaitForInputIdle(target.process, 3000);
            Sleep(1500);
        } else {
            TerminateProcess(target.process, 1);
        }
        if (ok) {
            ok = apply_stage_patch(target.process, base);
            if (!ok) {
                snprintf(error, error_size, "Post-launch patch failed.");
            }
        }
        CloseHandle(target.main_thread);
    } else {
        if (ok) {
            ok = apply_stage_patch(target.process, base);
        }
        if (!ok && hook_ok) {
            snprintf(error, error_size, "Immediate patch failed.");
        }
    }
    CloseHandle(target.process);

    return ok;
}

static DWORD WINAPI patch_worker(LPVOID param)
{
    PatchResult *result = (PatchResult *)param;
    result->ok = patch_game(result->message, sizeof(result->message));
    if (result->ok) {
        lstrcpynA(result->message, "Stage patched.", sizeof(result->message));
    }
    PostMessageA(g_hwnd, WM_PATCH_DONE, 0, (LPARAM)result);
    return 0;
}

static void start_patch_async(HWND hwnd)
{
    if (InterlockedCompareExchange(&g_patch_running, 1, 0) != 0) {
        return;
    }

    PatchResult *result = (PatchResult *)calloc(1, sizeof(*result));
    if (result == NULL) {
        InterlockedExchange(&g_patch_running, 0);
        MessageBoxA(hwnd, "Could not allocate patch task.", APP_TITLE, MB_OK | MB_ICONERROR);
        return;
    }

    GetWindowTextA(g_path_edit, g_game_path, sizeof(g_game_path));
    EnableWindow(g_launch_button, FALSE);
    SetWindowTextA(g_launch_button, "Patching...");

    HANDLE thread = CreateThread(NULL, 0, patch_worker, result, 0, NULL);
    if (thread == NULL) {
        free(result);
        InterlockedExchange(&g_patch_running, 0);
        EnableWindow(g_launch_button, TRUE);
        SetWindowTextA(g_launch_button, "Patch");
        MessageBoxA(hwnd, "Could not start patch worker.", APP_TITLE, MB_OK | MB_ICONERROR);
        return;
    }
    CloseHandle(thread);
}

static void browse_game(HWND hwnd)
{
    OPENFILENAMEA ofn;
    char file[MAX_PATH];

    lstrcpynA(file, g_game_path, sizeof(file));
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "OpenSyobonAction.exe\0OpenSyobonAction.exe\0Executable files\0*.exe\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (GetOpenFileNameA(&ofn)) {
        lstrcpynA(g_game_path, file, sizeof(g_game_path));
        SetWindowTextA(g_path_edit, g_game_path);
    }
}

static void draw_grid(HDC hdc)
{
    RECT rect;
    HBRUSH brush;
    HPEN grid_pen = CreatePen(PS_SOLID, 1, RGB(64, 71, 82));
    HPEN old_pen = SelectObject(hdc, grid_pen);
    HBRUSH old_brush;

    for (int y = 0; y < MAP_ROWS; ++y) {
        for (int x = 0; x < MAP_COLS; ++x) {
            COLORREF color = RGB(31, 38, 48);
            for (size_t i = 0; i < sizeof(g_tiles) / sizeof(g_tiles[0]); ++i) {
                if (g_tiles[i].id == g_map[y][x]) {
                    color = g_tiles[i].color;
                    break;
                }
            }

            rect.left = GRID_X + x * CELL;
            rect.top = GRID_Y + y * CELL;
            rect.right = rect.left + CELL;
            rect.bottom = rect.top + CELL;

            brush = CreateSolidBrush(color);
            old_brush = SelectObject(hdc, brush);
            Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);
            SelectObject(hdc, old_brush);
            DeleteObject(brush);
        }
    }

    SelectObject(hdc, old_pen);
    DeleteObject(grid_pen);
}

static void paint_cell_from_mouse(int x, int y)
{
    int gx = (x - GRID_X) / CELL;
    int gy = (y - GRID_Y) / CELL;
    if (gx < 0 || gx >= MAP_COLS || gy < 0 || gy >= MAP_ROWS) return;
    g_map[gy][gx] = (uint8_t)g_selected_tile;
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void create_controls(HWND hwnd)
{
    CreateWindowA("STATIC", "Game:", WS_CHILD | WS_VISIBLE,
                  12, 12, 44, 22, hwnd, NULL, NULL, NULL);

    g_path_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", g_game_path,
                                  WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                  58, 10, 500, 24, hwnd, (HMENU)EDIT_PATH, NULL, NULL);

    CreateWindowA("BUTTON", "...", WS_CHILD | WS_VISIBLE,
                  566, 10, 34, 24, hwnd, (HMENU)BTN_BROWSE, NULL, NULL);

    g_launch_button = CreateWindowA("BUTTON", "Patch", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                    608, 10, 128, 28, hwnd, (HMENU)BTN_LAUNCH, NULL, NULL);

    CreateWindowA("BUTTON", "Import", WS_CHILD | WS_VISIBLE,
                  744, 10, 78, 28, hwnd, (HMENU)BTN_IMPORT, NULL, NULL);

    CreateWindowA("BUTTON", "Save", WS_CHILD | WS_VISIBLE,
                  830, 10, 72, 28, hwnd, (HMENU)BTN_SAVE, NULL, NULL);

    CreateWindowA("BUTTON", "Load", WS_CHILD | WS_VISIBLE,
                  910, 10, 72, 28, hwnd, (HMENU)BTN_LOAD, NULL, NULL);

    CreateWindowA("BUTTON", "Clear", WS_CHILD | WS_VISIBLE,
                  990, 10, 72, 28, hwnd, (HMENU)BTN_CLEAR, NULL, NULL);

    int x = 12;
    for (size_t i = 0; i < sizeof(g_tiles) / sizeof(g_tiles[0]); ++i) {
        CreateWindowA("BUTTON", g_tiles[i].name, WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                      x, 44, 82, 24, hwnd, (HMENU)(TILE_BASE + i), NULL, NULL);
        if (g_tiles[i].id == g_selected_tile) {
            SendMessage(GetDlgItem(hwnd, TILE_BASE + (int)i), BM_SETCHECK, BST_CHECKED, 0);
        }
        x += 88;
    }
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        init_default_stage();
        create_controls(hwnd);
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(wparam);
        if (id == BTN_BROWSE) {
            browse_game(hwnd);
            return 0;
        }
        if (id == BTN_CLEAR) {
            ZeroMemory(g_map, sizeof(g_map));
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (id == BTN_IMPORT) {
            char error[256];
            if (import_stage_from_game(error, sizeof(error))) {
                InvalidateRect(hwnd, NULL, FALSE);
                MessageBoxA(hwnd, "Stage imported.", APP_TITLE, MB_OK | MB_ICONINFORMATION);
            } else {
                MessageBoxA(hwnd, error, APP_TITLE, MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        if (id == BTN_SAVE) {
            save_stage_file(hwnd);
            return 0;
        }
        if (id == BTN_LOAD) {
            load_stage_file(hwnd);
            return 0;
        }
        if (id == BTN_LAUNCH) {
            start_patch_async(hwnd);
            return 0;
        }
        if (id >= TILE_BASE && id < TILE_BASE + (int)(sizeof(g_tiles) / sizeof(g_tiles[0]))) {
            g_selected_tile = g_tiles[id - TILE_BASE].id;
            return 0;
        }
        break;
    }

    case WM_PATCH_DONE: {
        PatchResult *result = (PatchResult *)lparam;
        EnableWindow(g_launch_button, TRUE);
        SetWindowTextA(g_launch_button, "Patch");
        InterlockedExchange(&g_patch_running, 0);
        if (result != NULL) {
            MessageBoxA(hwnd, result->message, APP_TITLE,
                        MB_OK | (result->ok ? MB_ICONINFORMATION : MB_ICONERROR));
            free(result);
        }
        return 0;
    }

    case WM_LBUTTONDOWN:
    case WM_MOUSEMOVE:
        if (msg == WM_LBUTTONDOWN || (wparam & MK_LBUTTON)) {
            paint_cell_from_mouse(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        }
        return 0;

    case WM_RBUTTONDOWN: {
        int old = g_selected_tile;
        g_selected_tile = 0;
        paint_cell_from_mouse(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        g_selected_tile = old;
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(230, 234, 240));
        draw_grid(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev, LPSTR cmdline, int show)
{
    (void)prev;
    (void)cmdline;

    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = "SyobonStagePatcherWindow";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(22, 27, 34));

    if (!RegisterClassA(&wc)) return 1;

    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, APP_TITLE,
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                GRID_X * 2 + MAP_COLS * CELL + 18,
                                GRID_Y + MAP_ROWS * CELL + 52,
                                NULL, NULL, instance, NULL);
    if (hwnd == NULL) return 1;

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}
