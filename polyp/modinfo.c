/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <ltdl.h>
#include <assert.h>

#include "xmalloc.h"
#include "util.h"
#include "modinfo.h"
#include "log.h"

#define PA_SYMBOL_AUTHOR "pa__get_author"
#define PA_SYMBOL_DESCRIPTION "pa__get_description"
#define PA_SYMBOL_USAGE "pa__get_usage"
#define PA_SYMBOL_VERSION "pa__get_version"

struct pa_modinfo *pa_modinfo_get_by_handle(lt_dlhandle dl) {
    struct pa_modinfo *i;
    const char* (*func)(void);
    assert(dl);

    i = pa_xmalloc0(sizeof(struct pa_modinfo));

    if ((func = (const char* (*)(void)) lt_dlsym(dl, PA_SYMBOL_AUTHOR)))
        i->author = pa_xstrdup(func());

    if ((func = (const char* (*)(void)) lt_dlsym(dl, PA_SYMBOL_DESCRIPTION)))
        i->description = pa_xstrdup(func());

    if ((func = (const char* (*)(void)) lt_dlsym(dl, PA_SYMBOL_USAGE)))
        i->usage = pa_xstrdup(func());

    if ((func = (const char* (*)(void)) lt_dlsym(dl, PA_SYMBOL_VERSION)))
        i->version = pa_xstrdup(func());

    return i;
}

struct pa_modinfo *pa_modinfo_get_by_name(const char *name) {
    lt_dlhandle dl;
    struct pa_modinfo *i;
    assert(name);

    if (!(dl = lt_dlopenext(name))) {
        pa_log(__FILE__": Failed to open module \"%s\": %s\n", name, lt_dlerror());
        return NULL;
    }

    i = pa_modinfo_get_by_handle(dl);
    lt_dlclose(dl);

    return i;
}

void pa_modinfo_free(struct pa_modinfo *i) {
    assert(i);
    pa_xfree(i->author);
    pa_xfree(i->description);
    pa_xfree(i->usage);
    pa_xfree(i->version);
    pa_xfree(i);
}
