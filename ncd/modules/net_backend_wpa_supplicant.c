/**
 * @file net_backend_wpa_supplicant.c
 * @author Ambroz Bizjak <ambrop7@gmail.com>
 * 
 * @section LICENSE
 * 
 * This file is part of BadVPN.
 * 
 * BadVPN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 * 
 * BadVPN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * 
 * @section DESCRIPTION
 * 
 * Wireless interface module which runs wpa_supplicant.
 * 
 * Note: wpa_supplicant does not monitor the state of rfkill switches and will fail to
 * start if the switch is of when it is started, and will stop working indefinitely if the
 * switch is turned off while it is running. Therefore, you should put a "net.backend.rfkill"
 * statement in front of the wpa_supplicant statement.
 * 
 * Synopsis: net.backend.wpa_supplicant(string ifname, string conf, string exec, list(string) args)
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <misc/cmdline.h>
#include <misc/string_begins_with.h>
#include <system/BSocket.h>
#include <flow/LineBuffer.h>
#include <flowextra/StreamSocketSource.h>
#include <ncd/NCDModule.h>

#include <generated/blog_channel_ncd_net_backend_wpa_supplicant.h>

#define MAX_LINE_LEN 512
#define EVENT_STRING_CONNECTED "CTRL-EVENT-CONNECTED"
#define EVENT_STRING_DISCONNECTED "CTRL-EVENT-DISCONNECTED"

#define ModuleLog(i, ...) NCDModuleInst_Backend_Log((i), BLOG_CURRENT_CHANNEL, __VA_ARGS__)

struct instance {
    NCDModuleInst *i;
    char *ifname;
    char *conf;
    char *exec;
    NCDValue *args;
    int dying;
    int up;
    BProcess process;
    int pipe_fd;
    BSocket pipe_sock;
    FlowErrorDomain pipe_domain;
    StreamSocketSource pipe_source;
    LineBuffer pipe_buffer;
    PacketPassInterface pipe_input;
};

static int build_cmdline (struct instance *o, CmdLine *c);
static int init_pipe (struct instance *o, int pipe_fd);
static void free_pipe (struct instance *o);
static void process_handler (struct instance *o, int normally, uint8_t normally_exit_status);
static void process_pipe_handler_error (struct instance *o, int component, int code);
static void process_pipe_handler_send (struct instance *o, uint8_t *data, int data_len);
static void instance_free (struct instance *o);

int build_cmdline (struct instance *o, CmdLine *c)
{
    if (!CmdLine_Init(c)) {
        goto fail0;
    }
    
    // append exec
    if (!CmdLine_Append(c, o->exec)) {
        goto fail1;
    }
    
    // append user arguments
    NCDValue *arg = NCDValue_ListFirst(o->args);
    while (arg) {
        if (NCDValue_Type(arg) != NCDVALUE_STRING) {
            ModuleLog(o->i, BLOG_ERROR, "wrong type");
            goto fail1;
        }
        
        // append argument
        if (!CmdLine_Append(c, NCDValue_StringValue(arg))) {
            goto fail1;
        }
        
        arg = NCDValue_ListNext(o->args, arg);
    }
    
    // append interface name
    if (!CmdLine_Append(c, "-i") || !CmdLine_Append(c, o->ifname)) {
        goto fail1;
    }
    
    // append config file
    if (!CmdLine_Append(c, "-c") || !CmdLine_Append(c, o->conf)) {
        goto fail1;
    }
    
    // terminate cmdline
    if (!CmdLine_Finish(c)) {
        goto fail1;
    }
    
    return 1;
    
fail1:
    CmdLine_Free(c);
fail0:
    return 0;
}

int init_pipe (struct instance *o, int pipe_fd)
{
    // init socket
    if (BSocket_InitPipe(&o->pipe_sock, o->i->reactor, pipe_fd) < 0) {
        ModuleLog(o->i, BLOG_ERROR, "BSocket_InitPipe failed");
        goto fail0;
    }
    
    // init domain
    FlowErrorDomain_Init(&o->pipe_domain, (FlowErrorDomain_handler)process_pipe_handler_error, o);
    
    // init source
    StreamSocketSource_Init(&o->pipe_source, FlowErrorReporter_Create(&o->pipe_domain, 0), &o->pipe_sock, BReactor_PendingGroup(o->i->reactor));
    
    // init input interface
    PacketPassInterface_Init(&o->pipe_input, MAX_LINE_LEN, (PacketPassInterface_handler_send)process_pipe_handler_send, o, BReactor_PendingGroup(o->i->reactor));
    
    // init buffer
    if (!LineBuffer_Init(&o->pipe_buffer, StreamSocketSource_GetOutput(&o->pipe_source), &o->pipe_input, MAX_LINE_LEN, '\n')) {
        ModuleLog(o->i, BLOG_ERROR, "LineBuffer_Init failed");
        goto fail1;
    }
    
    return 1;
    
fail1:
    PacketPassInterface_Free(&o->pipe_input);
    StreamSocketSource_Free(&o->pipe_source);
    BSocket_Free(&o->pipe_sock);
fail0:
    return 0;
}

void free_pipe (struct instance *o)
{
    // free buffer
    LineBuffer_Free(&o->pipe_buffer);
    
    // free input interface
    PacketPassInterface_Free(&o->pipe_input);
    
    // free source
    StreamSocketSource_Free(&o->pipe_source);
    
    // free socket
    BSocket_Free(&o->pipe_sock);
}

void process_handler (struct instance *o, int normally, uint8_t normally_exit_status)
{
    ModuleLog(o->i, (o->dying ? BLOG_INFO : BLOG_ERROR), "process terminated");
    
    if (!o->dying) {
        NCDModuleInst_Backend_SetError(o->i);
    }
    
    // die
    instance_free(o);
    return;
}

void process_pipe_handler_error (struct instance *o, int component, int code)
{
    ASSERT(o->pipe_fd >= 0)
    
    if (code == STREAMSOCKETSOURCE_ERROR_CLOSED) {
        ModuleLog(o->i, BLOG_INFO, "pipe eof");
    } else {
        ModuleLog(o->i, BLOG_ERROR, "pipe error");
    }
    
    // free pipe reading
    free_pipe(o);
    
    // close pipe read end
    ASSERT_FORCE(close(o->pipe_fd) == 0)
    
    // forget pipe
    o->pipe_fd = -1;
}

void process_pipe_handler_send (struct instance *o, uint8_t *data, int data_len)
{
    ASSERT(o->pipe_fd >= 0)
    ASSERT(data_len > 0)
    
    // accept packet
    PacketPassInterface_Done(&o->pipe_input);
    
    if (o->dying) {
        return;
    }
    
    if (data_begins_with(data, data_len, EVENT_STRING_CONNECTED)) {
        ModuleLog(o->i, BLOG_INFO, "connected event");
        
        if (!o->up) {
            o->up = 1;
            NCDModuleInst_Backend_Event(o->i, NCDMODULE_EVENT_UP);
        }
    }
    else if (data_begins_with(data, data_len, EVENT_STRING_DISCONNECTED)) {
        ModuleLog(o->i, BLOG_INFO, "disconnected event");
        
        if (o->up) {
            o->up = 0;
            NCDModuleInst_Backend_Event(o->i, NCDMODULE_EVENT_DOWN);
        }
    }
}

static void func_new (NCDModuleInst *i)
{
    // allocate instance
    struct instance *o = malloc(sizeof(*o));
    if (!o) {
        ModuleLog(i, BLOG_ERROR, "failed to allocate instance");
        goto fail0;
    }
    NCDModuleInst_Backend_SetUser(i, o);
    
    // init arguments
    o->i = i;
    
    // read arguments
    NCDValue *ifname_arg;
    NCDValue *conf_arg;
    NCDValue *exec_arg;
    NCDValue *args_arg;
    if (!NCDValue_ListRead(o->i->args, 4, &ifname_arg, &conf_arg, &exec_arg, &args_arg)) {
        ModuleLog(o->i, BLOG_ERROR, "wrong arity");
        goto fail1;
    }
    if (NCDValue_Type(ifname_arg) != NCDVALUE_STRING || NCDValue_Type(conf_arg) != NCDVALUE_STRING ||
        NCDValue_Type(exec_arg) != NCDVALUE_STRING || NCDValue_Type(args_arg) != NCDVALUE_LIST) {
        ModuleLog(o->i, BLOG_ERROR, "wrong type");
        goto fail1;
    }
    o->ifname = NCDValue_StringValue(ifname_arg);
    o->conf = NCDValue_StringValue(conf_arg);
    o->exec = NCDValue_StringValue(exec_arg);
    o->args = args_arg;
    
    // set not dying
    o->dying = 0;
    
    // set not up
    o->up = 0;
    
    // create pipe
    int pipefds[2];
    if (pipe(pipefds) < 0) {
        ModuleLog(o->i, BLOG_ERROR, "pipe failed");
        goto fail1;
    }
    
    // init pipe reading
    if (!init_pipe(o, pipefds[0])) {
        goto fail2;
    }
    
    // build process cmdline
    CmdLine c;
    if (!build_cmdline(o, &c)) {
        ModuleLog(o->i, BLOG_ERROR, "failed to build cmdline");
        goto fail3;
    }
    
    // start process
    int fds[] = { pipefds[1], -1 };
    int fds_map[] = { 1 };
    if (!BProcess_InitWithFds(&o->process, o->i->manager, (BProcess_handler)process_handler, o, ((char **)c.arr.v)[0], (char **)c.arr.v, NULL, fds, fds_map)) {
        ModuleLog(o->i, BLOG_ERROR, "BProcess_Init failed");
        goto fail4;
    }
    
    // remember pipe read end
    o->pipe_fd = pipefds[0];
    
    CmdLine_Free(&c);
    ASSERT_FORCE(close(pipefds[1]) == 0)
    
    return;
    
fail4:
    CmdLine_Free(&c);
fail3:
    free_pipe(o);
fail2:
    ASSERT_FORCE(close(pipefds[0]) == 0)
    ASSERT_FORCE(close(pipefds[1]) == 0)
fail1:
    free(o);
fail0:
    NCDModuleInst_Backend_SetError(i);
    NCDModuleInst_Backend_Event(i, NCDMODULE_EVENT_DEAD);
}

void instance_free (struct instance *o)
{
    NCDModuleInst *i = o->i;
    
    // free process
    BProcess_Free(&o->process);
    
    if (o->pipe_fd >= 0) {
        // free pipe reading
        free_pipe(o);
        
        // close pipe read end
        ASSERT_FORCE(close(o->pipe_fd) == 0)
    }
    
    // free instance
    free(o);
    
    NCDModuleInst_Backend_Event(i, NCDMODULE_EVENT_DEAD);
}

static void func_die (void *vo)
{
    struct instance *o = vo;
    ASSERT(!o->dying)
    
    // request termination
    BProcess_Terminate(&o->process);
    
    // remember dying
    o->dying = 1;
}

static const struct NCDModule modules[] = {
    {
        .type = "net.backend.wpa_supplicant",
        .func_new = func_new,
        .func_die = func_die
    }, {
        .type = NULL
    }
};

const struct NCDModuleGroup ncdmodule_net_backend_wpa_supplicant = {
    .modules = modules
};
