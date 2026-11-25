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
    The one exception is READ_SOCKET with nothing waiting: the firmware gives every
    socket a 40 ms receive timeout (SO_RCVTIMEO), so the interface stays busy until a
    packet arrives or the 40 ms pass. That is emulated in C64 cycles
    (-ucireadwait <ms>, 0 = answer at once), checking the socket about every 1000
    cycles so a packet that arrives during the wait ends it early, as on the real one.

    Targets:
      1, 2  DOS: IDENTIFY, OPEN/CLOSE_FILE, READ/WRITE_DATA, CHANGE_DIR, GET_PATH,
            COPY_HOME_PATH, ECHO. Files live in memory (folders "/", "/Temp", "/Usb0");
            a file written is logged when closed. -ucitempfs off: opening in /Temp
            fails as on a U64 with firmware 3.15a ("NO FILESYSTEM").
      3     network: IDENTIFY, GET_INTERFACE_COUNT, GET_NETADDR (-ucimac), GET_IPADDR
            (-uciip, or what SET_IPADDR set: that outlives a C64 reset, as on the
            Ultimate), SET_IPADDR,
            OPEN_TCP (real TCP to the host, IPv4 like the firmware's lwIP),
            OPEN_UDP (a real UDP socket; one WRITE_SOCKET sends one datagram to the
            host and port given at open, one READ_SOCKET returns one datagram),
            CLOSE_SOCKET, READ_SOCKET (waits up to -ucireadwait ms, then
            "02,NO DATA: 11"), WRITE_SOCKET.
            UDP as in the firmware: a datagram longer than the READ length is cut
            and the status says "04,DATAGRAM TRUNCATED: <its length>". An empty
            datagram is a 0-byte read with "00,OK" and the socket stays open, as
            in firmware after 3.15a (commit dc1c298a); -uciemptyclose: as in 3.15a,
            it closes the socket like a TCP close ("01,CONNECTION CLOSED BY HOST"). Differences: the firmware
            connect()s its UDP socket, so it only takes datagrams from that host and
            port; here any sender gets through. Windows reports a lost ICMP
            "port unreachable" as a receive error; that reads as "no data" here, as
            lwIP ignores it.
            Error numbers are the firmware's (newlib for nios2, through lwIP's
            err_to_errno): a refused connect is "11,ERROR ON CONNECT: 104" (lwIP
            sees a reset), no answer "...: 113" (lwIP's SYN timeout; here after the
            host's own timeout, with VICE stopped meanwhile). A TCP connection the
            host resets stays open with its error, as in lwIP: the first two READs
            say "02,NO DATA: 104" (113 if aborted), and a WRITE that finds the
            error first says "12,SEND ERROR: 104"; after that every READ and WRITE
            gets 128 (ENOTCONN), at once. A peer's normal close is "01,CONNECTION
            CLOSED BY HOST" and closes the socket. A bad handle gives errno 9.
      4     control: IDENTIFY, GET_HWINFO (device 0: -uciproduct; device 1: one SID
            at $D400), LOAD_CONFIG $50 (logs the file; +uciloadconfig answers
            "21,UNKNOWN COMMAND" as a C64 Ultimate does).
    Anything else: "21,UNKNOWN COMMAND".
*/

#include "vice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alarm.h"
#include "cartio.h"
#include "cartridge.h"
#include "cmdline.h"
#include "lib.h"
#include "log.h"
#include "machine.h"
#include "maincpu.h"
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
#define DATAGRAM_MAX    65536   /* receive buffer for UDP: Windows fails a datagram that doesn't fit */
#define WAIT_POLL       1000    /* cycles between socket checks during the read wait */
#define MAX_FILES       16
#define FILE_MAX        65536

static log_t uci_log = LOG_DEFAULT;

/* resources */
static int uci_enabled = 0;
static char *uci_mac = NULL;            /* "02:55:43:49:00:01" */
static char *uci_product = NULL;        /* GET_HWINFO device 0 */
static char *uci_ip = NULL;             /* -uciip: "ip[/netmask[/gateway]]" */
static int uci_load_config = 1;         /* control $50 known (firmware 3.15+) */
static int uci_temp_fs = 1;             /* /Temp takes new files */
static int uci_empty_closes = 0;        /* an empty UDP datagram closes the socket (fw 3.15a) */
static int uci_read_wait = 40;          /* ms READ_SOCKET waits for data (firmware SO_RCVTIMEO) */

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
static vice_network_socket_address_t *udp_peer[MAX_SOCKETS];   /* UDP: where WRITE sends; NULL = TCP */
static uint8_t *datagram;               /* UDP receive buffer, DATAGRAM_MAX bytes */

/* A TCP connection the host broke (reset, aborted) stays open in the table, as in
   lwIP, and answers with its error the way lwIP does: the error is posted once to
   the receive queue and once as the "pending" error, and each is used up by the
   first READ (queue first) or WRITE (pending only) that finds it; after that every
   READ and WRITE gets ENOTCONN. */
static int broken_errno[MAX_SOCKETS];   /* 0 = fine, else the firmware errno of the break */
static int broken_in_queue[MAX_SOCKETS];
static int broken_pending[MAX_SOCKETS];

/* errno values as the firmware reports them (newlib for nios2, as lwIP's
   err_to_errno gives them) */
#define FW_EBADF        9
#define FW_EWOULDBLOCK  11
#define FW_ECONNRESET   104     /* lwIP ERR_RST: reset by the peer, also a refused connect */
#define FW_ECONNABORTED 113     /* lwIP ERR_ABRT: no answer (connect or retransmit timeout) */
#define FW_ENOTCONN     128     /* lwIP ERR_CONN: the connection is gone */

/* the host's codes from vice_network_get_errorcode() that need telling apart */
#ifdef WINDOWS_COMPILE
#define HOST_EWOULDBLOCK    10035   /* WSAEWOULDBLOCK */
#define HOST_ECONNABORTED   10053   /* WSAECONNABORTED */
#define HOST_ECONNREFUSED   10061   /* WSAECONNREFUSED */
#else
#include <errno.h>
#define HOST_EWOULDBLOCK    EWOULDBLOCK
#define HOST_ECONNABORTED   ECONNABORTED
#define HOST_ECONNREFUSED   ECONNREFUSED
#endif

/* the interface's address (GET_IPADDR / SET_IPADDR): IP, netmask, gateway */
static uint8_t ip_config[12];

/* a READ_SOCKET waiting for data (the interface is busy meanwhile) */
static alarm_t *wait_alarm = NULL;
static int wait_active;
static int wait_handle, wait_want, wait_no_reply;
static CLOCK wait_end;                  /* cycle the wait gives up at */

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

/* close the socket in a slot (0-based) and forget its UDP peer */
static void net_close_slot(int slot)
{
    if (sockets[slot]) {
        vice_network_socket_close(sockets[slot]);
        sockets[slot] = NULL;
    }
    if (udp_peer[slot]) {
        vice_network_address_close(udp_peer[slot]);
        udp_peer[slot] = NULL;
    }
    broken_errno[slot] = broken_in_queue[slot] = broken_pending[slot] = 0;
}

/* The host broke the TCP connection in a slot: from now on it answers as lwIP does
   (see broken_errno). host_code: vice_network_get_errorcode() of the failed call. */
static void net_break(int slot, int host_code)
{
    if (broken_errno[slot]) {
        return;
    }
    broken_errno[slot] = (host_code == HOST_ECONNABORTED) ? FW_ECONNABORTED : FW_ECONNRESET;
    broken_in_queue[slot] = 1;
    broken_pending[slot] = 1;
    log_message(uci_log, "socket %d: connection broken (host error %d), answers errno %d",
                slot + 1, host_code, broken_errno[slot]);
}

/* the errno a READ on a broken connection gets (and uses up) */
static int net_broken_read_errno(int slot)
{
    if (broken_in_queue[slot]) {
        broken_in_queue[slot] = 0;
        return broken_errno[slot];
    }
    if (broken_pending[slot]) {
        broken_pending[slot] = 0;
        return broken_errno[slot];
    }
    return FW_ENOTCONN;
}

/* the errno a WRITE on a broken connection gets (and uses up) */
static int net_broken_write_errno(int slot)
{
    if (broken_pending[slot]) {
        broken_pending[slot] = 0;
        return broken_errno[slot];
    }
    return FW_ENOTCONN;
}

/* the errno a failed TCP connect gives on the firmware: a refused connect is a
   reset to lwIP; anything else (no answer, no route) ends in lwIP's SYN timeout */
static int net_connect_errno(int host_code)
{
    return (host_code == HOST_ECONNREFUSED) ? FW_ECONNRESET : FW_ECONNABORTED;
}

/* parse "ip[/netmask[/gateway]]" into ip_config; a missing netmask is
   255.255.255.0, a missing gateway the address with .1 at the end */
static int parse_ip_config(const char *text, uint8_t *out)
{
    unsigned int v[12] = { 0, 0, 0, 0, 255, 255, 255, 0, 0, 0, 0, 1 };
    int n, i;

    if (text == NULL) {
        return -1;
    }
    n = sscanf(text, "%u.%u.%u.%u/%u.%u.%u.%u/%u.%u.%u.%u",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
               &v[8], &v[9], &v[10], &v[11]);
    if (n != 4 && n != 8 && n != 12) {
        return -1;
    }
    if (n < 12) {
        v[8] = v[0];
        v[9] = v[1];
        v[10] = v[2];
        v[11] = 1;
    }
    for (i = 0; i < 12; i++) {
        if (v[i] > 255) {
            return -1;
        }
        out[i] = (uint8_t)v[i];
    }
    return 0;
}

/* stop a READ_SOCKET wait without answering it */
static void wait_cancel(void)
{
    if (wait_active) {
        alarm_unset(wait_alarm);
        wait_active = 0;
    }
}

static void net_close_all(void)
{
    int i;
    wait_cancel();
    for (i = 0; i < MAX_SOCKETS; i++) {
        net_close_slot(i);
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

/* READ_SOCKET's answer when nothing came: length $FFFF (-1) and "02,NO DATA: errno"
   (11 = EWOULDBLOCK after the timeout, 9 = EBADF for a socket that isn't open) */
static void net_read_no_data(int err)
{
    uint8_t head[2] = { 0xff, 0xff };
    char text[32];

    reply_add(head, 2);
    snprintf(text, sizeof text, "02,NO DATA: %d", err);
    set_status(text);
}

/* Try to answer a READ_SOCKET now. Returns 1 when it is answered (data, closed,
   or a socket that isn't open), 0 when nothing has arrived yet. */
static int net_read_try(int handle, int want)
{
    vice_network_socket_t *s = net_socket(handle);
    int slot = handle - 1, got, copied;
    uint8_t buf[READ_MAX], head[2];
    char text[48];

    if (s == NULL) {
        net_read_no_data(FW_EBADF);
        return 1;
    }
    if (broken_errno[slot]) {
        /* a broken connection answers at once, with no wait (lwIP: queue closed) */
        net_read_no_data(net_broken_read_errno(slot));
        return 1;
    }
    if (vice_network_select_poll_one(s) <= 0) {
        return 0;
    }

    if (udp_peer[slot] != NULL) {
        /* UDP: take the whole datagram, hand out as much as was asked for */
        if (datagram == NULL) {
            datagram = lib_malloc(DATAGRAM_MAX);
        }
        got = (int)vice_network_receive_from(s, datagram, DATAGRAM_MAX, 0);
        if (got < 0) {
            /* Windows: an ICMP "port unreachable" from an earlier send.
               lwIP ignores those, so it counts as nothing received. */
            return 0;
        }
        if (got == 0) {
            head[0] = head[1] = 0;
            reply_add(head, 2);
            if (uci_empty_closes) {
                /* firmware 3.15a treats an empty datagram as a closed peer */
                net_close_slot(slot);
                log_message(uci_log, "socket %d: empty datagram, closed (as firmware 3.15a)", handle);
                set_status("01,CONNECTION CLOSED BY HOST");
            } else {
                /* later firmware (commit dc1c298a): it is data, of length 0 */
                set_status("00,OK");
            }
            return 1;
        }
        copied = got > want ? want : got;
        head[0] = (uint8_t)(copied & 0xff);
        head[1] = (uint8_t)(copied >> 8);
        reply_add(head, 2);
        reply_add(datagram, copied);
        if (got > want) {
            snprintf(text, sizeof text, "04,DATAGRAM TRUNCATED: %d", got);
            set_status(text);
        } else {
            set_status("00,OK");
        }
        return 1;
    }

    /* TCP: as much of the stream as fits */
    got = (int)vice_network_receive(s, buf, (size_t)want, 0);
    if (got < 0) {
        int code = vice_network_get_errorcode();
        if (code == HOST_EWOULDBLOCK) {
            return 0;
        }
        /* reset or aborted: the socket stays open with the error, as in lwIP */
        net_break(slot, code);
        net_read_no_data(net_broken_read_errno(slot));
        return 1;
    }
    if (got == 0) {
        /* closed by the peer (FIN): the firmware closes the socket */
        net_close_slot(slot);
        head[0] = head[1] = 0;
        reply_add(head, 2);
        log_message(uci_log, "socket %d closed by the host", handle);
        set_status("01,CONNECTION CLOSED BY HOST");
        return 1;
    }
    head[0] = (uint8_t)(got & 0xff);
    head[1] = (uint8_t)(got >> 8);
    reply_add(head, 2);
    reply_add(buf, got);
    set_status("00,OK");
    return 1;
}

/* The answer of a command is ready: hand it out, or drop it when none was wanted. */
static void finish_command(int no_reply)
{
    if (no_reply) {
        to_idle();
        return;
    }
    cmd_len = 0;
    show_block();
}

/* the read wait's alarm: check the socket, answer when data came or time is up */
static void wait_alarm_handler(CLOCK offset, void *data)
{
    if (!wait_active) {
        alarm_unset(wait_alarm);
        return;
    }
    if (net_read_try(wait_handle, wait_want)) {
        alarm_unset(wait_alarm);
        wait_active = 0;
        finish_command(wait_no_reply);
    } else if (maincpu_clk >= wait_end) {
        alarm_unset(wait_alarm);
        wait_active = 0;
        net_read_no_data(FW_EWOULDBLOCK);
        finish_command(wait_no_reply);
    } else {
        alarm_set(wait_alarm, maincpu_clk + WAIT_POLL);
    }
}

/* start a READ_SOCKET wait: the interface is busy until wait_alarm_handler answers */
static void wait_start(int handle, int want)
{
    CLOCK cycles = (CLOCK)((double)machine_get_cycles_per_second() * uci_read_wait / 1000.0);

    if (wait_alarm == NULL) {
        wait_alarm = alarm_new(maincpu_alarm_context, "UltimateUCIReadWait", wait_alarm_handler, NULL);
    }
    wait_active = 1;
    wait_handle = handle;
    wait_want = want;
    wait_no_reply = (cmd[0] & NO_REPLY) != 0;
    wait_end = maincpu_clk + cycles;
    alarm_set(wait_alarm, maincpu_clk + (cycles < WAIT_POLL ? cycles : WAIT_POLL));
    state = ST_BUSY;
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
                reply_add(ip_config, 12);       /* -uciip, or the last SET_IPADDR */
                set_status("00,OK");
            }
            break;
        }
        case 0x06:  /* SET_IPADDR: interface, IP, netmask, gateway (4 bytes each) */
            if (cmd_len != 15) {
                set_status("81,INVALID PARAMS");
            } else if (cmd[2] != 0) {
                set_status("82,PARAMETER(S) OUT OF RANGE");
            } else {
                /* the Ultimate's own setting: it outlives a C64 reset */
                memcpy(ip_config, cmd + 3, 12);
                log_message(uci_log, "SET_IPADDR %d.%d.%d.%d/%d.%d.%d.%d/%d.%d.%d.%d",
                            cmd[3], cmd[4], cmd[5], cmd[6], cmd[7], cmd[8], cmd[9], cmd[10],
                            cmd[11], cmd[12], cmd[13], cmd[14]);
                set_status("00,OK");
            }
            break;
        case 0x07:  /* OPEN_TCP */
        case 0x08: { /* OPEN_UDP */
            char host[256], addr[264];
            int port, slot;
            vice_network_socket_address_t *sa;
            vice_network_socket_t *s;

            if (cmd_len < 5) {
                set_status("81,INVALID PARAMS");
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
                log_message(uci_log, "OPEN_%s %s:%d: unresolved host",
                            cmd[1] == 0x08 ? "UDP" : "TCP", host, port);
                set_status("84,UNRESOLVED HOST");
                break;
            }
            if (cmd[1] == 0x08) {
                /* UDP: bound to any local port, so a READ before the first WRITE
                   works; the address given is where every WRITE goes */
                vice_network_socket_address_t *local = vice_network_address_generate("ip4://", 0);
                s = local ? vice_network_udp_open(local) : NULL;
                if (local) {
                    vice_network_address_close(local);
                }
                if (s == NULL) {
                    vice_network_address_close(sa);
                    log_message(uci_log, "OPEN_UDP %s:%d: no socket", host, port);
                    set_status("85,ERROR OPENING SOCKET");
                    break;
                }
                sockets[slot] = s;
                udp_peer[slot] = sa;
                {
                    uint8_t handle = (uint8_t)(slot + 1);
                    reply_add(&handle, 1);
                }
                log_message(uci_log, "OPEN_UDP %s:%d -> socket %d", host, port, slot + 1);
                set_status("00,OK");
                break;
            }
            s = vice_network_client(sa);
            if (s == NULL) {
                int code = vice_network_get_errorcode();
                vice_network_address_close(sa);
                log_message(uci_log, "OPEN_TCP %s:%d: connect failed (host error %d)", host, port, code);
                snprintf(text, sizeof text, "11,ERROR ON CONNECT: %d", net_connect_errno(code));
                set_status(text);
                break;
            }
            vice_network_address_close(sa);
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
            net_close_slot(cmd[2] - 1);
            log_message(uci_log, "CLOSE_SOCKET %d", cmd[2]);
            set_status("00,OK");
            break;
        }
        case 0x10: { /* READ_SOCKET: handle, length lo, hi */
            int want;

            if (cmd_len != 5) {
                set_status("81,INVALID PARAMS");
                break;
            }
            want = cmd[3] | (cmd[4] << 8);
            if (want > READ_MAX) {
                set_status("82,PARAMETER(S) OUT OF RANGE");
                break;
            }
            if (net_read_try(cmd[2], want)) {
                break;
            }
            if (uci_read_wait > 0) {
                /* nothing yet: stay busy until data comes or the timeout passes */
                wait_start(cmd[2], want);
                break;
            }
            net_read_no_data(FW_EWOULDBLOCK);
            break;
        }
        case 0x11: { /* WRITE_SOCKET */
            vice_network_socket_t *s = (cmd_len >= 3) ? net_socket(cmd[2]) : NULL;
            int n = cmd_len - 3, sent = -1, err = FW_EBADF, slot = cmd[2] - 1;
            uint8_t head[2];

            if (s != NULL && n >= 0 && udp_peer[slot] != NULL) {
                /* UDP: one write, one datagram (an empty one too, as lwip_send).
                   lwIP reports no failure of its own here: a datagram the host
                   can't send is lost, as one lwIP can't deliver is. */
                sent = (int)vice_network_send_to(s, cmd + 3, (size_t)n, 0, udp_peer[slot]);
                if (sent < 0) {
                    log_message(uci_log, "socket %d: datagram not sent (host error %d)",
                                cmd[2], vice_network_get_errorcode());
                    sent = n;
                }
            } else if (s != NULL && n >= 0 && broken_errno[slot]) {
                err = net_broken_write_errno(slot);
            } else if (s != NULL && n >= 0) {
                sent = n ? (int)vice_network_send(s, cmd + 3, (size_t)n, 0) : 0;
                if (sent < 0) {
                    net_break(slot, vice_network_get_errorcode());
                    err = net_broken_write_errno(slot);
                }
            }
            head[0] = (uint8_t)(sent & 0xff);
            head[1] = (uint8_t)((sent >> 8) & 0xff);
            reply_add(head, 2);
            if (sent < 0) {
                snprintf(text, sizeof text, "12,SEND ERROR: %d", err);
                set_status(text);
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
    if (wait_active) {
        return;                         /* READ_SOCKET waits; its alarm finishes it */
    }
    finish_command(cmd[0] & NO_REPLY);
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
        wait_cancel();
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
    if (wait_active) {
        mon_out("READ_SOCKET %d waiting, %ld cycles left\n", wait_handle,
                (long)(wait_end - maincpu_clk));
    }
    return 0;
}

void ultimate_uci_reset(void)
{
    /* as on the Ultimate: a C64 reset closes all sockets and drops any answer */
    int i, open = 0, waiting = wait_active;
    for (i = 0; i < MAX_SOCKETS; i++) {
        open += sockets[i] != NULL;
    }
    if (open || waiting) {
        log_message(uci_log, "reset: %d socket(s) closed%s", open,
                    waiting ? ", READ wait cancelled" : "");
    }
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

/* -uciip: the address GET_IPADDR reports until a SET_IPADDR changes it */
static int set_uci_ip(const char *val, void *param)
{
    uint8_t parsed[12];

    if (parse_ip_config(val, parsed) < 0) {
        return -1;
    }
    memcpy(ip_config, parsed, 12);
    util_string_set(&uci_ip, val);
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

static int set_uci_empty_closes(int val, void *param)
{
    uci_empty_closes = val ? 1 : 0;
    return 0;
}

static int set_uci_read_wait(int val, void *param)
{
    if (val < 0 || val > 10000) {
        return -1;
    }
    uci_read_wait = val;
    return 0;
}

static const resource_int_t resources_int[] = {
    { "UltimateUCI", 0, RES_EVENT_STRICT, (resource_value_t)0,
      &uci_enabled, set_uci_enabled, NULL },
    { "UltimateUCILoadConfig", 1, RES_EVENT_NO, NULL,
      &uci_load_config, set_uci_load_config, NULL },
    { "UltimateUCITempFS", 1, RES_EVENT_NO, NULL,
      &uci_temp_fs, set_uci_temp_fs, NULL },
    { "UltimateUCIEmptyCloses", 0, RES_EVENT_NO, NULL,
      &uci_empty_closes, set_uci_empty_closes, NULL },
    { "UltimateUCIReadWait", 40, RES_EVENT_NO, NULL,
      &uci_read_wait, set_uci_read_wait, NULL },
    RESOURCE_INT_LIST_END
};

static const resource_string_t resources_string[] = {
    { "UltimateUCIMAC", "02:55:43:49:00:01", RES_EVENT_NO, NULL,
      &uci_mac, set_uci_mac, NULL },
    { "UltimateUCIProduct", "Ultimate 64 Elite", RES_EVENT_NO, NULL,
      &uci_product, set_uci_product, NULL },
    { "UltimateUCIIP", "192.168.64.64", RES_EVENT_NO, NULL,
      &uci_ip, set_uci_ip, NULL },
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
    lib_free(datagram);
    datagram = NULL;
    lib_free(uci_mac);
    uci_mac = NULL;
    lib_free(uci_product);
    uci_product = NULL;
    lib_free(uci_ip);
    uci_ip = NULL;
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
    { "-uciip", SET_RESOURCE, CMDLINE_ATTRIB_NEED_ARGS,
      NULL, NULL, "UltimateUCIIP", NULL,
      "<ip[/netmask[/gateway]]>", "Address GET_IPADDR reports until SET_IPADDR changes it (netmask 255.255.255.0 and gateway x.x.x.1 by default)" },
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
    { "-uciemptyclose", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "UltimateUCIEmptyCloses", (resource_value_t)1,
      NULL, "An empty UDP datagram closes the socket (firmware 3.15a)" },
    { "+uciemptyclose", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "UltimateUCIEmptyCloses", (resource_value_t)0,
      NULL, "An empty UDP datagram is a 0-byte read, the socket stays open (later firmware)" },
    { "-ucireadwait", SET_RESOURCE, CMDLINE_ATTRIB_NEED_ARGS,
      NULL, NULL, "UltimateUCIReadWait", NULL,
      "<ms>", "READ_SOCKET with no data stays busy this long, as the firmware's 40 ms receive timeout (0 = answer at once)" },
    CMDLINE_LIST_END
};

int ultimate_uci_cmdline_options_init(void)
{
    return cmdline_register_options(cmdline_options);
}
