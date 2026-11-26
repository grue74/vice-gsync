/** \file   monitor_network.c
 *  \brief  Monitor implementation - network access
 *
 *  \author Spiro Trikaliotis <spiro.trikaliotis@gmx.de>
 */

/*
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA.
 *
 */

#include "vice.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "cmdline.h"
#include "lib.h"
#include "log.h"
#include "monitor.h"
#include "monitor_network.h"
#include "montypes.h"
#include "resources.h"
#include "ui.h"
#include "uiapi.h"
#include "util.h"
#include "vicesocket.h"

#ifdef HAVE_NETWORK

#define ADDR_LIMIT(x) ((uint16_t)(addr_mask(x)))

static vice_network_socket_t * listen_socket = NULL;
static vice_network_socket_t * connected_socket = NULL;

static char * monitor_server_address = NULL;
static int monitor_enabled = 0;

/* Received text waiting to be split into command lines. It is kept between
   calls, so a line that arrives in pieces (one key at a time) is not lost. */
static char cmd_buffer[260] = { 0 };
static int cmd_bufferpos = 0;

/* Telnet clients (PuTTY in telnet mode, telnet.exe) send option negotiation
   (IAC sequences) and expect CR LF line ends. A connection is treated as telnet
   once it sends an IAC byte; plain clients (netcat, scripts) never do, so for
   them the output stays exactly as before. */
#define TELNET_SE    240
#define TELNET_SB    250
#define TELNET_WILL  251
#define TELNET_WONT  252
#define TELNET_DO    253
#define TELNET_DONT  254
#define TELNET_IAC   255

enum telnet_state_e {
    TELNET_STATE_DATA,
    TELNET_STATE_IAC,
    TELNET_STATE_OPTION,
    TELNET_STATE_SB,
    TELNET_STATE_SB_IAC
};

static int telnet_client = 0;           /* client has sent telnet commands */
static enum telnet_state_e telnet_state = TELNET_STATE_DATA;
static unsigned char telnet_option_cmd = 0;
static int input_last_cr = 0;           /* last received data byte was CR */
static int output_last_cr = 0;          /* last sent byte was CR */

/* forget everything about the previous connection */
static void monitor_network_reset_connection_state(void)
{
    cmd_bufferpos = 0;
    cmd_buffer[0] = 0;
    telnet_client = 0;
    telnet_state = TELNET_STATE_DATA;
    telnet_option_cmd = 0;
    input_last_cr = 0;
    output_last_cr = 0;
}

int monitor_network_transmit(const char * buffer, size_t buffer_length)
{
    int error = 0;

    if (connected_socket && !telnet_client) {
        size_t len = (size_t)vice_network_send(connected_socket, buffer, buffer_length, 0);

        if (len != buffer_length) {
            error = -1;
        } else {
            error = (int)len;
        }
    } else if (connected_socket) {
        /* telnet: LF -> CR LF, and a 0xff data byte is sent twice */
        unsigned char out[512];
        size_t outpos = 0;
        size_t i;

        for (i = 0; i < buffer_length; i++) {
            unsigned char c = (unsigned char)buffer[i];

            if (c == '\n' && !output_last_cr) {
                out[outpos++] = '\r';
            }
            if (c == TELNET_IAC) {
                out[outpos++] = TELNET_IAC;
            }
            out[outpos++] = c;
            output_last_cr = (c == '\r');

            if (outpos >= sizeof out - 3 || i == buffer_length - 1) {
                if ((size_t)vice_network_send(connected_socket, out, outpos, 0) != outpos) {
                    return -1;
                }
                outpos = 0;
            }
        }
        error = (int)buffer_length;
    }

    return error;
}

static void monitor_network_quit(void)
{
    vice_network_socket_close(connected_socket);
    connected_socket = NULL;
    monitor_network_reset_connection_state();
}

/* refuse a telnet option the client offered or asked for */
static void monitor_network_telnet_refuse(unsigned char cmd, unsigned char option)
{
    unsigned char reply[3];

    reply[0] = TELNET_IAC;
    reply[1] = (cmd == TELNET_WILL) ? TELNET_DONT : TELNET_WONT;
    reply[2] = option;
    vice_network_send(connected_socket, reply, sizeof reply, 0);
}

/* Remove telnet commands from received bytes, in place. All options are
   refused, so the client stays in its default mode: it echoes and edits the
   line itself and sends whole lines. Also drops NUL bytes (they would cut the
   line buffer short) and the LF or NUL that follows a CR, so CR LF is one line
   end even when it is split across two packets. Returns the new length. */
static size_t monitor_network_filter_input(unsigned char *buf, size_t len)
{
    size_t in;
    size_t out = 0;

    for (in = 0; in < len; in++) {
        unsigned char c = buf[in];

        switch (telnet_state) {
            case TELNET_STATE_DATA:
                if (c == TELNET_IAC) {
                    telnet_client = 1;
                    telnet_state = TELNET_STATE_IAC;
                    continue;
                }
                if (input_last_cr && (c == '\n' || c == 0)) {
                    input_last_cr = 0;
                    continue;
                }
                input_last_cr = (c == '\r');
                if (c != 0) {
                    buf[out++] = c;
                }
                break;
            case TELNET_STATE_IAC:
                if (c == TELNET_IAC) {
                    /* escaped 0xff: not useful in a monitor command, drop it */
                    telnet_state = TELNET_STATE_DATA;
                } else if (c >= TELNET_WILL && c <= TELNET_DONT) {
                    telnet_option_cmd = c;
                    telnet_state = TELNET_STATE_OPTION;
                } else if (c == TELNET_SB) {
                    telnet_state = TELNET_STATE_SB;
                } else {
                    /* NOP, IP, AYT, ...: ignored */
                    telnet_state = TELNET_STATE_DATA;
                }
                break;
            case TELNET_STATE_OPTION:
                if (telnet_option_cmd == TELNET_WILL || telnet_option_cmd == TELNET_DO) {
                    monitor_network_telnet_refuse(telnet_option_cmd, c);
                }
                telnet_state = TELNET_STATE_DATA;
                break;
            case TELNET_STATE_SB:
                if (c == TELNET_IAC) {
                    telnet_state = TELNET_STATE_SB_IAC;
                }
                break;
            case TELNET_STATE_SB_IAC:
                telnet_state = (c == TELNET_SE) ? TELNET_STATE_DATA : TELNET_STATE_SB;
                break;
        }
    }

    return out;
}

ssize_t monitor_network_receive(char * buffer, size_t buffer_length)
{
    ssize_t count = 0;

    do {
        if (!connected_socket) {
            break;
        }

        count = vice_network_receive(connected_socket, buffer, buffer_length, 0);

        if (count < 0) {
            log_message(LOG_DEFAULT, "monitor_network_receive(): vice_network_receive() returned -1, breaking connection");
            monitor_network_quit();
        }
    } while (0);

    return count;
}

static int monitor_network_data_available(void)
{
    int available = 0;

    if (connected_socket != NULL) {
        available = vice_network_select_poll_one(connected_socket);
    } else if (listen_socket != NULL) {
        /* we have no connection yet, allow for connection */

        if (vice_network_select_poll_one(listen_socket)) {
            connected_socket = vice_network_accept(listen_socket);
            monitor_network_reset_connection_state();
        }
    }


    return available;
}

/* Read what the client sent into cmd_buffer, without telnet commands.
   Call only when data is available. Returns -1 when the connection was closed
   (and closes it), else the number of text bytes added (0 for a packet that
   held only telnet negotiation). */
static int monitor_network_fill_buffer(void)
{
    size_t space = sizeof cmd_buffer - 1 - (size_t)cmd_bufferpos;
    ssize_t n;
    size_t added;

    if (space == 0) {
        return 0;
    }

    n = monitor_network_receive(cmd_buffer + cmd_bufferpos, space);
    if (n <= 0) {
        monitor_network_quit();
        return -1;
    }

    added = monitor_network_filter_input((unsigned char *)cmd_buffer + cmd_bufferpos, (size_t)n);
    cmd_bufferpos += (int)added;
    cmd_buffer[cmd_bufferpos] = 0;

    return (int)added;
}

void monitor_check_remote(void)
{
    if (monitor_network_data_available()) {
        /* Read it now, so a telnet client is known before the monitor prints
           anything, and telnet negotiation alone does not stop the machine. */
        if (monitor_network_fill_buffer() < 0) {
            return;
        }
        if (cmd_bufferpos > 0) {
            monitor_startup_trap();
        }
    }
}

int monitor_network_line_pending(void)
{
    return connected_socket != NULL
        && (strchr(cmd_buffer, '\n') != NULL
            || strchr(cmd_buffer, '\r') != NULL
            || cmd_bufferpos >= (int)sizeof cmd_buffer - 1);
}

static char * monitor_network_extract_text_command_line(char * pbuffer, int buffer_size, int * pbuffer_pos)
{
    char * cr_start = NULL;
    char * cr_end = NULL;
    char * p = NULL;

    do {
        cr_start = strchr(pbuffer, '\n');
        cr_end = strchr(pbuffer, '\r');

        if (cr_start || cr_end) {
            if (cr_start == NULL) {
                cr_start = cr_end;
            } else if (cr_end == NULL) {
                cr_end = cr_start;
            } else if (cr_end < cr_start) {
                char * cr_temp = cr_end;
                cr_end = cr_start;
                cr_start = cr_temp;
            }

            assert(cr_start != NULL);
            assert(cr_end != NULL);
        }

        if (cr_start) {
            assert(cr_end != NULL);

            *cr_start = 0;
            p = lib_strdup(pbuffer);

            memmove(pbuffer, cr_end + 1, strlen(cr_end + 1));

            *pbuffer_pos -= (int)(((long)strlen(p) + (cr_end - cr_start) + 1));
            pbuffer[*pbuffer_pos] = 0;
            break;
        } else if (*pbuffer_pos >= buffer_size) {
            /* we have a command that is too large:
             * process it anyway, so the sender knows something is wrong
             */
            p = lib_strdup(pbuffer);
            *pbuffer_pos = 0;
            pbuffer[0] = 0;
            break;
        }
    } while (0);

    return p;
}

/* Returns 0 when the connection was closed. Else 1, with *prompt set to the
   next command line, or to NULL when no complete line has arrived yet (the
   part received so far is kept for the next call). */
int monitor_network_get_command_line(char **prompt)
{
    /* commands already received (several lines in one packet) come first */
    *prompt = monitor_network_extract_text_command_line(cmd_buffer, sizeof cmd_buffer - 1, &cmd_bufferpos);
    if (*prompt) {
        return 1;
    }

    if (!monitor_network_data_available()) {
        return 1;
    }

    if (monitor_network_fill_buffer() < 0) {
        *prompt = NULL;
        return 0;
    }

    *prompt = monitor_network_extract_text_command_line(cmd_buffer, sizeof cmd_buffer - 1, &cmd_bufferpos);
    return 1;
}

static int monitor_network_activate(void)
{
    vice_network_socket_address_t * server_addr = NULL;
    int error = 1;

    do {
        if (!monitor_server_address) {
            log_error(LOG_DEFAULT,
                "monitor_network_activate(): monitor server address not set");
            break;
        }

        server_addr = vice_network_address_generate(monitor_server_address, 0);
        if (!server_addr) {
            log_error(LOG_DEFAULT,
                "monitor_network_activate(): could not generate monitor server address?");
            break;
        }

        listen_socket = vice_network_server(server_addr);
        if (!listen_socket) {
            log_error(LOG_DEFAULT,
                "monitor_network_activate(): could not initialize listening socket");
            break;
        }

        error = 0;
    } while (0);

    if (server_addr) {
        vice_network_address_close(server_addr);
    }

    return error;
}

static int monitor_network_deactivate(void)
{
    if (listen_socket) {
        vice_network_socket_close(listen_socket);
        listen_socket = NULL;
    }

    return 0;
}

/* ------------------------------------------------------------------------- */

/*! \internal \brief set the network monitor to the enabled or disabled state

 \param val
   if 0, disable the network monitor; else, enable it.

 \param param
   unused

 \return
   0 on success. else -1.
*/
static int set_monitor_enabled(int value, void *param)
{
    int val = value ? 1 : 0;

    if (!val) {
        if (monitor_enabled) {
            if (monitor_network_deactivate() < 0) {
                return -1;
            }
        }
        monitor_enabled = 0;
        return 0;
    } else {
        if (!monitor_enabled) {
            if (monitor_network_activate() < 0) {
                return -1;
            }
        }

        monitor_enabled = 1;
        return 0;
    }
}

/*! \internal \brief set the network address of the network monitor

 \param name
   pointer to a buffer which holds the network server addresss.

 \param param
   unused

 \return
   0 on success, else -1.
*/
static int set_server_address(const char *name, void *param)
{
    if (monitor_server_address != NULL && name != NULL
        && strcmp(name, monitor_server_address) == 0) {
        return 0;
    }

    if (monitor_enabled) {
        monitor_network_deactivate();
    }
    util_string_set(&monitor_server_address, name);

    if (monitor_enabled) {
        monitor_network_activate();
    }

    return 0;
}

/*! \brief string resources used by the network monitor module */
static const resource_string_t resources_string[] = {
    { "MonitorServerAddress", "ip4://127.0.0.1:6510", RES_EVENT_NO, NULL,
      &monitor_server_address, set_server_address, NULL },
    RESOURCE_STRING_LIST_END
};

/*! \brief integer resources used by the network monitor module */
static const resource_int_t resources_int[] = {
    { "MonitorServer", 0, RES_EVENT_STRICT, (resource_value_t)0,
      &monitor_enabled, set_monitor_enabled, NULL },
    RESOURCE_INT_LIST_END
};

/*! \brief initialize the network monitor resources
 \return
   0 on success, else -1.

 \remark
   Registers the string and the integer resources
*/
int monitor_network_resources_init(void)
{
    if (resources_register_string(resources_string) < 0) {
        return -1;
    }

    return resources_register_int(resources_int);
}

/*! \brief uninitialize the network monitor resources */
void monitor_network_resources_shutdown(void)
{
    monitor_network_deactivate();
    monitor_network_quit();

    lib_free(monitor_server_address);
}

/* ------------------------------------------------------------------------- */

static const cmdline_option_t cmdline_options[] =
{
    { "-remotemonitor", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "MonitorServer", (resource_value_t)1,
      NULL, "Enable remote monitor" },
    { "+remotemonitor", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "MonitorServer", (resource_value_t)0,
      NULL, "Disable remote monitor" },
    { "-remotemonitoraddress", SET_RESOURCE, CMDLINE_ATTRIB_NEED_ARGS,
      NULL, NULL, "MonitorServerAddress", NULL,
      "<Name>", "The local address the remote monitor should bind to" },
    CMDLINE_LIST_END
};

/*! \brief initialize the command-line options'
 \return
   0 on success, else -1.

 \remark
   Registers the command-line options
*/
int monitor_network_cmdline_options_init(void)
{
    return cmdline_register_options(cmdline_options);
}

vice_network_socket_t *monitor_get_connected_socket(void) {
    return connected_socket;
}

int monitor_is_remote(void)
{
    return connected_socket != NULL;
}

ui_jam_action_t monitor_network_ui_jam_dialog(const char *format, ...)
{
    char * txt;

    char init_string[] = "An error occurred: ";
    char end_string[] = "\n";

    va_list ap;
    va_start(ap, format);
    txt = lib_mvsprintf(format, ap);
    va_end(ap);

    monitor_network_transmit(init_string, sizeof init_string - 1);
    monitor_network_transmit(txt, strlen(txt));
    monitor_network_transmit(end_string, sizeof end_string - 1);

    lib_free(txt);

    return UI_JAM_MONITOR;
}

#else

int monitor_network_resources_init(void)
{
    return 0;
}

void monitor_network_resources_shutdown(void)
{
}

int monitor_network_cmdline_options_init(void)
{
    return 0;
}

void monitor_check_remote(void)
{
}

int monitor_network_transmit(const char * buffer, size_t buffer_length)
{
    return 0;
}

int monitor_network_get_command_line(char **prompt)
{
    return 0;
}

int monitor_network_line_pending(void)
{
    return 0;
}

int monitor_is_remote(void)
{
    return 0;
}

ui_jam_action_t monitor_network_ui_jam_dialog(const char *format, ...)
{
    return UI_JAM_HARD_RESET;
}

#endif
