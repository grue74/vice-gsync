/*
 * ultimate_uci.c - Ultimate Command Interface (1541 Ultimate-II / Ultimate 64) emulation.
 *
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

/*
    The Ultimate's "Command Interface" at $DF1C-$DF1F, as an I/O-only device that sits
    beside a cartridge (like the RTC or Digimax). Modelled on the firmware source,
    GideonZ/1541ultimate tag v3.15a: fpga/io/command_interface/vhdl_source/
    command_protocol.vhd (registers), software/io/command_interface/command_intf.cc
    (dispatch), software/io/network/network_target.cc, software/io/command_interface/
    control_target.cc and software/filemanager/dos.cc (targets).

    $DF1C  write: control  bit0 PUSH_CMD, bit1 DATA_ACC, bit2 ABORT, bit3 CLR_ERR
           read:  status   bit7 DATA_AV, bit6 STAT_AV, bits5-4 state (00 idle,
                           01 busy, 10 data last, 11 data more), bit3 ERROR,
                           bit2 ABORT pending, bit1 DATA_ACC, bit0 CMD_BUSY
    $DF1D  write: command byte          read: $C9 (identification)
    $DF1E  read:  response data (advances; $00 when none)
    $DF1F  read:  status text (advances; $00 when none)

    A command runs at once when pushed: the state goes straight from idle to a data
    state (the real one is busy for a while; programs wait on the busy state anyway).

    Targets:
      1, 2  DOS: IDENTIFY, OPEN/CLOSE_FILE, READ/WRITE_DATA, CHANGE_DIR, GET_PATH,
            COPY_HOME_PATH, ECHO. Files live in memory (folders "/", "/Temp", "/Usb0");
            a file written is logged when closed. -ucitempfs off: opening in /Temp
            fails as on a U64 with firmware 3.15a ("NO FILESYSTEM").
      3     network: IDENTIFY, GET_INTERFACE_COUNT, GET_NETADDR (-ucimac), GET_IPADDR,
            OPEN_TCP (real TCP to the host, IPv4 like the firmware's lwIP),
            CLOSE_SOCKET, READ_SOCKET (never waits: "02,NO DATA"), WRITE_SOCKET.
      4     control: IDENTIFY, GET_HWINFO (device 0: -uciproduct; device 1: one SID
            at $D400), LOAD_CONFIG $50 (logs the file; +uciloadconfig answers
            "21,UNKNOWN COMMAND" as a C64 Ultimate does).
    Anything else: "21,UNKNOWN COMMAND".
*/

#include "vice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartio.h"
#include "cartridge.h"
#include "cmdline.h"
#include "lib.h"
#include "log.h"
#include "monitor.h"
#include "resources.h"
#include "ultimate_uci.h"
#include "util.h"
#include "vicesocket.h"

#define UCI_ID          0xc9

#define CMD_MAX         896     /* command buffer; the last byte is overwritten when full */
#define BLOCK_MAX       895     /* response block ("a block of exactly 896 never ends") */
#define STATUS_MAX      256

#define ST_IDLE         0x00
#define ST_BUSY         0x10
#define ST_LAST         0x20
#define ST_MORE         0x30

#define CTL_PUSH        0x01
#define CTL_DATA_ACC    0x02
#define CTL_ABORT       0x04
#define CTL_CLR_ERR     0x08

#define NO_REPLY        0x80    /* in the target byte: no answer wanted */

#define MAX_SOCKETS     8
#define READ_MAX        1472
#define MAX_FILES       16
#define FILE_MAX        65536

static log_t uci_log = LOG_DEFAULT;

/* resources */
static int uci_enabled = 0;
static char *uci_mac = NULL;            /* "02:55:43:49:00:01" */
static char *uci_product = NULL;        /* GET_HWINFO device 0 */
static int uci_load_config = 1;         /* control $50 known (firmware 3.15+) */
static int uci_temp_fs = 1;             /* /Temp takes new files */

/* interface state */
static uint8_t cmd[CMD_MAX];
static int cmd_len;
static int state;
static int error_flag;
static int data_acc_flag;

static uint8_t *reply;                  /* the whole answer, cut into blocks */
static int reply_len, reply_at;         /* reply_at: start of the current block */
static int block_len, block_ptr;
static char status_text[STATUS_MAX];
static int status_len, status_ptr;

/* network */
static vice_network_socket_t *sockets[MAX_SOCKETS];

/* DOS: a tiny in-memory file system */
typedef struct {
    char path[256];
    uint8_t *data;
    int len;
} mem_file_t;
static mem_file_t files[MAX_FILES];
static char cwd[256] = "/Usb0/";
static mem_file_t *open_file = NULL;
static int open_pos;
static int open_write;

static uint8_t uci_read(uint16_t addr);
static uint8_t uci_peek(uint16_t addr);
static void uci_store(uint16_t addr, uint8_t value);
static int uci_dump(void);

static io_source_t uci_device = {
    CARTRIDGE_NAME_ULTIMATE_UCI,    /* name of the device */
    IO_DETACH_RESOURCE,             /* use resource to detach the device when involved in a read-collision */
    "UltimateUCI",                  /* resource to set to '0' */
    0xdf1c, 0xdf1f, 0x03,           /* range for the device, regs: $df1c-$df1f */
    1,                              /* read is always valid */
    uci_store,                      /* store function */
    NULL,                           /* NO poke function */
    uci_read,                       /* read function */
    uci_peek,                       /* peek function */
    uci_dump,                       /* device state information dump function */
    CARTRIDGE_ULTIMATE_UCI,         /* cartridge ID */
    IO_PRIO_HIGH,                   /* wins over the cartridge's I/O2 */
    0,                              /* insertion order, gets filled in by the registration function */
    IO_MIRROR_NONE                  /* NO mirroring */
};

static io_source_list_t *uci_list_item = NULL;

/* ---------------------------------------------------------------------*/
/* answers */

static void set_status(const char *text)
{
    strncpy(status_text, text, STATUS_MAX - 1);
    status_text[STATUS_MAX - 1] = 0;
    status_len = (int)strlen(status_text);
}

static void reply_reset(void)
{
    lib_free(reply);
    reply = NULL;
    reply_len = reply_at = block_len = block_ptr = 0;
    status_len = status_ptr = 0;
}

static void reply_add(const void *bytes, int n)
{
    if (n <= 0) {
        return;
    }
    reply = lib_realloc(reply, (size_t)(reply_len + n));
    memcpy(reply + reply_len, bytes, (size_t)n);
    reply_len += n;
}

static void reply_add_text(const char *text)
{
    reply_add(text, (int)strlen(text));
}

/* show the block starting at reply_at: "more" when bytes follow it */
static void show_block(void)
{
    block_len = reply_len - reply_at;
    if (block_len > BLOCK_MAX) {
        block_len = BLOCK_MAX;
    }
    block_ptr = 0;
    state = (reply_at + block_len < reply_len) ? ST_MORE : ST_LAST;
}

static void to_idle(void)
{
    reply_reset();
    state = ST_IDLE;
    cmd_len = 0;
}

/* a command argument from byte `at` on as a C string (up to a 0 or the end) */
static void arg_string(int at, char *out, int size)
{
    int i = 0;
    while (at < cmd_len && cmd[at] != 0 && i < size - 1) {
        out[i++] = (char)cmd[at++];
    }
    out[i] = 0;
}

/* ---------------------------------------------------------------------*/
/* network target (3) */

static void net_close_all(void)
{
    int i;
    for (i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i]) {
            vice_network_socket_close(sockets[i]);
            sockets[i] = NULL;
        }
    }
}

/* the socket for a handle byte, or NULL */
static vice_network_socket_t *net_socket(int handle)
{
    return (handle >= 1 && handle <= MAX_SOCKETS) ? sockets[handle - 1] : NULL;
}

static void parse_mac(uint8_t *mac)
{
    unsigned int b[6] = { 0x02, 0x55, 0x43, 0x49, 0x00, 0x01 };
    int i;
    if (uci_mac) {
        sscanf(uci_mac, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    }
    for (i = 0; i < 6; i++) {
        mac[i] = (uint8_t)b[i];
    }
}

static void net_command(void)
{
    char text[64];

    switch (cmd[1]) {
        case 0x01:  /* IDENTIFY */
            reply_add_text("ULTIMATE-II NETWORK INTERFACE V1.0");
            set_status("00,OK");
            break;
        case 0x02: { /* GET_INTERFACE_COUNT */
            uint8_t one = 1;
            reply_add(&one, 1);
            set_status("00,OK");
            break;
        }
        case 0x04:  /* GET_NETADDR */
        case 0x05: { /* GET_IPADDR */
            if (cmd_len != 3) {
                set_status("81,INVALID PARAMS");
            } else if (cmd[2] != 0) {
                set_status("82,PARAMETER(S) OUT OF RANGE");
            } else if (cmd[1] == 0x04) {
                uint8_t mac[6];
                parse_mac(mac);
                reply_add(mac, 6);
                set_status("00,OK");
            } else {
                static const uint8_t ip[12] = { 192, 168, 64, 64, 255, 255, 255, 0, 192, 168, 64, 1 };
                reply_add(ip, 12);
                set_status("00,OK");
            }
            break;
        }
        case 0x07:  /* OPEN_TCP */
        case 0x08: { /* OPEN_UDP (TCP only here) */
            char host[256], addr[264];
            int port, slot;
            vice_network_socket_address_t *sa;
            vice_network_socket_t *s;

            if (cmd_len < 5) {
                set_status("81,INVALID PARAMS");
                break;
            }
            if (cmd[1] == 0x08) {
                set_status("85,ERROR OPENING SOCKET");
                break;
            }
            port = cmd[2] | (cmd[3] << 8);
            arg_string(4, host, sizeof host);
            for (slot = 0; slot < MAX_SOCKETS && sockets[slot]; slot++) {
            }
            if (slot == MAX_SOCKETS) {
                set_status("85,ERROR OPENING SOCKET");
                break;
            }
            snprintf(addr, sizeof addr, "ip4://%s", host);   /* lwIP: IPv4 only */
            sa = vice_network_address_generate(addr, (unsigned short)port);
            if (sa == NULL) {
                log_message(uci_log, "OPEN_TCP %s:%d: unresolved host", host, port);
                set_status("84,UNRESOLVED HOST");
                break;
            }
            s = vice_network_client(sa);
            vice_network_address_close(sa);
            if (s == NULL) {
                log_message(uci_log, "OPEN_TCP %s:%d: connect failed", host, port);
                set_status("11,ERROR ON CONNECT: 104");
                break;
            }
            sockets[slot] = s;
            {
                uint8_t handle = (uint8_t)(slot + 1);
                reply_add(&handle, 1);
            }
            log_message(uci_log, "OPEN_TCP %s:%d -> socket %d", host, port, slot + 1);
            set_status("00,OK");
            break;
        }
        case 0x09: { /* CLOSE_SOCKET */
            vice_network_socket_t *s;
            if (cmd_len != 3) {
                set_status("81,INVALID PARAMS");
                break;
            }
            s = net_socket(cmd[2]);
            if (s == NULL) {
                set_status("12,ERROR ON CLOSE: 9");
                break;
            }
            vice_network_socket_close(s);
            sockets[cmd[2] - 1] = NULL;
            log_message(uci_log, "CLOSE_SOCKET %d", cmd[2]);
            set_status("00,OK");
            break;
        }
        case 0x10: { /* READ_SOCKET */
            vice_network_socket_t *s;
            int want, got;
            uint8_t buf[READ_MAX], head[2];

            if (cmd_len != 5) {
                set_status("81,INVALID PARAMS");
                break;
            }
            want = cmd[3] | (cmd[4] << 8);
            if (want > READ_MAX) {
                set_status("82,PARAMETER(S) OUT OF RANGE");
                break;
            }
            s = net_socket(cmd[2]);
            if (s == NULL || vice_network_select_poll_one(s) <= 0) {
                /* nothing waiting (the firmware waits 40 ms first; we don't) */
                head[0] = head[1] = 0xff;
                reply_add(head, 2);
                set_status("02,NO DATA: 11");
                break;
            }
            got = (int)vice_network_receive(s, buf, (size_t)want, 0);
            if (got <= 0) {
                /* closed by the host (or reset): socket gone */
                vice_network_socket_close(s);
                sockets[cmd[2] - 1] = NULL;
                head[0] = head[1] = 0;
                reply_add(head, 2);
                log_message(uci_log, "socket %d closed by the host", cmd[2]);
                set_status("01,CONNECTION CLOSED BY HOST");
                break;
            }
            head[0] = (uint8_t)(got & 0xff);
            head[1] = (uint8_t)(got >> 8);
            reply_add(head, 2);
            reply_add(buf, got);
            set_status("00,OK");
            break;
        }
        case 0x11: { /* WRITE_SOCKET */
            vice_network_socket_t *s = (cmd_len >= 3) ? net_socket(cmd[2]) : NULL;
            int n = cmd_len - 3, sent = -1;
            uint8_t head[2];

            if (s != NULL && n >= 0) {
                sent = n ? (int)vice_network_send(s, cmd + 3, (size_t)n, 0) : 0;
            }
            head[0] = (uint8_t)(sent & 0xff);
            head[1] = (uint8_t)((sent >> 8) & 0xff);
            reply_add(head, 2);
            if (sent < 0) {
                set_status("12,SEND ERROR: 9");
            } else {
                set_status("00,OK");
            }
            break;
        }
        default:
            snprintf(text, sizeof text, "21,UNKNOWN COMMAND");
            set_status(text);
            break;
    }
}

/* ---------------------------------------------------------------------*/
/* DOS target (1, 2): files in memory */

/* "/Temp" -> "/Temp/"; a relative name joins the current folder */
static void full_path(const char *name, char *out, int size, int as_dir)
{
    if (name[0] == '/') {
        snprintf(out, (size_t)size, "%s", name);
    } else {
        snprintf(out, (size_t)size, "%s%s", cwd, name);
    }
    if (as_dir && out[strlen(out) - 1] != '/' && (int)strlen(out) < size - 1) {
        strcat(out, "/");
    }
}

static int dir_exists(const char *dir)
{
    return !util_strcasecmp(dir, "/") || !util_strcasecmp(dir, "/Temp/") || !util_strcasecmp(dir, "/Usb0/");
}

static mem_file_t *find_file(const char *path, int create)
{
    int i, free_slot = -1;
    for (i = 0; i < MAX_FILES; i++) {
        if (files[i].path[0] && !util_strcasecmp(files[i].path, path)) {
            return &files[i];
        }
        if (!files[i].path[0] && free_slot < 0) {
            free_slot = i;
        }
    }
    if (!create || free_slot < 0) {
        return NULL;
    }
    snprintf(files[free_slot].path, sizeof files[free_slot].path, "%s", path);
    files[free_slot].data = NULL;
    files[free_slot].len = 0;
    return &files[free_slot];
}

static void log_file(const char *what, const mem_file_t *f)
{
    char *text = lib_malloc((size_t)f->len + 1);
    memcpy(text, f->data, (size_t)f->len);
    text[f->len] = 0;
    log_message(uci_log, "%s %s (%d bytes):\n%s", what, f->path, f->len, text);
    lib_free(text);
}

static void dos_close(void)
{
    if (open_file && open_write) {
        log_file("file written", open_file);
    }
    open_file = NULL;
}

static void dos_command(void)
{
    char name[256], path[300];

    switch (cmd[1]) {
        case 0x01:  /* IDENTIFY */
            reply_add_text("ULTIMATE-II DOS V1.2");
            set_status("00,OK");
            break;
        case 0x02: { /* OPEN_FILE: mode, name */
            int mode = cmd_len > 2 ? cmd[2] : 0;
            char dir[300];
            char *slash;
            arg_string(3, name, sizeof name);
            full_path(name, path, sizeof path, 0);
            dos_close();
            snprintf(dir, sizeof dir, "%s", path);
            slash = strrchr(dir, '/');
            if (slash) {
                slash[1] = 0;
            }
            if (mode & 0x02) {          /* FA_WRITE */
                if (!dir_exists(dir)) {
                    set_status("82,FILE NOT FOUND");
                    break;
                }
                if (!uci_temp_fs && !util_strcasecmp(dir, "/Temp/")) {
                    set_status("03,NO FILESYSTEM");     /* U64, firmware 3.15a */
                    break;
                }
                open_file = find_file(path, 1);
                if (open_file == NULL) {
                    set_status("86,DISK FULL");
                    break;
                }
                if (mode & 0x08) {      /* FA_CREATE_ALWAYS */
                    lib_free(open_file->data);
                    open_file->data = NULL;
                    open_file->len = 0;
                }
                open_write = 1;
                open_pos = open_file->len;
            } else {
                open_file = find_file(path, 0);
                if (open_file == NULL) {
                    set_status("82,FILE NOT FOUND");
                    break;
                }
                open_write = 0;
                open_pos = 0;
            }
            set_status("00,OK");
            break;
        }
        case 0x03:  /* CLOSE_FILE */
            if (open_file == NULL) {
                set_status("84,NO FILE TO CLOSE");
            } else {
                dos_close();
                set_status("00,OK");
            }
            break;
        case 0x04: { /* READ_DATA: length lo, hi */
            int want = cmd_len >= 4 ? cmd[2] | (cmd[3] << 8) : 0, n;
            if (open_file == NULL) {
                set_status("85,NO FILE OPEN");
                break;
            }
            n = open_file->len - open_pos;
            if (n > want) {
                n = want;
            }
            reply_add(open_file->data + open_pos, n);
            open_pos += n;
            set_status("");                     /* the firmware answers no status here */
            break;
        }
        case 0x05: { /* WRITE_DATA: 2 unused bytes, then the data */
            int n = cmd_len - 4;
            if (open_file == NULL || !open_write) {
                set_status("85,NO FILE OPEN");
                break;
            }
            if (n > 0 && open_file->len + n <= FILE_MAX) {
                open_file->data = lib_realloc(open_file->data, (size_t)(open_file->len + n));
                memcpy(open_file->data + open_file->len, cmd + 4, (size_t)n);
                open_file->len += n;
            }
            set_status("00,OK");
            break;
        }
        case 0x11:  /* CHANGE_DIR */
            arg_string(2, name, sizeof name);
            full_path(name, path, sizeof path, 1);
            if (!dir_exists(path)) {
                set_status("83,NO SUCH DIRECTORY");
            } else {
                snprintf(cwd, sizeof cwd, "%s", path);
                set_status("00,OK");
            }
            break;
        case 0x17:  /* COPY_HOME_PATH: to the home folder, then as GET_PATH */
            snprintf(cwd, sizeof cwd, "/Usb0/");
            /* fall through */
        case 0x12:  /* GET_PATH */
            reply_add_text(cwd);
            set_status("00,OK");
            break;
        case 0xf0:  /* ECHO */
            reply_add(cmd, cmd_len);
            set_status("00,OK");
            break;
        default:
            set_status("21,UNKNOWN COMMAND");
            break;
    }
}

/* ---------------------------------------------------------------------*/
/* control target (4) */

static void control_command(void)
{
    switch (cmd[1]) {
        case 0x01:  /* IDENTIFY */
            reply_add_text("CONTROL TARGET V1.1");
            set_status("00,OK");
            break;
        case 0x28: { /* GET_HWINFO */
            int device = cmd_len >= 3 ? cmd[2] : 0;
            if (device == 0) {
                reply_add_text(uci_product ? uci_product : "Ultimate 64 Elite");
                set_status("00,OK");
            } else if (device == 1) {
                static const uint8_t sids[6] = { 1, 0x00, 0xd4, 0, 0, 0 };
                reply_add(sids, 6);
                set_status("00,OK");
            } else {
                set_status("81,INVALID PARAMS");
            }
            break;
        }
        case 0x50: { /* LOAD_CONFIG: file name (default /temp/uci_config.cfg) */
            char name[256];
            mem_file_t *f;
            if (!uci_load_config) {
                set_status("21,UNKNOWN COMMAND");   /* C64 Ultimate firmware */
                break;
            }
            arg_string(2, name, sizeof name);
            if (!name[0]) {
                snprintf(name, sizeof name, "/temp/uci_config.cfg");
            }
            f = find_file(name, 0);
            if (f == NULL) {
                log_message(uci_log, "LOAD_CONFIG %s: no such file", name);
                set_status("88,CANNOT OPEN CONFIG FILE");
                break;
            }
            log_file("LOAD_CONFIG", f);
            set_status("00,OK");
            break;
        }
        default:
            set_status("21,UNKNOWN COMMAND");
            break;
    }
}

/* ---------------------------------------------------------------------*/

static void run_command(void)
{
    int target;

    reply_reset();
    if (cmd_len == 0) {                 /* null command: straight to "data last" */
        show_block();
        return;
    }
    target = cmd[0] & 0x0f;
    if (cmd_len < 2) {
        cmd[1] = 0;
    }
    switch (target) {
        case 1:
        case 2:
            dos_command();
            break;
        case 3:
            net_command();
            break;
        case 4:
            control_command();
            break;
        default:
            if (cmd[1] == 0x01) {
                reply_add_text("NO TARGET");
                set_status("00,OK");
            } else {
                set_status("21,UNKNOWN COMMAND");
            }
            break;
    }
    if (cmd[0] & NO_REPLY) {
        to_idle();
        return;
    }
    cmd_len = 0;
    show_block();
}

static uint8_t status_byte(void)
{
    uint8_t s = (uint8_t)(state | (error_flag ? 0x08 : 0) | (data_acc_flag ? 0x02 : 0));
    if (state & 0x20) {
        if (block_ptr < block_len) {
            s |= 0x80;
        }
        if (status_ptr < status_len && state == ST_LAST) {
            s |= 0x40;
        }
    }
    return s;
}

static uint8_t uci_peek(uint16_t addr)
{
    switch (addr & 3) {
        case 0:
            return status_byte();
        case 1:
            return UCI_ID;
        case 2:
            return ((state & 0x20) && block_ptr < block_len) ? reply[reply_at + block_ptr] : 0;
        default:
            return (state == ST_LAST && status_ptr < status_len) ? (uint8_t)status_text[status_ptr] : 0;
    }
}

static uint8_t uci_read(uint16_t addr)
{
    uint8_t v = uci_peek(addr);
    switch (addr & 3) {
        case 2:
            if ((state & 0x20) && block_ptr < block_len) {
                block_ptr++;
            }
            break;
        case 3:
            if (state == ST_LAST && status_ptr < status_len) {
                status_ptr++;
            }
            break;
        default:
            break;
    }
    return v;
}

static void uci_store(uint16_t addr, uint8_t value)
{
    if ((addr & 3) == 1) {              /* command byte */
        if (cmd_len < CMD_MAX - 1) {
            cmd[cmd_len++] = value;
        } else {
            cmd[CMD_MAX - 2] = value;   /* the pointer stops on the last byte */
        }
        return;
    }
    if ((addr & 3) != 0) {
        return;
    }
    /* control */
    if (value & CTL_CLR_ERR) {
        error_flag = 0;
    }
    if (value & CTL_ABORT) {
        to_idle();
        data_acc_flag = 0;
        return;
    }
    if (value & CTL_DATA_ACC) {
        if (state == ST_MORE) {
            reply_at += block_len;
            data_acc_flag = 1;
            show_block();
        } else if (state == ST_LAST) {
            data_acc_flag = 0;
            to_idle();
        }
    }
    if (value & CTL_PUSH) {
        if (state == ST_IDLE) {
            data_acc_flag = 0;
            run_command();
        } else {
            error_flag = 1;
        }
    }
}

static int uci_dump(void)
{
    mon_out("UCI state %02x, command %d bytes, block %d/%d, status \"%.*s\" (%d/%d)\n",
            (unsigned int)state, cmd_len, block_ptr, block_len, status_len, status_text,
            status_ptr, status_len);
    return 0;
}

void ultimate_uci_reset(void)
{
    /* as on the Ultimate: a C64 reset closes all sockets and drops any answer */
    net_close_all();
    dos_close();
    to_idle();
    error_flag = 0;
    data_acc_flag = 0;
}

int ultimate_uci_cart_enabled(void)
{
    return uci_enabled;
}

/* ---------------------------------------------------------------------*/

static int set_uci_enabled(int value, void *param)
{
    int val = value ? 1 : 0;

    if (uci_log == LOG_DEFAULT) {
        uci_log = log_open("UCI");
    }
    if (!uci_enabled && val) {
        uci_list_item = io_source_register(&uci_device);
        ultimate_uci_reset();
        uci_enabled = 1;
    } else if (uci_enabled && !val) {
        if (uci_list_item != NULL) {
            io_source_unregister(uci_list_item);
            uci_list_item = NULL;
        }
        ultimate_uci_reset();
        uci_enabled = 0;
    }
    return 0;
}

static int set_uci_mac(const char *val, void *param)
{
    util_string_set(&uci_mac, val);
    return 0;
}

static int set_uci_product(const char *val, void *param)
{
    util_string_set(&uci_product, val);
    return 0;
}

static int set_uci_load_config(int val, void *param)
{
    uci_load_config = val ? 1 : 0;
    return 0;
}

static int set_uci_temp_fs(int val, void *param)
{
    uci_temp_fs = val ? 1 : 0;
    return 0;
}

static const resource_int_t resources_int[] = {
    { "UltimateUCI", 0, RES_EVENT_STRICT, (resource_value_t)0,
      &uci_enabled, set_uci_enabled, NULL },
    { "UltimateUCILoadConfig", 1, RES_EVENT_NO, NULL,
      &uci_load_config, set_uci_load_config, NULL },
    { "UltimateUCITempFS", 1, RES_EVENT_NO, NULL,
      &uci_temp_fs, set_uci_temp_fs, NULL },
    RESOURCE_INT_LIST_END
};

static const resource_string_t resources_string[] = {
    { "UltimateUCIMAC", "02:55:43:49:00:01", RES_EVENT_NO, NULL,
      &uci_mac, set_uci_mac, NULL },
    { "UltimateUCIProduct", "Ultimate 64 Elite", RES_EVENT_NO, NULL,
      &uci_product, set_uci_product, NULL },
    RESOURCE_STRING_LIST_END
};

int ultimate_uci_resources_init(void)
{
    if (resources_register_string(resources_string) < 0) {
        return -1;
    }
    return resources_register_int(resources_int);
}

void ultimate_uci_resources_shutdown(void)
{
    net_close_all();
    lib_free(uci_mac);
    uci_mac = NULL;
    lib_free(uci_product);
    uci_product = NULL;
}

static const cmdline_option_t cmdline_options[] =
{
    { "-uci", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "UltimateUCI", (resource_value_t)1,
      NULL, "Enable the Ultimate Command Interface ($DF1C-$DF1F)" },
    { "+uci", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "UltimateUCI", (resource_value_t)0,
      NULL, "Disable the Ultimate Command Interface" },
    { "-ucimac", SET_RESOURCE, CMDLINE_ATTRIB_NEED_ARGS,
      NULL, NULL, "UltimateUCIMAC", NULL,
      "<aa:bb:cc:dd:ee:ff>", "MAC address the Ultimate's network interface reports" },
    { "-uciproduct", SET_RESOURCE, CMDLINE_ATTRIB_NEED_ARGS,
      NULL, NULL, "UltimateUCIProduct", NULL,
      "<name>", "Product name GET_HWINFO reports (e.g. \"Ultimate 64 Elite\", \"Ultimate II+\")" },
    { "-uciloadconfig", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "UltimateUCILoadConfig", (resource_value_t)1,
      NULL, "Control target knows LOAD_CONFIG $50 (firmware 3.15 and later)" },
    { "+uciloadconfig", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "UltimateUCILoadConfig", (resource_value_t)0,
      NULL, "LOAD_CONFIG answers \"21,UNKNOWN COMMAND\" (C64 Ultimate firmware)" },
    { "-ucitempfs", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "UltimateUCITempFS", (resource_value_t)1,
      NULL, "New files can be made in /Temp" },
    { "+ucitempfs", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "UltimateUCITempFS", (resource_value_t)0,
      NULL, "Opening a new file in /Temp fails (\"NO FILESYSTEM\", U64 firmware 3.15a)" },
    CMDLINE_LIST_END
};

int ultimate_uci_cmdline_options_init(void)
{
    return cmdline_register_options(cmdline_options);
}
