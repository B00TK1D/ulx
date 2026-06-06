/* work.cpp -- main work driver

   This file is part of the UPX executable compressor.

   Copyright (C) Markus Franz Xaver Johannes Oberhumer
   Copyright (C) Laszlo Molnar
   All Rights Reserved.

   UPX and the UCL library are free software; you can redistribute them
   and/or modify them under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; see the file COPYING.
   If not, write to the Free Software Foundation, Inc.,
   59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

   Markus F.X.J. Oberhumer              Laszlo Molnar
   <markus@oberhumer.com>               <ezerotven+github@gmail.com>
 */

// This file implements the central loop, and it uses class PackMaster to
// dispatch. PackMaster by itself will instantiate a concrete subclass of
// class PackerBase which then does the actual work; search for "HERE".
// And see p_com.cpp for a simple executable format.
//
// Additionally this file also has the burden to deal with all those pesky
// low-level file handling issues.

#define WANT_WINDOWS_LEAN_H 1 // _get_osfhandle, GetFileTime, SetFileTime
#include "util/system_headers.h"
#if USE_UTIMENSAT
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif
#include "conf.h"
#include "file.h"
#include "packmast.h"
#include "ui.h"
#include "util/membuffer.h"
#include "compress/compress.h"

#if USE_UTIMENSAT && defined(AT_FDCWD)
#elif defined(_WIN32) || defined(__CYGWIN__)
#define USE_SETFILETIME 1
#elif (ACC_OS_DOS32) && defined(__DJGPP__)
#define USE_FTIME 1
#elif ((ACC_OS_WIN32 || ACC_OS_WIN64) && (ACC_CC_INTELC || ACC_CC_MSC))
#define USE__FUTIME 1
#elif HAVE_UTIME
#define USE_UTIME 1
#endif

#if !defined(SH_DENYRW)
#define SH_DENYRW (-1)
#endif
#if !defined(SH_DENYWR)
#define SH_DENYWR (-1)
#endif

/*************************************************************************
// file util
**************************************************************************/

namespace {

struct XStat final {
    struct stat st;
#if USE_SETFILETIME
    FILETIME ft_atime;
    FILETIME ft_mtime;
#elif USE_FTIME
    struct ftime ft_ftime;
#endif
};

// ignore errors in some cases and silence __attribute__((__warn_unused_result__))
#define IGNORE_ERROR(var) ACC_UNUSED(var)

enum OpenMode { RO_MUST_EXIST, WO_MUST_EXIST_TRUNCATE, WO_MUST_CREATE, WO_CREATE_OR_TRUNCATE };

static constexpr int get_open_flags(OpenMode om) noexcept {
    constexpr int wo_flags = O_WRONLY | O_BINARY;
    if (om == WO_MUST_EXIST_TRUNCATE)
        return wo_flags | O_TRUNC; // will cause an error if file does not exist
    if (om == WO_MUST_CREATE)
        return wo_flags | O_CREAT | O_EXCL; // will cause an error if file already exists
    if (om == WO_CREATE_OR_TRUNCATE)
        return wo_flags | O_CREAT | O_TRUNC; // create if not exists, otherwise truncate
    // RO_MUST_EXIST
    return O_RDONLY | O_BINARY; // will cause an error if file does not exist
}

// set file time of an open file
static noinline void set_fd_timestamp(int fd, const XStat *xst) noexcept {
#if USE_SETFILETIME
    BOOL r = SetFileTime((HANDLE) _get_osfhandle(fd), nullptr, &xst->ft_atime, &xst->ft_mtime);
    IGNORE_ERROR(r);
#elif USE_FTIME
    struct ftime ft_ftime = xst->ft_ftime; // djgpp2 libc bug/feature: not const, so use a copy
    int r = setftime(fd, &ft_ftime);
    IGNORE_ERROR(r);
#elif USE__FUTIME
    struct _utimbuf u = {};
    u.actime = xst->st.st_atime;
    u.modtime = xst->st.st_mtime;
    int r = _futime(fd, &u);
    IGNORE_ERROR(r);
#endif
    // maybe unused
    UNUSED(fd);
    UNUSED(xst);
}

static noinline void copy_file_contents(const char *iname, const char *oname, OpenMode om,
                                        const XStat *oname_timestamp) may_throw {
    InputFile fi;
    fi.sopen(iname, get_open_flags(RO_MUST_EXIST), SH_DENYWR);
    fi.seek(0, SEEK_SET);
    int flags = get_open_flags(om);
    int shmode = SH_DENYWR;
    int omode = 0600; // affected by umask; ignored unless O_CREAT
    OutputFile fo;
    fo.sopen(oname, flags, shmode, omode);
    fo.seek(0, SEEK_SET);
    MemBuffer buf(256 * 1024 * 1024);
    for (;;) {
        size_t bytes = fi.read(buf, buf.getSize());
        if (bytes == 0)
            break;
        fo.write(buf, bytes);
    }
    if (oname_timestamp != nullptr)
        set_fd_timestamp(fo.getFd(), oname_timestamp);
    fi.closex();
    fo.closex();
}

static noinline void copy_file_attributes(const XStat *xst, const char *oname, bool preserve_mode,
                                          bool preserve_ownership,
                                          bool preserve_timestamp) noexcept {
    const struct stat *const st = &xst->st;
    // copy time stamp
    if (preserve_timestamp) {
#if USE_UTIMENSAT && defined(AT_FDCWD)
        struct timespec times[2] = {};
#if HAVE_STRUCT_STAT_ST_MTIMESPEC_TV_NSEC
        // macOS
        times[0] = st->st_atimespec;
        times[1] = st->st_mtimespec;
#else
        // POSIX.1-2008
        times[0] = st->st_atim;
        times[1] = st->st_mtim;
#endif
        int r = utimensat(AT_FDCWD, oname, &times[0], 0);
        IGNORE_ERROR(r);
#elif USE_UTIME
        struct utimbuf u = {};
        u.actime = st->st_atime;
        u.modtime = st->st_mtime;
        int r = utime(oname, &u);
        IGNORE_ERROR(r);
#endif
    }
#if HAVE_CHOWN
    // copy the group ownership
    if (preserve_ownership) {
        int r = chown(oname, -1, st->st_gid);
        IGNORE_ERROR(r);
    }
#endif
#if HAVE_CHMOD
    // copy permissions
    if (preserve_mode) {
        int r = chmod(oname, st->st_mode);
        IGNORE_ERROR(r);
    }
#endif
#if HAVE_CHOWN
    // copy the user ownership
    if (preserve_ownership) {
        int r = chown(oname, st->st_uid, -1);
        IGNORE_ERROR(r);
    }
#endif
    // maybe unused
    UNUSED(xst);
    UNUSED(st);
    UNUSED(oname);
    UNUSED(preserve_mode);
    UNUSED(preserve_ownership);
    UNUSED(preserve_timestamp);
}

// ULX5 dual-binary constants
#define ULX_MAGIC       "ULX1"
#define ULX_FOOTER_SIZE 24
#define ULX_BINFO_SIZE  12
#define ULX_SCAN_TAIL   128

static bool validate_pack_header(const byte *p, int blen, unsigned *u_file_size) noexcept {
    if (blen < 32)
        return false;
    if (get_le32(p) != UPX_MAGIC_LE32)
        return false;
    const unsigned version = p[4];
    const unsigned format = p[5];
    if (version < 8 || version >= 0xff)
        return false;
    if (!((format >= 1 && format <= UPX_F_CPM86_CMD) || (format >= 129 && format <= UPX_F_DYLIB_PPC64)))
        return false;
    const unsigned u_len = get_le32(p + 16);
    const unsigned c_len = get_le32(p + 20);
    const unsigned ufs = get_le32(p + 24);
    if (c_len == 0 || u_len == 0 || c_len > u_len || u_len > ufs)
        return false;
    if (u_file_size)
        *u_file_size = ufs;
    return true;
}

static upx_off_t find_pack_header_offset(const byte *data, upx_off_t file_size) noexcept {
    if (file_size < 64)
        return -1;
    const unsigned scan = (unsigned) UPX_MIN((upx_off_t) ULX_SCAN_TAIL, file_size);
    const byte *tail = data + file_size - scan;
    for (int rel = (int) scan - 4; rel >= 0; --rel) {
        unsigned ufs = 0;
        if (!validate_pack_header(tail + rel, scan - rel, &ufs))
            continue;
        return file_size - scan + rel;
    }
    return -1;
}

static void insert_true_payload(const char *oname, const byte *footer, unsigned footer_size,
                                const byte *binfo, const byte *compressed,
                                unsigned c_len) may_throw {
    InputFile fi;
    fi.sopen(oname, get_open_flags(RO_MUST_EXIST), SH_DENYWR);
    const upx_off_t file_size = fi.st_size();
    if (file_size <= 0 || !mem_size_valid_bytes(file_size))
        throwIOException("bad packed output size");

    MemBuffer buf((size_t) file_size);
    fi.readx(buf, (size_t) file_size);
    fi.closex();

    const upx_off_t ph_off = find_pack_header_offset(buf, file_size);
    if (ph_off < 0)
        throwIOException("could not locate UPX pack header");

    unsigned u_file_size = 0;
    if (!validate_pack_header(buf + ph_off, (int) (file_size - ph_off), &u_file_size))
        throwIOException("invalid UPX pack header");

    const unsigned payload_size = footer_size + ULX_BINFO_SIZE + c_len;
    const upx_off_t suffix_size = file_size - ph_off;
    const upx_off_t new_size = ph_off + payload_size + suffix_size;
    if (new_size > (upx_off_t) u_file_size)
        throwIOException("TRUE payload too large for UPX file size budget");

    char tname[ACC_FN_PATH_MAX + 1];
    if (!maketempname(tname, sizeof(tname), oname, ".ulx_tmp"))
        throwIOException("could not create temp file");
    OutputFile fo;
    fo.sopen(tname, get_open_flags(WO_MUST_CREATE), SH_DENYWR, 0600);
    fo.write(buf, (size_t) ph_off);
    fo.write(footer, footer_size);
    fo.write(binfo, ULX_BINFO_SIZE);
    fo.write(compressed, c_len);
    fo.write(buf + ph_off, (size_t) suffix_size);
    fo.closex();

    FileBase::rename(tname, oname);
}

} // namespace

/*************************************************************************
// process one file
**************************************************************************/

void do_one_file(const char *const iname, char *const oname) may_throw {
    oname[0] = 0; // make empty

    // check iname stat
    XStat xst = {};
    struct stat &st = xst.st;
#if HAVE_LSTAT
    int rr = lstat(iname, &st);
#else
    int rr = stat(iname, &st);
#endif
    if (rr != 0) {
        if (errno == ENOENT)
            throw FileNotFoundException(iname, errno);
        else
            throwIOException(iname, errno);
    }
#if HAVE_LSTAT
    if (S_ISLNK(st.st_mode))
        throwIOException("is a symlink -- skipped");
#endif
    if (S_ISDIR(st.st_mode))
        throwIOException("is a directory -- skipped");
    if (!(S_ISREG(st.st_mode)))
        throwIOException("not a regular file -- skipped");
#if defined(__unix__)
    // no special bits may be set
    if ((st.st_mode & (S_ISUID | S_ISGID | S_ISVTX)) != 0)
        throwIOException("file has special permissions -- skipped");
#endif
    if (st.st_size <= 0)
        throwIOException("empty file -- skipped");
    if (st.st_size < 512)
        throwIOException("file is too small -- skipped");
    if (!mem_size_valid_bytes(st.st_size))
        throwIOException("file is too large -- skipped");
    if ((st.st_mode & S_IWUSR) == 0) {
        bool skip = true;
        if (opt->output_name)
            skip = false;
        else if (opt->to_stdout)
            skip = false;
        else if (opt->backup)
            skip = false;
        if (skip)
            throwIOException("file is write protected -- skipped");
    }

    // open input file
    InputFile fi;
    fi.sopen(iname, get_open_flags(RO_MUST_EXIST), SH_DENYWR);

    if (opt->preserve_timestamp) {
#if USE_SETFILETIME
        if (GetFileTime((HANDLE) _get_osfhandle(fi.getFd()), nullptr, &xst.ft_atime,
                        &xst.ft_mtime) == 0)
            throwIOException("cannot determine file timestamp");
#elif USE_FTIME
        if (getftime(fi.getFd(), &xst.ft_ftime) != 0)
            throwIOException("cannot determine file timestamp");
#endif
    }

    // open output file
    // NOTE: only use "preserve_link" if you really need it, e.g. it can fail
    //   with ETXTBSY and other unexpected errors; renaming files is much safer
    OutputFile fo;
    bool preserve_link = opt->preserve_link;
    bool copy_timestamp_only = false;
    if (opt->cmd == CMD_COMPRESS || opt->cmd == CMD_DECOMPRESS) {
        if (opt->to_stdout) {
            preserve_link = false; // not needed
            if (!fo.openStdout(1, opt->force ? true : false))
                throwIOException("data not written to a terminal; Use '-f' to force.");
        } else {
            char tname[ACC_FN_PATH_MAX + 1];
            if (opt->output_name) {
                strcpy(tname, opt->output_name);
                if ((opt->force_overwrite || opt->force >= 2) && !preserve_link)
                    (void) FileBase::unlink_noexcept(tname); // IGNORE_ERROR
            } else {
                if (st.st_nlink < 2)
                    preserve_link = false; // not needed
                if (!maketempname(tname, sizeof(tname), iname, ".upx"))
                    throwIOException("could not create a temporary file name");
            }
            int flags = get_open_flags(WO_MUST_CREATE); // don't overwrite files by default
            if (opt->output_name && preserve_link) {
                flags = get_open_flags(WO_CREATE_OR_TRUNCATE);
#if HAVE_LSTAT
                struct stat ost = {};
                int r = lstat(tname, &ost);
                if (r == 0 && S_ISREG(ost.st_mode)) {
                    preserve_link = ost.st_nlink >= 2;
                } else if (r == 0 && S_ISLNK(ost.st_mode)) {
                    // output_name is a symlink (valid or dangling)
                    (void) FileBase::unlink_noexcept(tname); // IGNORE_ERROR
                    preserve_link = false;                   // not needed
                } else {
                    preserve_link = false; // not needed
                }
#endif
                if (preserve_link) {
                    flags = get_open_flags(WO_MUST_EXIST_TRUNCATE);
                    copy_timestamp_only = true;
                }
            } else if (opt->force_overwrite || opt->force) {
                flags = get_open_flags(WO_CREATE_OR_TRUNCATE);
            }
            int shmode = SH_DENYWR;
#if (ACC_ARCH_M68K && ACC_OS_TOS && ACC_CC_GNUC) && defined(__MINT__)
            // TODO later: check current mintlib if this hack is still needed
            flags |= O_TRUNC;
            shmode = O_DENYRW;
#endif
            // cannot rely on open() because of umask
            // int omode = st.st_mode | 0600;
            int omode = opt->preserve_mode ? 0600 : 0666; // affected by umask; only for O_CREAT
            fo.sopen(tname, flags, shmode, omode);
            // open succeeded - now set oname[]
            strcpy(oname, tname);
        }
    }

    // TRUE binary handling for dual-binary mode (compress)
    struct {
        bool is_valid = false;
        MemBuffer compressed;
        unsigned c_len = 0;
        unsigned u_len = 0;
        unsigned entry = 0;
        int method = M_NRV2B_LE32;
    } ulx_true;

    if (opt->true_name && opt->cmd == CMD_COMPRESS) {
        InputFile true_fi;
        true_fi.sopen(opt->true_name, get_open_flags(RO_MUST_EXIST), SH_DENYWR);
        ulx_true.u_len = (unsigned) true_fi.st_size();
        MemBuffer true_buf(ulx_true.u_len);
        size_t nread = true_fi.read(true_buf, ulx_true.u_len);
        if (nread != (size_t) ulx_true.u_len)
            throwIOException("failed to read TRUE binary");
        true_fi.closex();

        if (ulx_true.u_len >= 64) {
            unsigned char *hdr = (unsigned char *) (void *) true_buf;
            if (hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F') {
                if (hdr[4] == 2) {
                    ulx_true.entry = (unsigned) (
                        (unsigned long long) hdr[24] | ((unsigned long long) hdr[25] << 8) |
                        ((unsigned long long) hdr[26] << 16) | ((unsigned long long) hdr[27] << 24) |
                        ((unsigned long long) hdr[28] << 32) | ((unsigned long long) hdr[29] << 40) |
                        ((unsigned long long) hdr[30] << 48) | ((unsigned long long) hdr[31] << 56));
                } else if (hdr[4] == 1) {
                    ulx_true.entry = (unsigned) (hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) |
                                                 (hdr[27] << 24));
                }
            }
        }

        unsigned dst_len = MemBuffer::getSizeForCompression(ulx_true.u_len);
        ulx_true.compressed.alloc(dst_len);
        int method = opt->method > 0 ? opt->method : M_NRV2B_LE32;
        int level = opt->level > 0 ? opt->level : 10;
        upx_compress_result_t cresult;
        int r = upx_compress(true_buf, ulx_true.u_len, ulx_true.compressed, &dst_len, nullptr,
                             method, level, nullptr, &cresult);
        if (r != UPX_E_OK)
            throwInternalError("compression of TRUE binary failed");
        ulx_true.is_valid = true;
        ulx_true.c_len = dst_len;
        ulx_true.method = method;
    }

    // handle command - actual work starts HERE
    {
        PackMaster pm(&fi, opt);
        if (opt->cmd == CMD_COMPRESS)
            pm.pack(&fo);
        else if (opt->cmd == CMD_DECOMPRESS)
            pm.unpack(&fo);
        else if (opt->cmd == CMD_TEST)
            pm.test();
        else if (opt->cmd == CMD_LIST)
            pm.list();
        else if (opt->cmd == CMD_FILEINFO)
            pm.fileInfo();
        else
            throwInternalError("invalid command");
    }

    // Insert hidden TRUE payload before pack header (stock upx -d still unpacks MASK)
    if (ulx_true.is_valid) {
        unsigned char footer[ULX_FOOTER_SIZE] = {};
        memcpy(footer, ULX_MAGIC, 4);
        footer[4] = (unsigned char) (ulx_true.c_len >> 0);
        footer[5] = (unsigned char) (ulx_true.c_len >> 8);
        footer[6] = (unsigned char) (ulx_true.c_len >> 16);
        footer[7] = (unsigned char) (ulx_true.c_len >> 24);
        footer[8] = (unsigned char) (ulx_true.u_len >> 0);
        footer[9] = (unsigned char) (ulx_true.u_len >> 8);
        footer[10] = (unsigned char) (ulx_true.u_len >> 16);
        footer[11] = (unsigned char) (ulx_true.u_len >> 24);
        footer[12] = (unsigned char) (ulx_true.entry >> 0);
        footer[13] = (unsigned char) (ulx_true.entry >> 8);
        footer[14] = (unsigned char) (ulx_true.entry >> 16);
        footer[15] = (unsigned char) (ulx_true.entry >> 24);
        footer[16] = (unsigned char) ulx_true.method;

        unsigned char binfo[ULX_BINFO_SIZE] = {};
        binfo[0] = (unsigned char) (ulx_true.u_len >> 0);
        binfo[1] = (unsigned char) (ulx_true.u_len >> 8);
        binfo[2] = (unsigned char) (ulx_true.u_len >> 16);
        binfo[3] = (unsigned char) (ulx_true.u_len >> 24);
        binfo[4] = (unsigned char) (ulx_true.c_len >> 0);
        binfo[5] = (unsigned char) (ulx_true.c_len >> 8);
        binfo[6] = (unsigned char) (ulx_true.c_len >> 16);
        binfo[7] = (unsigned char) (ulx_true.c_len >> 24);
        binfo[8] = (unsigned char) ulx_true.method;

        const char *outpath = opt->output_name ? opt->output_name : oname;
        if (!outpath[0])
            throwIOException("no output file for dual-binary pack");
        insert_true_payload(outpath, footer, sizeof(footer), binfo, ulx_true.compressed,
                            ulx_true.c_len);
    }

    // copy time stamp
    if (oname[0] && opt->preserve_timestamp && fo.isOpen())
        set_fd_timestamp(fo.getFd(), &xst);

    // close files
    fi.closex();
    fo.closex();

    // rename or copy files
    if (oname[0] && !opt->output_name) {
        // both iname and oname do exist; rename oname to iname
        if (opt->backup) {
            char bakname[ACC_FN_PATH_MAX + 1];
            if (!makebakname(bakname, sizeof(bakname), iname))
                throwIOException("could not create a backup file name");
            if (preserve_link) {
                copy_file_contents(iname, bakname, WO_MUST_CREATE, &xst);
                copy_file_attributes(&xst, bakname, true, true, true);
                const XStat *xstamp = opt->preserve_timestamp ? &xst : nullptr;
                copy_file_contents(oname, iname, WO_MUST_EXIST_TRUNCATE, xstamp);
                FileBase::unlink(oname);
                copy_timestamp_only = true;
            } else {
                FileBase::rename(iname, bakname);
                FileBase::rename(oname, iname);
            }
        } else if (preserve_link) {
            const XStat *xstamp = opt->preserve_timestamp ? &xst : nullptr;
            copy_file_contents(oname, iname, WO_MUST_EXIST_TRUNCATE, xstamp);
            FileBase::unlink(oname);
            copy_timestamp_only = true;
        } else {
            FileBase::unlink(iname);
            FileBase::rename(oname, iname);
        }
        // now iname is the new packed/unpacked file and oname does not exist any longer
    }

    // copy file attributes
    if (oname[0]) {
        oname[0] = 0; // done with oname
        const char *name = opt->output_name ? opt->output_name : iname;
        if (copy_timestamp_only)
            copy_file_attributes(&xst, name, false, false, opt->preserve_timestamp);
        else
            copy_file_attributes(&xst, name, opt->preserve_mode, opt->preserve_ownership,
                                 opt->preserve_timestamp);
    }

    UiPacker::uiConfirmUpdate();
}

/*************************************************************************
// process all files from the commandline
**************************************************************************/

static noinline void unlink_ofile(char *oname) noexcept {
    if (oname && oname[0]) {
        (void) FileBase::unlink_noexcept(oname); // IGNORE_ERROR
        oname[0] = 0;                            // done with oname
    }
}

int do_files(int i, int argc, char *argv[]) may_throw {
    upx_compiler_sanity_check();
    if (opt->verbose >= 1) {
        show_header();
        UiPacker::uiHeader();
    }

    if (opt->cmd == CMD_COMPRESS && opt->true_name && opt->mask_name && opt->output_name) {
        infoHeader();
        char oname[ACC_FN_PATH_MAX + 1];
        oname[0] = 0;
        try {
            do_one_file(opt->mask_name, oname);
        } catch (const Exception &e) {
            unlink_ofile(oname);
            if (opt->verbose >= 1 || (opt->verbose >= 0 && !e.isWarning()))
                printErr(opt->mask_name, e);
            main_set_exit_code(e.isWarning() ? EXIT_WARN : EXIT_ERROR);
        } catch (const Error &e) {
            unlink_ofile(oname);
            printErr(opt->mask_name, e);
            main_set_exit_code(EXIT_ERROR);
            return -1;
        } catch (std::bad_alloc &) {
            unlink_ofile(oname);
            printErr(opt->mask_name, "out of memory");
            main_set_exit_code(EXIT_ERROR);
            return -1;
        } catch (const std::exception &e) {
            unlink_ofile(oname);
            printUnhandledException(opt->mask_name, &e);
            main_set_exit_code(EXIT_ERROR);
            return -1;
        } catch (...) {
            unlink_ofile(oname);
            printUnhandledException(opt->mask_name, nullptr);
            main_set_exit_code(EXIT_ERROR);
            return -1;
        }
        UiPacker::uiPackTotal();
        return 0;
    }

    const int end = argc;
    for (; i < end; i++) {
        infoHeader();

        const char *const iname = argv[i];
        char oname[ACC_FN_PATH_MAX + 1];
        oname[0] = 0;

        try {
            do_one_file(iname, oname);
        } catch (const Exception &e) {
            unlink_ofile(oname);
            if (opt->verbose >= 1 || (opt->verbose >= 0 && !e.isWarning()))
                printErr(iname, e);
            main_set_exit_code(e.isWarning() ? EXIT_WARN : EXIT_ERROR);
            // this is not fatal, continue processing more files
        } catch (const Error &e) {
            unlink_ofile(oname);
            printErr(iname, e);
            main_set_exit_code(EXIT_ERROR);
            return -1; // fatal error
        } catch (std::bad_alloc *e) {
            unlink_ofile(oname);
            printErr(iname, "out of memory");
            UNUSED(e);
            // delete e;
            main_set_exit_code(EXIT_ERROR);
            return -1; // fatal error
        } catch (const std::bad_alloc &) {
            unlink_ofile(oname);
            printErr(iname, "out of memory");
            main_set_exit_code(EXIT_ERROR);
            return -1; // fatal error
        } catch (std::exception *e) {
            unlink_ofile(oname);
            printUnhandledException(iname, e);
            // delete e;
            main_set_exit_code(EXIT_ERROR);
            return -1; // fatal error
        } catch (const std::exception &e) {
            unlink_ofile(oname);
            printUnhandledException(iname, &e);
            main_set_exit_code(EXIT_ERROR);
            return -1; // fatal error
        } catch (...) {
            unlink_ofile(oname);
            printUnhandledException(iname, nullptr);
            main_set_exit_code(EXIT_ERROR);
            return -1; // fatal error
        }
    }

    if (opt->cmd == CMD_COMPRESS)
        UiPacker::uiPackTotal();
    else if (opt->cmd == CMD_DECOMPRESS)
        UiPacker::uiUnpackTotal();
    else if (opt->cmd == CMD_LIST)
        UiPacker::uiListTotal();
    else if (opt->cmd == CMD_TEST)
        UiPacker::uiTestTotal();
    else if (opt->cmd == CMD_FILEINFO)
        UiPacker::uiFileInfoTotal();
    return 0;
}

/* vim:set ts=4 sw=4 et: */
