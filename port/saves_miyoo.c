/*
 * Miyoo Mini: EEPROM (recordes/opcoes) e Controller Pak (fantasmas) em arquivos simples.
 *   ./saves/mk64.eep  512 bytes (EEPROM 4K, enderecos em blocos de 8 bytes)
 *   ./saves/mk64.gho  arquivo unico do Controller Pak (dados do fantasma)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <ultra64.h>
#include <macros.h>
#include "save.h"
#include "save_data.h"

extern SaveData gSaveData;
extern s32 D_800DC5AC;

#define SAVE_DIR "saves"
#define EEP_PATH SAVE_DIR "/mk64.eep"
#define GHO_PATH SAVE_DIR "/mk64.gho"
#define EEP_SIZE 512
#define GHO_MIN_SIZE 32768

int32_t ControllerPakStatus;
int32_t FilesUsed = 0;
int32_t Pak_Memory = 0;
int32_t Pak_Size = 0;

static uint8_t eep[EEP_SIZE];
static int eep_loaded = 0;

static OSPfsState gho_state;
static int gho_state_valid = 0;

static void ensure_dir(void) {
    mkdir(SAVE_DIR, 0777);
}

static long file_size(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    return (long) st.st_size;
}

static int write_whole(const char* path, const void* data, size_t len) {
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return -1;
    const uint8_t* p = data;
    size_t left = len;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w <= 0) {
            close(fd);
            return -1;
        }
        p += w;
        left -= (size_t) w;
    }
    fsync(fd);
    close(fd);
    if (rename(tmp, path) != 0)
        return -1;
    sync();
    return 0;
}

/* ---------------- EEPROM ---------------- */

s32 osEepromProbe(UNUSED OSMesgQueue* mq) {
    ensure_dir();
    if (file_size(EEP_PATH) == EEP_SIZE) {
        FILE* f = fopen(EEP_PATH, "rb");
        if (f) {
            if (fread(eep, 1, EEP_SIZE, f) == EEP_SIZE)
                eep_loaded = 1;
            fclose(f);
        }
    }
    if (!eep_loaded) {
        memset(eep, 0, EEP_SIZE);
        write_whole(EEP_PATH, eep, EEP_SIZE);
        eep_loaded = 1;
        /* igual ao port DC: inicializa os dados (ver menus.c) */
        func_800B46D0();
        D_800DC5AC = 0;
    }
    return EEPROM_TYPE_4K;
}

s32 osEepromLongRead(UNUSED OSMesgQueue* mq, unsigned char address, unsigned char* buffer, s32 length) {
    if (!eep_loaded)
        osEepromProbe(mq);
    int off = address * 8;
    if (off + length > EEP_SIZE || length < 0)
        return 1;
    memcpy(buffer, eep + off, length);
    return 0;
}

s32 osEepromRead(OSMesgQueue* mq, u8 address, u8* buffer) {
    return osEepromLongRead(mq, address, buffer, 8);
}

s32 osEepromLongWrite(UNUSED OSMesgQueue* mq, unsigned char address, unsigned char* buffer, s32 length) {
    if (!eep_loaded)
        osEepromProbe(mq);
    int off = address * 8;
    if (off + length > EEP_SIZE || length < 0)
        return 1;
    memcpy(eep + off, buffer, length);
    ensure_dir();
    return write_whole(EEP_PATH, eep, EEP_SIZE) ? 1 : 0;
}

s32 osEepromWrite(OSMesgQueue* mq, unsigned char address, unsigned char* buffer) {
    return osEepromLongWrite(mq, address, buffer, 8);
}

/* after a save state is loaded (savestate_miyoo.c): the game's copy of the save data back
   from the EEPROM file, so a state never rolls the records and the cups won back (nor
   writes the old ones over them at the next save) */
void port_saves_reapply(void) {
    if (!eep_loaded)
        return;
    memcpy(&gSaveData, eep, sizeof(gSaveData) < EEP_SIZE ? sizeof(gSaveData) : EEP_SIZE);
}

/* ---------------- Controller Pak (um unico arquivo) ---------------- */

int vmu_status(int channel) {
    ControllerPakStatus = 0;
    FilesUsed = -1;
    if (channel != 0)
        return PFS_ERR_NOPACK;
    ensure_dir();
    long sz = file_size(GHO_PATH);
    Pak_Memory = 200;
    FilesUsed = 0;
    if (sz > 0) {
        Pak_Memory -= (int32_t) ((sz + 511) / 512);
        FilesUsed = 1;
    }
    ControllerPakStatus = 1;
    return 0;
}

void draw_vmu_icon(UNUSED int controller, UNUSED int charid) {
}

s32 osPfsIsPlug(UNUSED OSMesgQueue* queue, u8* pattern) {
    *pattern = 0;
    if (!vmu_status(0))
        *pattern = 1;
    return 1;
}

s32 osPfsInit(OSMesgQueue* queue, OSPfs* pfs, int channel) {
    if (vmu_status(channel))
        return PFS_NO_PAK_INSERTED;
    pfs->queue = queue;
    pfs->channel = channel;
    pfs->status = PFS_INITIALIZED;
    return PFS_NO_ERROR;
}

s32 osPfsNumFiles(UNUSED OSPfs* pfs, s32* max_files, s32* files_used) {
    vmu_status(0);
    *max_files = 16;
    *files_used = FilesUsed < 0 ? 0 : FilesUsed;
    return 0;
}

s32 osPfsFileState(UNUSED OSPfs* pfs, UNUSED s32 file_no, UNUSED OSPfsState* state) {
    return PFS_NO_ERROR;
}

s32 osPfsFreeBlocks(OSPfs* pfs, s32* bytes_not_used) {
    vmu_status(pfs->channel);
    *bytes_not_used = Pak_Memory * 512;
    return PFS_NO_ERROR;
}

void __osPfsCloseAllFiles(void) {
}

static void set_state(u16 company_code, u32 game_code, u8* game_name, u8* ext_name, long size) {
    memset(&gho_state, 0, sizeof(gho_state));
    gho_state.company_code = company_code;
    gho_state.game_code = game_code;
    memcpy(gho_state.game_name, game_name, sizeof(gho_state.game_name));
    memcpy(gho_state.ext_name, ext_name, sizeof(gho_state.ext_name));
    gho_state.file_size = (u32) size;
    gho_state_valid = 1;
}

s32 osPfsFindFile(OSPfs* pfs, u16 company_code, u32 game_code, u8* game_name, u8* ext_name, s32* file_no) {
    if (pfs->channel != 0)
        return PFS_NO_PAK_INSERTED;
    long sz = file_size(GHO_PATH);
    if (sz <= 0)
        return PFS_ERR_INVALID;
    set_state(company_code, game_code, game_name, ext_name, sz);
    *file_no = 0;
    return PFS_NO_ERROR;
}

s32 osPfsAllocateFile(OSPfs* pfs, u16 company_code, u32 game_code, u8* game_name, u8* ext_name,
                      int file_size_in_bytes, s32* file_no) {
    if (pfs->channel != 0)
        return PFS_NO_PAK_INSERTED;
    ensure_dir();
    long sz = file_size_in_bytes > GHO_MIN_SIZE ? file_size_in_bytes : GHO_MIN_SIZE;
    void* z = calloc(1, sz);
    if (!z)
        return PFS_INVALID_DATA;
    int rv = write_whole(GHO_PATH, z, sz);
    free(z);
    if (rv)
        return PFS_CORRUPTED;
    set_state(company_code, game_code, game_name, ext_name, sz);
    *file_no = 0;
    return PFS_NO_ERROR;
}

s32 osPfsDeleteFile(OSPfs* pfs, UNUSED u16 company_code, UNUSED u32 game_code, UNUSED u8* game_name,
                    UNUSED u8* ext_name) {
    if (pfs->channel != 0)
        return PFS_ERR_NOPACK;
    gho_state_valid = 0;
    if (unlink(GHO_PATH) != 0)
        return PFS_ERR_ID_FATAL;
    sync();
    return PFS_NO_ERROR;
}

s32 osPfsReadWriteFile(UNUSED OSPfs* pfs, s32 file_no, u8 flag, int offset, int size_in_bytes, u8* data_buffer) {
    if (file_no != 0 || offset < 0 || size_in_bytes < 0)
        return PFS_ERR_INVALID;
    int fd = open(GHO_PATH, flag == PFS_READ ? O_RDONLY : O_RDWR);
    if (fd < 0)
        return PFS_ERR_INVALID;
    ssize_t r;
    if (flag == PFS_READ) {
        r = pread(fd, data_buffer, size_in_bytes, offset);
        close(fd);
        return (r == size_in_bytes) ? PFS_NO_ERROR : PFS_ERR_BAD_DATA;
    }
    r = pwrite(fd, data_buffer, size_in_bytes, offset);
    fsync(fd);
    close(fd);
    sync();
    return (r == size_in_bytes) ? PFS_NO_ERROR : PFS_CORRUPTED;
}
