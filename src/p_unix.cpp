/* p_unix.cpp --

   This file is part of the UPX executable compressor.

   Copyright (C) 1996-2003 Markus Franz Xaver Johannes Oberhumer
   Copyright (C) 1996-2003 Laszlo Molnar
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

   Markus F.X.J. Oberhumer   Laszlo Molnar
   markus@oberhumer.com      ml1050@cdata.tvnet.hu
 */


#include "conf.h"

#include "file.h"
#include "filter.h"
#include "packer.h"
#include "p_unix.h"
#include "p_elf.h"

// do not change
#define BLOCKSIZE       (512*1024)


/*************************************************************************
//
**************************************************************************/

PackUnix::PackUnix(InputFile *f) :
    super(f), exetype(0), blocksize(0), overlay_offset(0), lsize(0)
{
    COMPILE_TIME_ASSERT(sizeof(Elf_LE32_Ehdr) == 52);
    COMPILE_TIME_ASSERT(sizeof(Elf_LE32_Phdr) == 32);
    COMPILE_TIME_ASSERT(sizeof(b_info) == 12);
    COMPILE_TIME_ASSERT(sizeof(l_info) == 12);
    COMPILE_TIME_ASSERT(sizeof(p_info) == 12);
}


// common part of canPack(), enhanced by subclasses
bool PackUnix::canPack()
{
    if (exetype == 0)
        return false;

#if defined(__unix__)
    // must be executable by owner
    if ((fi->st.st_mode & S_IXUSR) == 0)
        throwCantPack("file not executable; try `chmod +x'");
#endif
    if (file_size < 4096)
        throwCantPack("file is too small");

    // info: currently the header is 36 (32+4) bytes before EOF
    unsigned char buf[256];
    fi->seek(-(off_t)sizeof(buf), SEEK_END);
    fi->readx(buf, sizeof(buf));
    checkAlreadyPacked(buf, sizeof(buf));

    return true;
}


void PackUnix::writePackHeader(OutputFile *fo)
{
    unsigned char buf[32];
    memset(buf, 0, sizeof(buf));

    const int hsize = ph.getPackHeaderSize();
    assert((unsigned)hsize <= sizeof(buf));

    // note: magic constants are always le32
    set_le32(buf+0, UPX_MAGIC_LE32);
    set_le32(buf+4, UPX_MAGIC2_LE32);

    checkPatch(NULL, 0, 0, 0);  // reset
    patchPackHeader(buf, hsize);
    checkPatch(NULL, 0, 0, 0);  // reset

    fo->write(buf, hsize);
}


/*************************************************************************
// Generic Unix pack(). Subclasses must provide patchLoader().
//
// A typical compressed Unix executable looks like this:
//   - loader stub
//   - 12 bytes header info
//   - the compressed blocks, each with a 8 byte header for block sizes
//   - 4 bytes block end marker (uncompressed size 0)
//   - 32 bytes UPX packheader
//   - 4 bytes overlay offset (needed for decompression)
**************************************************************************/

// see note below and Packer::compress()
bool PackUnix::checkCompressionRatio(unsigned, unsigned) const
{
    return true;
}

void PackUnix::pack1(OutputFile * /*fo*/, Filter & /*ft*/)
{
    // derived class usually provides this
}

int
PackUnix::getStrategy(Filter &/*ft*/)
{
    // Called just before reading and compressing each block.
    // Might want to adjust blocksize, etc.

    // If user specified the filter, then use it (-2==strategy).
    // Else try the first two filters, and pick the better (2==strategy).
    return ((opt->filter > 0) ? -2 : 2);
}

void PackUnix::pack2(OutputFile *fo, Filter &ft)
{
    // compress blocks
    unsigned total_in = 0;
    unsigned total_out = 0;

// FIXME: ui_total_passes is not correct with multiple blocks...
//    ui_total_passes = (file_size + blocksize - 1) / blocksize;
//    if (ui_total_passes == 1)
//        ui_total_passes = 0;

    unsigned remaining = file_size;
    while (remaining > 0)
    {
        // FIXME: disable filters if we have more than one block.
        // FIXME: There is only 1 un-filter in the stub [as of 2002-11-10].
        // So the next block really has no choice!
        // This merely prevents an assert() in compressWithFilters(),
        // which assumes it has free choice on each call [block].
        // And if the choices aren't the same on each block,
        // then un-filtering will give incorrect results.
        int strategy = getStrategy(ft);
        if (file_size > (off_t)blocksize)
            strategy = -3;      // no filters

        int l = fi->readx(ibuf, UPX_MIN(blocksize, remaining));
        remaining -= l;

        // Note: compression for a block can fail if the
        //       file is e.g. blocksize + 1 bytes long

        // compress
        ph.overlap_overhead = 0;
        ph.c_len = ph.u_len = l;
        ft.buf_len = l;

        // compressWithFilters() updates u_adler _inside_ compress();
        // that is, AFTER filtering.  We want BEFORE filtering,
        // so that decompression checks the end-to-end checksum.
        unsigned const end_u_adler = upx_adler32(ibuf, ph.u_len, ph.u_adler);
        compressWithFilters(&ft, OVERHEAD, strategy);

        if (ph.c_len < ph.u_len) {
            ph.overlap_overhead = OVERHEAD;
            if (!testOverlappingDecompression(obuf, ph.overlap_overhead)) {
                // not in-place compressible
                ph.c_len = ph.u_len;
            }
        }
        if (ph.c_len >= ph.u_len) {
            // block is not compressible
            ph.c_len = ph.u_len;
            // must manually update checksum of compressed data
            ph.c_adler = upx_adler32(ibuf, ph.u_len, ph.saved_c_adler);
        }

        // write block header
        b_info blk_info;
        memset(&blk_info, 0, sizeof(blk_info));
        set_native32(&blk_info.sz_unc, ph.u_len);
        set_native32(&blk_info.sz_cpr, ph.c_len);
        if (ph.c_len < ph.u_len) {
            blk_info.b_method = (unsigned char) ph.method;
            blk_info.b_ftid = (unsigned char) ph.filter;
            blk_info.b_cto8 = (unsigned char) ph.filter_cto;
        }
        fo->write(&blk_info, sizeof(blk_info));
        b_len += sizeof(b_info);

        // write compressed data
        if (ph.c_len < ph.u_len) {
            fo->write(obuf, ph.c_len);
            verifyOverlappingDecompression();  // uses ph.u_adler
        }
        else {
            fo->write(ibuf, ph.u_len);
        }
        ph.u_adler = end_u_adler;

        total_in += ph.u_len;
        total_out += ph.c_len;
    }

    // update header with totals
    ph.u_len = total_in;
    ph.c_len = total_out;

    if ((off_t)total_in != file_size) {
        throwEOFException();
    }
}

void PackUnix::pack3(OutputFile *fo, Filter &ft)
{
    upx_byte const *p = getLoader();
    lsize = getLoaderSize();
    patchFilter32(const_cast<upx_byte *>(p), lsize, &ft);
    updateLoader(fo);
    patchLoaderChecksum();
    fo->write(p, lsize);
}

void PackUnix::pack4(OutputFile *fo, Filter &)
{
    writePackHeader(fo);

    unsigned tmp;
    set_native32(&tmp, overlay_offset);
    fo->write(&tmp, sizeof(tmp));
}

void PackUnix::pack(OutputFile *fo)
{
    Filter ft(ph.level);
    ft.addvalue = 0;
    b_len = 0;
    progid = 0;

    // set options
    blocksize = opt->unix.blocksize;
    if (blocksize <= 0)
        blocksize = BLOCKSIZE;
    if ((off_t)blocksize > file_size)
        blocksize = file_size;

    // init compression buffers
    ibuf.alloc(blocksize);
    obuf.allocForCompression(blocksize);

    fi->seek(0, SEEK_SET);
    pack1(fo, ft);  // generate Elf header, etc.

    p_info hbuf;
    set_native32(&hbuf.p_progid, progid);
    set_native32(&hbuf.p_filesize, file_size);
    set_native32(&hbuf.p_blocksize, blocksize);
    fo->write(&hbuf, sizeof(hbuf));

    pack2(fo, ft);  // append the compressed body

    // write block end marker (uncompressed size 0)
    b_info hdr; memset(&hdr, 0, sizeof(hdr));
    set_le32(&hdr.sz_cpr, UPX_MAGIC_LE32);
    fo->write(&hdr, sizeof(hdr));

    pack3(fo, ft);  // append loader

    pack4(fo, ft);  // append PackHeader and overlay_offset; update Elf header

    // finally check the compression ratio
    if (!checkFinalCompressionRatio(fo))
        throwNotCompressible();
}


/*************************************************************************
// Generic Unix canUnpack().
**************************************************************************/

int PackUnix::canUnpack()
{
    upx_byte buf[128];
    const int bufsize = sizeof(buf);

    fi->seek(-bufsize, SEEK_END);
    fi->readx(buf, bufsize);
    if (!getPackHeader(buf, bufsize))
        return false;

    int l = ph.buf_offset + ph.getPackHeaderSize();
    if (l < 0 || l + 4 > bufsize)
        throwCantUnpack("file corrupted");
    overlay_offset = get_native32(buf+l);
    if ((off_t)overlay_offset >= file_size)
        throwCantUnpack("file corrupted");

    return true;
}


/*************************************************************************
// Generic Unix unpack().
//
// This code looks much like the one in stub/l_linux.c
// See notes there.
**************************************************************************/

void PackUnix::unpack(OutputFile *fo)
{
    unsigned szb_info = sizeof(b_info);
    {
        Elf_LE32_Ehdr ehdr;
        fi->seek(0, SEEK_SET);
        fi->readx(&ehdr, sizeof(ehdr));
        unsigned const e_entry = get_native32(&ehdr.e_entry);
        if (e_entry < 0x401180) { /* old style, 8-byte b_info */
            szb_info = 2*sizeof(unsigned);
        }
        else {
            Elf_LE32_Phdr phdr;
            fi->seek(get_native32(&ehdr.e_phoff), SEEK_SET);
            fi->readx(&phdr, sizeof(phdr));
            unsigned const p_vaddr = get_native32(&phdr.p_vaddr);
            if (0x80==(e_entry - p_vaddr)) { /* 1.22 old style */
                szb_info = 2*sizeof(unsigned);
            }
        }
    }

    unsigned c_adler = upx_adler32(NULL, 0);
    unsigned u_adler = upx_adler32(NULL, 0);

    // defaults for ph.version == 8
    unsigned orig_file_size = 0;
    blocksize = 512 * 1024;

    fi->seek(overlay_offset, SEEK_SET);
    if (ph.version > 8)
    {
        p_info hbuf;
        fi->readx(&hbuf, sizeof(hbuf));
        orig_file_size = get_native32(&hbuf.p_filesize);
        blocksize = get_native32(&hbuf.p_blocksize);

        if (file_size > (off_t)orig_file_size || blocksize > orig_file_size)
            throwCantUnpack("file header corrupted");
    }
    else
    {
        // skip 4 bytes (program id)
        fi->seek(4, SEEK_CUR);
    }

    ibuf.alloc(blocksize + OVERHEAD);

    // decompress blocks
    unsigned total_in = 0;
    unsigned total_out = 0;
    b_info bhdr; memset(&bhdr, 0, sizeof(bhdr));
    for (;;)
    {
#define buf ibuf
        int i;
        unsigned sz_unc, sz_cpr;

        fi->readx(&bhdr, szb_info);
        ph.u_len = sz_unc = get_native32(&bhdr.sz_unc);
        ph.c_len = sz_cpr = get_native32(&bhdr.sz_cpr);

        if (sz_unc == 0)                   // uncompressed size 0 -> EOF
        {
            // note: must reload sz_cpr as magic is always stored le32
            sz_cpr = get_le32(&bhdr.sz_cpr);
            if (sz_cpr != UPX_MAGIC_LE32)  // sz_cpr must be h->magic
                throwCompressedDataViolation();
            break;
        }
        if (sz_unc <= 0 || sz_cpr <= 0)
            throwCompressedDataViolation();
        if (sz_cpr > sz_unc || sz_unc > blocksize)
            throwCompressedDataViolation();

        i = blocksize + OVERHEAD - sz_cpr;
        fi->readx(buf+i, sz_cpr);
        // update checksum of compressed data
        c_adler = upx_adler32(buf + i, sz_cpr, c_adler);
        // decompress
        if (sz_cpr < sz_unc) {
            decompress(buf+i, buf, false);
            if (0!=bhdr.b_ftid) {
                Filter ft(ph.level);
                ft.init(bhdr.b_ftid);
                ft.cto = bhdr.b_cto8;
                ft.unfilter(buf, sz_unc);
            }
            i = 0;
        }
        // update checksum of uncompressed data
        u_adler = upx_adler32(buf + i, sz_unc, u_adler);
        total_in  += sz_cpr;
        total_out += sz_unc;
        // write block
        if (fo)
            fo->write(buf + i, sz_unc);
#undef buf
    }

    // update header with totals
    ph.c_len = total_in;
    ph.u_len = total_out;

    // all bytes must be written
    if (ph.version > 8 && total_out != orig_file_size)
        throwEOFException();

    // finally test the checksums
    if (ph.c_adler != c_adler || ph.u_adler != u_adler)
        throwChecksumError();
}


/*
vi:ts=4:et
*/

