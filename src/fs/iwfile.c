/**************************************************************************************************
 *  IOWOW library
 *  Copyright (C) 2012-2015 Softmotions Ltd <info@softmotions.com>
 *
 *  This file is part of IOWOW.
 *  IOWOW is free software; you can redistribute it and/or modify it under the terms of
 *  the GNU Lesser General Public License as published by the Free Software Foundation; either
 *  version 2.1 of the License or any later version. IOWOW is distributed in the hope
 *  that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 *  License for more details.
 *  You should have received a copy of the GNU Lesser General Public License along with IOWOW;
 *  if not, write to the Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 *  Boston, MA 02111-1307 USA.
 *************************************************************************************************/

#include "iwfile.h"
#include "platform/iwp.h"
#include "iwcfg.h"
#include "log/iwlog.h"

#include <fcntl.h>
#include <unistd.h>
#include <limits.h>

typedef struct IWFS_FILE_IMPL {
    HANDLE              fh;         /**< File handle. */
    iwfs_openstatus     ostatus;    /**< File open status. */
    IWFS_FILE_OPTS      opts;       /**< File open options. */
} _IWF;

static iwrc _iwfs_write(struct IWFS_FILE *f, off_t off,
                        const void *buf, size_t siz, size_t *sp) {
    assert(f);
    _IWF *impl = f->impl;
    if (!impl) {
        return IW_ERROR_INVALID_STATE;
    }
    if (!(impl->opts.omode & IWFS_OWRITE)) {
        return IW_ERROR_READONLY;
    }
    return iwp_write(impl->fh, off, buf, siz, sp);
}

static iwrc _iwfs_read(struct IWFS_FILE *f, off_t off,
                       void *buf, size_t siz, size_t *sp) {

    assert(f);
    _IWF *impl = f->impl;
    if (!impl) {
        return IW_ERROR_INVALID_STATE;
    }
    return iwp_read(impl->fh, off, buf, siz, sp);
}

static iwrc _iwfs_close(struct IWFS_FILE *f) {
    assert(f);
    iwrc rc = 0;
    _IWF *impl = f->impl;
    if (!impl) {
        return 0;
    }
    IWFS_FILE_OPTS *opts = &impl->opts;
    if (opts->lock_mode != IWP_NOLOCK) {
        IWRC(iwp_unlock(impl->fh), rc);
    }
    IWRC(iwp_closefh(impl->fh), rc);
    if (opts->path) {
        free((char*) opts->path);
        opts->path = 0;
    }
    free(f->impl);
    f->impl = 0;
    return rc;
}

static iwrc _iwfs_sync(struct IWFS_FILE *f, iwfs_sync_flags flags) {
    assert(f);
    if (!f->impl) {
        return IW_ERROR_INVALID_STATE;
    }
    if (flags & IWFS_FDATASYNC) {
        if (fdatasync(f->impl->fh) == -1) {
            return iwrc_set_errno(IW_ERROR_IO_ERRNO, errno);
        }
    } else if (fsync(f->impl->fh) == -1) {
        return iwrc_set_errno(IW_ERROR_IO_ERRNO, errno);
    }
    return 0;
}

static iwrc _iwfs_state(struct IWFS_FILE *f, IWFS_FILE_STATE* state) {
    assert(f);
    assert(state);
    memset(state, 0, sizeof(*state));
    _IWF *impl = f->impl;
    state->is_open = !!impl;
    if (!state->is_open) {
        return 0;
    }
    state->ostatus = impl->ostatus;
    state->opts = impl->opts;
    state->fh = impl->fh;
    return 0;
}

iwrc iwfs_file_open(IWFS_FILE *f, const IWFS_FILE_OPTS *_opts) {
    assert(f);
    assert(_opts && _opts->path);

    IWFS_FILE_OPTS *opts;
    _IWF *impl;
    IWP_FILE_STAT fstat;
    iwfs_omode omode;
    iwrc rc;
    int mode;

    memset(f, 0, sizeof(*f));
    impl = f->impl = calloc(sizeof(*f->impl), 1);
    if (!impl) {
        return iwrc_set_errno(IW_ERROR_ALLOC, errno);
    }

    f->write = _iwfs_write;
    f->read = _iwfs_read;
    f->close = _iwfs_close;
    f->sync = _iwfs_sync;
    f->state = _iwfs_state;

    impl->opts = *_opts;
    opts = &impl->opts;
    opts->path = strndup(_opts->path, PATH_MAX);
    if (!opts->path) {
        rc = iwrc_set_errno(IW_ERROR_ALLOC, errno);
        goto finish;
    }

    if (!opts->lock_mode) {
        opts->lock_mode = IWFS_DEFAULT_LOCKMODE;
    }
    if (!opts->omode) {
        opts->omode = IWFS_DEFAULT_OMODE;
    }
    if (!opts->filemode) {
        opts->filemode = IWFS_DEFAULT_FILEMODE;
    }
    opts->omode |= IWFS_OREAD;
    if (opts->omode & IWFS_OTRUNC) {
        opts->omode |= IWFS_OWRITE;
        opts->omode |= IWFS_OCREATE;
    }
    if ((opts->omode & IWFS_OCREATE) || (opts->omode & IWFS_OTRUNC)) {
        opts->omode |= IWFS_OWRITE;
    }
    omode = opts->omode;

    if (!(opts->omode & IWFS_OWRITE) && (opts->lock_mode & IWP_WLOCK)) {
        opts->lock_mode &= ~IWP_WLOCK;
    }

    rc = iwp_fstat(opts->path, &fstat);
    if (!rc && !(opts->omode & IWFS_OTRUNC)) {
        impl->ostatus = IWFS_OPEN_EXISTING;
    } else {
        impl->ostatus = IWFS_OPEN_NEW;
    }
    rc = 0;
    mode = O_RDONLY;
    if (omode & IWFS_OWRITE) {
        mode = O_RDWR;
        if (omode & IWFS_OCREATE) mode |= O_CREAT;
        if (omode & IWFS_OTRUNC) mode |= O_TRUNC;
    }
    impl->fh = open(opts->path, mode, opts->filemode);
    if (INVALIDHANDLE(impl->fh)) {
        rc = iwrc_set_errno(IW_ERROR_IO_ERRNO, errno);
        goto finish;
    }
    if (opts->lock_mode != IWP_NOLOCK) {
        rc = iwp_flock(impl->fh, opts->lock_mode);
        if (rc) {
            goto finish;
        }
    }

finish:
    if (rc) {
        impl->ostatus = IWFS_OPEN_FAIL;
        if (opts->path) {
            free((char*) opts->path);
        }
    }
    return rc;
}

iwrc iwfs_file_init(void) {
    return 0;
}