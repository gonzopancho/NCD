/**
 * @file concat.c
 * @author Ambroz Bizjak <ambrop7@gmail.com>
 * 
 * @section LICENSE
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the author nor the
 *    names of its contributors may be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 * @section DESCRIPTION
 * 
 * String concatenation module.
 * 
 * Synopsis: concat(string elem1, ..., string elemN)
 * Variables:
 *   string (empty) - elem1, ..., elemN concatenated
 */

#include <stdlib.h>

#include <misc/expstring.h>
#include <ncd/NCDModule.h>

#include <generated/blog_channel_ncd_concat.h>

#define ModuleLog(i, ...) NCDModuleInst_Backend_Log((i), BLOG_CURRENT_CHANNEL, __VA_ARGS__)

struct instance {
    NCDModuleInst *i;
    char *string;
};

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
    
    // init string
    ExpString s;
    if (!ExpString_Init(&s)) {
        ModuleLog(i, BLOG_ERROR, "ExpString_Init failed");
        goto fail1;
    }
    
    // append arguments
    NCDValue *arg = NCDValue_ListFirst(o->i->args);
    while (arg) {
        if (NCDValue_Type(arg) != NCDVALUE_STRING) {
            ModuleLog(i, BLOG_ERROR, "wrong type");
            goto fail2;
        }
        
        if (!ExpString_Append(&s, NCDValue_StringValue(arg))) {
            ModuleLog(i, BLOG_ERROR, "ExpString_Append failed");
            goto fail2;
        }
        
        arg = NCDValue_ListNext(o->i->args, arg);
    }
    
    // set string
    o->string = ExpString_Get(&s);
    
    // signal up
    NCDModuleInst_Backend_Up(o->i);
    
    return;
    
fail2:
    ExpString_Free(&s);
fail1:
    free(o);
fail0:
    NCDModuleInst_Backend_SetError(i);
    NCDModuleInst_Backend_Dead(i);
}

static void func_die (void *vo)
{
    struct instance *o = vo;
    NCDModuleInst *i = o->i;
    
    // free string
    free(o->string);
    
    // free instance
    free(o);
    
    NCDModuleInst_Backend_Dead(i);
}

static int func_getvar (void *vo, const char *name, NCDValue *out)
{
    struct instance *o = vo;
    
    if (!strcmp(name, "")) {
        if (!NCDValue_InitString(out, o->string)) {
            ModuleLog(o->i, BLOG_ERROR, "NCDValue_InitCopy failed");
            return 0;
        }
        
        return 1;
    }
    
    return 0;
}

static const struct NCDModule modules[] = {
    {
        .type = "concat",
        .func_new = func_new,
        .func_die = func_die,
        .func_getvar = func_getvar
    }, {
        .type = NULL
    }
};

const struct NCDModuleGroup ncdmodule_concat = {
    .modules = modules
};
