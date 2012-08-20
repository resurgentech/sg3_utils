#define _XOPEN_SOURCE 600
#ifndef _GNU_SOURCE
#define _GNU_SOURCE     /* resolves u_char typedef in scsi/scsi.h [lk 2.4] */
#endif

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#define __STDC_FORMAT_MACROS 1
#include <inttypes.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <sys/file.h>
#include <linux/major.h>
#include <linux/fs.h>   /* <sys/mount.h> */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "sg_lib.h"
#include "sg_cmds_basic.h"
#include "sg_cmds_extra.h"
#include "sg_io_linux.h"

/* A utility program for copying files. Similar to 'dd' but using
 * the 'Extended Copy' command.
 *
 *  Copyright (c) 2011-2012 Hannes Reinecke, SUSE Labs
 *
 *  Largerly taken from 'sg_dd', which has the
 *
 *  Copyright (C) 1999 - 2010 D. Gilbert and P. Allworth
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.

   This program is a specialisation of the Unix "dd" command in which
   either the input or the output file is a scsi generic device, raw
   device, a block device or a normal file. The block size ('bs') is
   assumed to be 512 if not given. This program complains if 'ibs' or
   'obs' are given with a value that differs from 'bs' (or the default 512).
   If 'if' is not given or 'if=-' then stdin is assumed. If 'of' is
   not given or 'of=-' then stdout assumed.

   A non-standard argument "bpt" (blocks per transfer) is added to control
   the maximum number of blocks in each transfer. The default value is 128.
   For example if "bs=512" and "bpt=32" then a maximum of 32 blocks (16 KiB
   in this case) is transferred to or from the sg device in a single SCSI
   command.

   This version is designed for the linux kernel 2.4 and 2.6 series.
*/

static char * version_str = "0.2 20120322";

#define ME "sg_xcp: "

#define SG_DEBUG

#define STR_SZ 1024
#define INOUTF_SZ 512
#define EBUFF_SZ 512

#define DEF_BLOCK_SIZE 512
#define DEF_BLOCKS_PER_TRANSFER 128
#define DEF_BLOCKS_PER_2048TRANSFER 32

#define DEF_MODE_RESP_LEN 252
#define RW_ERR_RECOVERY_MP 1
#define CACHING_MP 8
#define CONTROL_MP 0xa

#define SENSE_BUFF_LEN 32       /* Arbitrary, could be larger */
#define READ_CAP_REPLY_LEN 8
#define RCAP16_REPLY_LEN 32
#define EXTENDED_COPY_OPCODE 0x83
#define READ_LONG_OPCODE 0x3E
#define READ_LONG_CMD_LEN 10
#define READ_LONG_DEF_BLK_INC 8

#define DEF_TIMEOUT 60000       /* 60,000 millisecs == 60 seconds */

#ifndef RAW_MAJOR
#define RAW_MAJOR 255   /*unlikey value */
#endif

#define SG_LIB_FLOCK_ERR 90

#define FT_OTHER 1              /* filetype is probably normal */
#define FT_SG 2                 /* filetype is sg char device or supports
                                   SG_IO ioctl */
#define FT_RAW 4                /* filetype is raw char device */
#define FT_DEV_NULL 8           /* either "/dev/null" or "." as filename */
#define FT_ST 16                /* filetype is st char device (tape) */
#define FT_BLOCK 32             /* filetype is block device */
#define FT_FIFO 64              /* filetype is a fifo (name pipe) */
#define FT_ERROR 128            /* couldn't "stat" file */

#define TD_FC_WWPN 1
#define TD_FC_PORT 2
#define TD_FC_WWPN_AND_PORT 4
#define TD_SPI 8
#define TD_VPD 16
#define TD_IPV4 32
#define TD_ALIAS 64
#define TD_RDMA 128
#define TD_FW 256
#define TD_SAS 512

#define DEV_NULL_MINOR_NUM 3

#define MIN_RESERVED_SIZE 8192

#define MAX_UNIT_ATTENTIONS 10
#define MAX_ABORTED_CMDS 256

static int64_t dd_count = -1;
static int64_t in_full = 0;
static int in_partial = 0;
static int64_t out_full = 0;
static int out_partial = 0;
static int recovered_errs = 0;
static int unrecovered_errs = 0;
static int num_retries = 0;

static int do_time = 0;
static int verbose = 0;
static int start_tm_valid = 0;
static struct timeval start_tm;
static int blk_sz = 0;
static int priority = 1;
static int list_id_usage = -1;

struct xcopy_fp_t {
    char fname[INOUTF_SZ];
    dev_t devno;
    int sg_type;
    int sg_fd;
    int append;
    int excl;
    int flock;
    int cat;     /* Destination count    */
    int dc;      /* Descriptor type code */
    int pdt;     /* Peripheral device type */
    int retries;
};

static struct xcopy_fp_t ifp;
static struct xcopy_fp_t ofp;

static void calc_duration_throughput(int contin);


static void
install_handler(int sig_num, void (*sig_handler) (int sig))
{
    struct sigaction sigact;
    sigaction (sig_num, NULL, &sigact);
    if (sigact.sa_handler != SIG_IGN)
    {
        sigact.sa_handler = sig_handler;
        sigemptyset (&sigact.sa_mask);
        sigact.sa_flags = 0;
        sigaction (sig_num, &sigact, NULL);
    }
}


static void
print_stats(const char * str)
{
    if (0 != dd_count)
        fprintf(stderr, "  remaining block count=%"PRId64"\n", dd_count);
    fprintf(stderr, "%s%"PRId64"+%d records in\n", str, in_full - in_partial,
            in_partial);
    fprintf(stderr, "%s%"PRId64"+%d records out\n", str, out_full - out_partial,
            out_partial);
    if (recovered_errs > 0)
        fprintf(stderr, "%s%d recovered errors\n", str, recovered_errs);
    if (num_retries > 0)
        fprintf(stderr, "%s%d retries attempted\n", str, num_retries);
    else if (unrecovered_errs)
        fprintf(stderr, "%s%d unrecovered error(s)\n", str,
                unrecovered_errs);
}


static void
interrupt_handler(int sig)
{
    struct sigaction sigact;

    sigact.sa_handler = SIG_DFL;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigaction(sig, &sigact, NULL);
    fprintf(stderr, "Interrupted by signal,");
    if (do_time)
        calc_duration_throughput(0);
    print_stats("");
    kill(getpid (), sig);
}


static void
siginfo_handler(int sig)
{
    sig = sig;  /* dummy to stop -W warning messages */
    fprintf(stderr, "Progress report, continuing ...\n");
    if (do_time)
        calc_duration_throughput(1);
    print_stats("  ");
}

static int bsg_major_checked = 0;
static int bsg_major = 0;

static void
find_bsg_major(void)
{
    const char * proc_devices = "/proc/devices";
    FILE *fp;
    char a[128];
    char b[128];
    char * cp;
    int n;

    if (NULL == (fp = fopen(proc_devices, "r"))) {
        if (verbose)
            fprintf(stderr, "fopen %s failed: %s\n", proc_devices,
                    strerror(errno));
        return;
    }
    while ((cp = fgets(b, sizeof(b), fp))) {
        if ((1 == sscanf(b, "%s", a)) &&
            (0 == memcmp(a, "Character", 9)))
            break;
    }
    while (cp && (cp = fgets(b, sizeof(b), fp))) {
        if (2 == sscanf(b, "%d %s", &n, a)) {
            if (0 == strcmp("bsg", a)) {
                bsg_major = n;
                break;
            }
        } else
            break;
    }
    if (verbose > 5) {
        if (cp)
            fprintf(stderr, "found bsg_major=%d\n", bsg_major);
        else
            fprintf(stderr, "found no bsg char device in %s\n", proc_devices);
    }
    fclose(fp);
}

static int
open_sg(struct xcopy_fp_t * fp, int verbose)
{
    int devmajor, devminor, offset;
    struct sg_simple_inquiry_resp sir;
    char ebuff[EBUFF_SZ];
    int len;

    devmajor = major(fp->devno);
    devminor = minor(fp->devno);

    if (fp->sg_type & FT_SG) {
        snprintf(ebuff, EBUFF_SZ, "%s", fp->fname);
    } else if (fp->sg_type & FT_BLOCK) {
        int fd;

        snprintf(ebuff, EBUFF_SZ, "/sys/dev/block/%d:%d/partition",
                 devmajor, devminor);
        if ((fd = open(ebuff, O_RDONLY)) < 0) {
            perror("opening partition");
        } else {
            len = read(fd, ebuff, EBUFF_SZ);
            if (len < 0) {
                perror("read partition");
            } else {
                offset = strtoul(ebuff, NULL, 10);
                devminor -= offset;
            }
            close(fd);
        }
        snprintf(ebuff, EBUFF_SZ, "/dev/block/%d:%d", devmajor, devminor);
    } else {
        snprintf(ebuff, EBUFF_SZ, "/dev/char/%d:%d", devmajor, devminor);
    }
    fp->sg_fd = sg_cmds_open_device(ebuff, 0, verbose);
    if (fp->sg_fd < 0) {
        snprintf(ebuff, EBUFF_SZ,
                 ME "could not open %s device %d:%d for sg",
                 fp->sg_type & FT_BLOCK ? "block" : "char",
                 devmajor, devminor);
        perror(ebuff);
        return -1;
    }
    if (sg_simple_inquiry(fp->sg_fd, &sir, 0, verbose)) {
        fprintf(stderr, "INQUIRY failed on %s\n", ebuff);
        sg_cmds_close_device(fp->sg_fd);
        fp->sg_fd = -1;
        return fp->sg_fd;
    }

    fp->pdt = sir.peripheral_type;
    if (verbose)
        fprintf(stderr, "    %s: %.8s  %.16s  %.4s  [pdt=%d]\n",
                fp->fname, sir.vendor, sir.product, sir.revision, fp->pdt);

    return fp->sg_fd;
}

static int
dd_filetype(struct xcopy_fp_t * fp)
{
    struct stat st;
    size_t len = strlen(fp->fname);

    if ((1 == len) && ('.' == fp->fname[0]))
        return FT_DEV_NULL;
    if (stat(fp->fname, &st) < 0)
        return FT_ERROR;
    if (S_ISCHR(st.st_mode)) {
        fp->devno = st.st_rdev;
        /* major() and minor() defined in sys/sysmacros.h */
        if ((MEM_MAJOR == major(st.st_rdev)) &&
            (DEV_NULL_MINOR_NUM == minor(st.st_rdev)))
            return FT_DEV_NULL;
        if (RAW_MAJOR == major(st.st_rdev))
            return FT_RAW;
        if (SCSI_GENERIC_MAJOR == major(st.st_rdev))
            return FT_SG;
        if (SCSI_TAPE_MAJOR == major(st.st_rdev))
            return FT_ST;
        if (! bsg_major_checked) {
            bsg_major_checked = 1;
            find_bsg_major();
        }
        if (bsg_major == (int)major(st.st_rdev))
            return FT_SG;
    } else if (S_ISBLK(st.st_mode)) {
        fp->devno = st.st_rdev;
        return FT_BLOCK;
    } else if (S_ISFIFO(st.st_mode)) {
        fp->devno = st.st_dev;
        return FT_FIFO;
    }
    fp->devno = st.st_dev;
    return FT_OTHER | FT_BLOCK;
}


static char *
dd_filetype_str(int ft, char * buff)
{
    int off = 0;

    if (FT_DEV_NULL & ft)
        off += snprintf(buff + off, 32, "null device ");
    if (FT_SG & ft)
        off += snprintf(buff + off, 32, "SCSI generic (sg) device ");
    if (FT_BLOCK & ft)
        off += snprintf(buff + off, 32, "block device ");
    if (FT_FIFO & ft)
        off += snprintf(buff + off, 32, "fifo (named pipe) ");
    if (FT_ST & ft)
        off += snprintf(buff + off, 32, "SCSI tape device ");
    if (FT_RAW & ft)
        off += snprintf(buff + off, 32, "raw device ");
    if (FT_OTHER & ft)
        off += snprintf(buff + off, 32, "other (perhaps ordinary file) ");
    if (FT_ERROR & ft)
        off += snprintf(buff + off, 32, "unable to 'stat' file ");
    return buff;
}


static void
usage()
{
    fprintf(stderr, "Usage: "
           "sg_xcopy  [bs=BS] [count=COUNT] [ibs=BS] [if=IFILE]"
           " [iflag=FLAGS]\n"
           "              [obs=BS] [of=OFILE] [oflag=FLAGS] "
           "[seek=SEEK] [skip=SKIP]\n"
           "              [--help] [--version]\n\n"
           "              [list_id=ID] [id_usage=hold|discard] \n"
           "              [bpt=BPT] [cat=0|1] [dc=0|1] [odir=0|1] "
           "[of2=OFILE2] [prio=PRIO] [retries=RETR]\n"
           "              [time=0|1] [verbose=VERB]\n"
           "  where:\n"
           "    bpt         is blocks_per_transfer (default is 128 or 32 "
           "when BS>=2048)\n"
           "    bs          block size (default is 512)\n");
    fprintf(stderr,
           "    count       number of blocks to copy (def: device size)\n"
           "    ibs         input block size (if given must be same as "
           "'bs=')\n"
           "    if          file or device to read from (def: stdin)\n"
           "    iflag       comma separated list from: [cat,dc,excl,\n"
           "                flock,null]\n"
           "    obs         output block size (if given must be same as "
           "'bs=')\n"
           "    of          file or device to write to (def: stdout), "
           "OFILE of '.'\n");
    fprintf(stderr,
           "                treated as /dev/null\n"
           "    of2         additional output file (def: /dev/null), "
           "OFILE2 should be\n"
           "                normal file or pipe\n"
           "    oflag       comma separated list from: [append,cat,dc,\n"
           "                excl,flock,null]\n"
           "    prio        Use priority PRIO (def: 1)\n"
           "    retries     retry sgio errors RETR times (def: 0)\n"
           "    seek        block position to start writing to OFILE\n"
           "    skip        block position to start reading from IFILE\n"
           "    time        0->no timing(def), 1->time plus calculate "
           "throughput\n"
           "    verbose     0->quiet(def), 1->some noise, 2->more noise, "
           "etc\n"
           "    --help      print out this usage message then exit\n"
           "    --version   print version information then exit\n\n"
           "copy from IFILE to OFILE, similar to dd command; "
           "but using the EXTENDED COPY SCSI command\n");
}

static int
scsi_extended_copy(int sg_fd, unsigned char list_id,
                   unsigned char *src_desc, unsigned char *dst_desc,
                   int64_t num_blk, uint64_t src_lba, uint64_t dst_lba)
{
    unsigned char xcopyBuff[256];
    unsigned char *seg_desc;
    int verb;

    verb = (verbose ? verbose - 1: 0);

    memset(xcopyBuff, 0, 256);
    xcopyBuff[0] = list_id;
    xcopyBuff[1] = (list_id_usage << 3) | priority;
    xcopyBuff[2] = 0;
    xcopyBuff[3] = 64; /* Two target descriptors */
    xcopyBuff[11] = 28; /* One segment descriptor */
    memcpy(xcopyBuff + 16, src_desc, 32);
    memcpy(xcopyBuff + 48, dst_desc, 32);
    seg_desc = xcopyBuff + 80;
    seg_desc[0] = 0x02;
    seg_desc[1] = ifp.cat | (ifp.dc << 1);
    seg_desc[2] = 0;
    seg_desc[3] = 0x18;
    seg_desc[4] = 0;
    seg_desc[5] = 0; /* Source target index */
    seg_desc[7] = 1; /* Destination target index */
    seg_desc[10] = (num_blk >> 8) & 0xff;
    seg_desc[11] = num_blk & 0xff;
    seg_desc[12] = (src_lba >> 56) & 0xff;
    seg_desc[13] = (src_lba >> 48) & 0xff;
    seg_desc[14] = (src_lba >> 40) & 0xff;
    seg_desc[15] = (src_lba >> 32) & 0xff;
    seg_desc[16] = (src_lba >> 24) & 0xff;
    seg_desc[17] = (src_lba >> 16) & 0xff;
    seg_desc[18] = (src_lba >> 8) & 0xff;
    seg_desc[19] = src_lba & 0xff;
    seg_desc[20] = (dst_lba >> 56) & 0xff;
    seg_desc[21] = (dst_lba >> 48) & 0xff;
    seg_desc[22] = (dst_lba >> 40) & 0xff;
    seg_desc[23] = (dst_lba >> 32) & 0xff;
    seg_desc[24] = (dst_lba >> 24) & 0xff;
    seg_desc[25] = (dst_lba >> 16) & 0xff;
    seg_desc[26] = (dst_lba >> 8) & 0xff;
    seg_desc[27] = dst_lba & 0xff;

    if (verbose > 3) {
        fprintf(stderr, "\nParameter list in hex:\n");
        dStrHex((const char *)xcopyBuff, 108, 1);
    }
    return sg_ll_extended_copy(sg_fd, xcopyBuff, 108, 0, verb);
}

/* Return of 0 -> success, see sg_ll_read_capacity*() otherwise */
static int
scsi_read_capacity(int sg_fd, int64_t * num_sect, int * sect_sz)
{
    int k, res;
    unsigned int ui;
    unsigned char rcBuff[RCAP16_REPLY_LEN];
    int verb;

    verb = (verbose ? verbose - 1: 0);
    res = sg_ll_readcap_10(sg_fd, 0, 0, rcBuff, READ_CAP_REPLY_LEN, 0, verb);
    if (0 != res)
        return res;

    if ((0xff == rcBuff[0]) && (0xff == rcBuff[1]) && (0xff == rcBuff[2]) &&
        (0xff == rcBuff[3])) {
        int64_t ls;

        res = sg_ll_readcap_16(sg_fd, 0, 0, rcBuff, RCAP16_REPLY_LEN, 0,
                               verb);
        if (0 != res)
            return res;
        for (k = 0, ls = 0; k < 8; ++k) {
            ls <<= 8;
            ls |= rcBuff[k];
        }
        *num_sect = ls + 1;
        *sect_sz = (rcBuff[8] << 24) | (rcBuff[9] << 16) |
                   (rcBuff[10] << 8) | rcBuff[11];
    } else {
        ui = ((rcBuff[0] << 24) | (rcBuff[1] << 16) | (rcBuff[2] << 8) |
              rcBuff[3]);
        /* take care not to sign extend values > 0x7fffffff */
        *num_sect = (int64_t)ui + 1;
        *sect_sz = (rcBuff[4] << 24) | (rcBuff[5] << 16) |
                   (rcBuff[6] << 8) | rcBuff[7];
    }
    if (verbose)
        fprintf(stderr, "      number of blocks=%"PRId64" [0x%"PRIx64"], block "
                "size=%d\n", *num_sect, *num_sect, *sect_sz);
    return 0;
}

static int
scsi_operating_parameter(int sg_fd, int type, int is_target,
                         unsigned long *max_bytep)
{
    int res;
    unsigned char rcBuff[256];
    unsigned int rcBuffLen = 256, len, n, td_list = 0;
    unsigned long max_segment_len, max_segment_num, held_data_limit, num;
    int verb, valid = 0;

    verb = (verbose ? verbose - 1: 0);
    res = sg_ll_receive_copy_results(sg_fd, 0x03, 0, rcBuff, rcBuffLen, 0, verb);
    if (0 != res)
        return -res;

    len = (rcBuff[0] << 24) | (rcBuff[1] << 16) | (rcBuff[2] << 8) | rcBuff[3];
    if (len > rcBuffLen) {
        fprintf(stderr, "  <<report too long for internal buffer,"
                " output truncated\n");
    }
    if (verbose > 2) {
        fprintf(stderr, "\nOutput response in hex:\n");
        dStrHex((const char *)rcBuff, len, 1);
    }
    printf("Receive copy results (report operating parameters):\n");
    num = rcBuff[8] << 8 | rcBuff[9];
    printf("    Maximum target descriptor count: %lu\n", num);
    max_segment_num = rcBuff[10] << 8 | rcBuff[11];
    printf("    Maximum segment descriptor count: %lu\n", max_segment_num);
    num = rcBuff[12] << 24 | rcBuff[13] << 16 | rcBuff[14] << 8 | rcBuff[15];
    printf("    Maximum descriptor list length: %lu\n", num);
    max_segment_len = rcBuff[16] << 24 | rcBuff[17] << 16 |
        rcBuff[18] << 8 | rcBuff[19];
    *max_bytep = max_segment_len;
    printf("    Maximum segment length: %lu\n", max_segment_len);
    num = rcBuff[20] << 24 | rcBuff[21] << 16 | rcBuff[22] << 8 | rcBuff[23];
    printf("    Maximum inline data length: %lu\n", num);
    held_data_limit = rcBuff[24] << 24 | rcBuff[25] << 16 |
        rcBuff[26] << 8 | rcBuff[27];
    if (list_id_usage < 0) {
        if (!held_data_limit)
            list_id_usage = 2;
        else
            list_id_usage = 0;
    }
    printf("    Held data limit: %lu (usage: %d)\n", held_data_limit, list_id_usage);
    num = rcBuff[28] << 24 | rcBuff[29] << 16 | rcBuff[30] << 8 | rcBuff[31];
    printf("    Maximum stream device transfer size: %lu\n", num);
    printf("    Maximum concurrent copies: %u\n", rcBuff[36]);
    printf("    Data segment granularity: %u\n", rcBuff[37]);
    printf("    Inline data granularity: %u\n", rcBuff[38]);
    printf("    Held data granularity: %u\n", rcBuff[39]);

    printf("    Implemented descriptor list:\n");
    for (n = 0; n < rcBuff[43]; n++) {
        switch(rcBuff[44 + n]) {
        case 0x00: /* copy block to stream device */
            if (!is_target && (type & FT_BLOCK))
                valid++;
            if (is_target && (type & FT_ST))
                valid++;
            printf("        Copy Block to Stream device\n");
            break;
        case 0x01: /* copy stream to block device */
            if (!is_target && (type & FT_ST))
                valid++;
            if (is_target && (type & FT_BLOCK))
                valid++;
            printf("        Copy Stream to Block device\n");
            break;
        case 0x02: /* copy block to block device */
            if (!is_target && (type & FT_BLOCK))
                valid++;
            if (is_target && (type & FT_BLOCK))
                valid++;
            printf("        Copy Block to Block device\n");
            break;
        case 0x03: /* copy stream to stream device */
            if (!is_target && (type & FT_ST))
                valid++;
            if (is_target && (type & FT_ST))
                valid++;
            printf("        Copy Stream to Stream device\n");
            break;
        case 0xe0: /* FC N_Port_Name */
            printf("        FC N_Port_Name target descriptor\n");
            td_list |= TD_FC_WWPN;
            break;
        case 0xe1: /* FC Port_ID */
            printf("        FC Port_ID target descriptor\n");
            td_list |= TD_FC_PORT;
            break;
        case 0xe2: /* FC N_Port_ID with N_Port_Name checking */
            printf("        FC N_Port_ID with N_Port_Name target descriptor\n");
            td_list |= TD_FC_WWPN_AND_PORT;
            break;
        case 0xe3: /* Parallel Interface T_L  */
            printf("        SPI T_L target descriptor\n");
            td_list |= TD_SPI;
            break;
        case 0xe4: /* identification descriptor */
            printf("        Identification target descriptor\n");
            td_list |= TD_VPD;
            break;
        case 0xe5: /* IPv4  */
            printf("        IPv4 target descriptor\n");
            td_list |= TD_IPV4;
            break;
        case 0xe6: /* Alias */
            printf("        Alias target descriptor\n");
            td_list |= TD_ALIAS;
            break;
        case 0xe7: /* RDMA */
            printf("        RDMA target descriptor\n");
            td_list |= TD_RDMA;
            break;
        case 0xe8: /* FireWire */
            printf("        IEEE 1394 target descriptor\n");
            td_list |= TD_FW;
            break;
        case 0xe9: /* SAS */
            printf("        SAS target descriptor\n");
            td_list |= TD_SAS;
            break;
        default:
            printf("        Unhandled target descriptor 0x%02x\n",
                   rcBuff[44 + n]);
            break;
        }
    }
    if (!valid) {
        fprintf(stderr, ">> no matching target descriptor supported\n");
        td_list = 0;
    }
    return td_list;
}

static void
decode_designation_descriptor(const unsigned char * ucp, int i_len)
{
    int m, p_id, piv, c_set, assoc, desig_type, d_id, naa;
    int k;
    const unsigned char * ip;
    uint64_t vsei;
    char b[64];

    ip = ucp + 4;
    p_id = ((ucp[0] >> 4) & 0xf);
    c_set = (ucp[0] & 0xf);
    piv = ((ucp[1] & 0x80) ? 1 : 0);
    assoc = ((ucp[1] >> 4) & 0x3);
    desig_type = (ucp[1] & 0xf);
    printf("    designator type: %d,  code set: %d\n", desig_type, c_set);
    if (piv && ((1 == assoc) || (2 == assoc)))
        printf("     transport: %s\n",
               sg_get_trans_proto_str(p_id, sizeof(b), b));
    /* printf("    associated with the %s\n", assoc_arr[assoc]); */
    switch (desig_type) {
    case 0: /* vendor specific */
        k = 0;
        if ((1 == c_set) || (2 == c_set)) { /* ASCII or UTF-8 */
            for (k = 0; (k < i_len) && isprint(ip[k]); ++k)
                ;
            if (k >= i_len)
                k = 1;
        }
        if (k)
            printf("      vendor specific: %.*s\n", i_len, ip);
        else
            dStrHex((const char *)ip, i_len, 0);
        break;
    case 1: /* T10 vendor identification */
        printf("      vendor id: %.8s\n", ip);
        if (i_len > 8)
            printf("      vendor specific: %.*s\n", i_len - 8, ip + 8);
        break;
    case 2: /* EUI-64 based */
        if ((8 != i_len) && (12 != i_len) && (16 != i_len)) {
            fprintf(stderr, "      << expect 8, 12 and 16 byte "
                    "EUI, got %d>>\n", i_len);
            dStrHex((const char *)ip, i_len, 0);
            break;
        }
        printf("      0x");
        for (m = 0; m < i_len; ++m)
            printf("%02x", (unsigned int)ip[m]);
        printf("\n");
        break;
    case 3: /* NAA */
        if (1 != c_set) {
            fprintf(stderr, "      << unexpected code set %d for "
                    "NAA>>\n", c_set);
            dStrHex((const char *)ip, i_len, 0);
            break;
        }
        naa = (ip[0] >> 4) & 0xff;
        if (! ((2 == naa) || (5 == naa) || (6 == naa))) {
            fprintf(stderr, "      << unexpected NAA [0x%x]>>\n", naa);
            dStrHex((const char *)ip, i_len, 0);
            break;
        }
        if ((5 == naa) && (0x10 == i_len)) {
            if (verbose > 2)
                fprintf(stderr, "      << unexpected NAA 5 len 16, assuming "
                        "NAA 6 >>\n");
            naa = 6;
        }
        if (2 == naa) {
            if (8 != i_len) {
                fprintf(stderr, "      << unexpected NAA 2 identifier "
                        "length: 0x%x>>\n", i_len);
                dStrHex((const char *)ip, i_len, 0);
                break;
            }
            d_id = (((ip[0] & 0xf) << 8) | ip[1]);
            /* c_id = ((ip[2] << 16) | (ip[3] << 8) | ip[4]); */
            /* vsi = ((ip[5] << 16) | (ip[6] << 8) | ip[7]); */
            printf("      0x");
            for (m = 0; m < 8; ++m)
                printf("%02x", (unsigned int)ip[m]);
            printf("\n");
        } else if (5 == naa) {
            if (8 != i_len) {
                fprintf(stderr, "      << unexpected NAA 5 identifier "
                        "length: 0x%x>>\n", i_len);
                dStrHex((const char *)ip, i_len, 0);
                break;
            }
            /* c_id = (((ip[0] & 0xf) << 20) | (ip[1] << 12) | */
                    /* (ip[2] << 4) | ((ip[3] & 0xf0) >> 4)); */
            vsei = ip[3] & 0xf;
            for (m = 1; m < 5; ++m) {
                vsei <<= 8;
                vsei |= ip[3 + m];
            }
            printf("      0x");
            for (m = 0; m < 8; ++m)
                printf("%02x", (unsigned int)ip[m]);
            printf("\n");
        } else if (6 == naa) {
            if (16 != i_len) {
                fprintf(stderr, "      << unexpected NAA 6 identifier "
                        "length: 0x%x>>\n", i_len);
                dStrHex((const char *)ip, i_len, 0);
                break;
            }
            /* c_id = (((ip[0] & 0xf) << 20) | (ip[1] << 12) | */
                    /* (ip[2] << 4) | ((ip[3] & 0xf0) >> 4)); */
            vsei = ip[3] & 0xf;
            for (m = 1; m < 5; ++m) {
                vsei <<= 8;
                vsei |= ip[3 + m];
            }
            printf("      0x");
            for (m = 0; m < 16; ++m)
                printf("%02x", (unsigned int)ip[m]);
            printf("\n");
        }
        break;
    case 4: /* Relative target port */
        if ((1 != c_set) || (1 != assoc) || (4 != i_len)) {
            fprintf(stderr, "      << expected binary code_set, target "
                    "port association, length 4>>\n");
            dStrHex((const char *)ip, i_len, 0);
            break;
        }
        d_id = ((ip[2] << 8) | ip[3]);
        printf("      Relative target port: 0x%x\n", d_id);
        break;
    case 5: /* (primary) Target port group */
        if ((1 != c_set) || (1 != assoc) || (4 != i_len)) {
            fprintf(stderr, "      << expected binary code_set, target "
                    "port association, length 4>>\n");
            dStrHex((const char *)ip, i_len, 0);
            break;
        }
        d_id = ((ip[2] << 8) | ip[3]);
        printf("      Target port group: 0x%x\n", d_id);
        break;
    case 6: /* Logical unit group */
        if ((1 != c_set) || (0 != assoc) || (4 != i_len)) {
            fprintf(stderr, "      << expected binary code_set, logical "
                    "unit association, length 4>>\n");
            dStrHex((const char *)ip, i_len, 0);
            break;
        }
        d_id = ((ip[2] << 8) | ip[3]);
        printf("      Logical unit group: 0x%x\n", d_id);
        break;
    case 7: /* MD5 logical unit identifier */
        if ((1 != c_set) || (0 != assoc)) {
            printf("      << expected binary code_set, logical "
                   "unit association>>\n");
            dStrHex((const char *)ip, i_len, 0);
            break;
        }
        printf("      MD5 logical unit identifier:\n");
        dStrHex((const char *)ip, i_len, 0);
        break;
    case 8: /* SCSI name string */
        if (3 != c_set) {
            fprintf(stderr, "      << expected UTF-8 code_set>>\n");
            dStrHex((const char *)ip, i_len, 0);
            break;
        }
        printf("      SCSI name string:\n");
        /* does %s print out UTF-8 ok??
         * Seems to depend on the locale. Looks ok here with my
         * locale setting: en_AU.UTF-8
         */
        printf("      %s\n", (const char *)ip);
        break;
    default: /* reserved */
        dStrHex((const char *)ip, i_len, 0);
        break;
    }
}

static int
desc_from_vpd_id(int sg_fd, unsigned char *desc, int desc_len,
                 unsigned int block_size)
{
    int res;
    unsigned char rcBuff[256], *ucp, *best = NULL;
    unsigned int len = 254;
    int off = -1, u, i_len, best_len = 0, assoc, desig, f_desig = 0;

    memset(rcBuff, 0xff, len);
    res = sg_ll_inquiry(sg_fd, 0, 1, 0x83, rcBuff, 4, 1, verbose);
    if (0 != res) {
        fprintf(stderr, "VPD inquiry failed with %d\n", res);
        return res;
    } else if (rcBuff[1] != 0x83) {
        fprintf(stderr, "invalid VPD response\n");
        return SG_LIB_CAT_MALFORMED;
    }
    len = ((rcBuff[2] << 8) + rcBuff[3]) + 4;
    res = sg_ll_inquiry(sg_fd, 0, 1, 0x83, rcBuff, len, 1, verbose);
    if (0 != res) {
        fprintf(stderr, "VPD inquiry failed with %d\n", res);
        return res;
    } else if (rcBuff[1] != 0x83) {
        fprintf(stderr, "invalid VPD response\n");
        return SG_LIB_CAT_MALFORMED;
    }
    if (verbose > 2) {
        fprintf(stderr, "Output response in hex:\n");
        dStrHex((const char *)rcBuff, len, 1);
    }

    while ((u = sg_vpd_dev_id_iter(rcBuff + 4, len - 4, &off, 0, -1, -1)) == 0) {
        ucp = rcBuff + 4 + off;
        i_len = ucp[3];
        if (((unsigned int)off + i_len + 4) > len) {
            fprintf(stderr, "    VPD page error: designator length %d longer "
                    "than\n     remaining response length=%d\n", i_len, (len - off));
            return SG_LIB_CAT_MALFORMED;
        }
        assoc = ((ucp[1] >> 4) & 0x3);
        desig = (ucp[1] & 0xf);
        fprintf(stderr, "    Desc %d: assoc %u desig %u len %d\n", off,
                assoc, desig, i_len);
        /* Descriptor must be less than 16 bytes */
        if (i_len > 16)
            continue;
        if (desig == 3) {
            best = ucp;
            best_len = i_len;
            break;
        }
        if (desig == 2) {
            if (!best || f_desig < 2) {
                best = ucp;
                best_len = i_len;
                f_desig = 2;
            }
        } else if (desig == 1) {
            if (!best || f_desig == 0) {
                best = ucp;
                best_len = i_len;
                f_desig = desig;
            }
        } else if (desig == 0) {
            if (!best) {
                best = ucp;
                best_len = i_len;
                f_desig = desig;
            }
        }
    }
    if (best) {
        decode_designation_descriptor(best, best_len);
        if (best_len + 4 < desc_len) {
            memset(desc, 0, 32);
            desc[0] = 0xe4;
            memcpy(desc + 4, best, best_len + 4);
            desc[4] &= 0x1f;
            desc[29] = (block_size >> 16) & 0xff;
            desc[30] = (block_size >> 8) & 0xff;
            desc[31] = block_size & 0xff;
            if (verbose > 3) {
                fprintf(stderr, "Descriptor in hex (bs %d):\n", block_size);
                dStrHex((const char *)desc, 32, 1);
            }
            return 32;
        }
        return  best_len + 8;
    }
    return 0;
}

static void
calc_duration_throughput(int contin)
{
    struct timeval end_tm, res_tm;
    double a, b;
    int64_t blks;

    if (start_tm_valid && (start_tm.tv_sec || start_tm.tv_usec)) {
        blks = (in_full > out_full) ? in_full : out_full;
        gettimeofday(&end_tm, NULL);
        res_tm.tv_sec = end_tm.tv_sec - start_tm.tv_sec;
        res_tm.tv_usec = end_tm.tv_usec - start_tm.tv_usec;
        if (res_tm.tv_usec < 0) {
            --res_tm.tv_sec;
            res_tm.tv_usec += 1000000;
        }
        a = res_tm.tv_sec;
        a += (0.000001 * res_tm.tv_usec);
        b = (double)blk_sz * blks;
        fprintf(stderr, "time to transfer data%s: %d.%06d secs",
                (contin ? " so far" : ""), (int)res_tm.tv_sec,
                (int)res_tm.tv_usec);
        if ((a > 0.00001) && (b > 511))
            fprintf(stderr, " at %.2f MB/sec\n", b / (a * 1000000.0));
        else
            fprintf(stderr, "\n");
    }
}

static int
process_flags(const char * arg, struct xcopy_fp_t * fp)
{
    char buff[256];
    char * cp;
    char * np;

    strncpy(buff, arg, sizeof(buff));
    buff[sizeof(buff) - 1] = '\0';
    if ('\0' == buff[0]) {
        fprintf(stderr, "no flag found\n");
        return 1;
    }
    cp = buff;
    do {
        np = strchr(cp, ',');
        if (np)
            *np++ = '\0';
        if (0 == strcmp(cp, "append"))
            fp->append = 1;
        else if (0 == strcmp(cp, "dc"))
            ++fp->dc;
        else if (0 == strcmp(cp, "excl"))
            fp->excl = 1;
        else if (0 == strcmp(cp, "null"))
            ;
        else if (0 == strcmp(cp, "cat"))
            ++fp->cat;
        else if (0 == strcmp(cp, "flock"))
            ++fp->flock;
        else {
            fprintf(stderr, "unrecognised flag: %s\n", cp);
            return 1;
        }
        cp = np;
    } while (cp);
    return 0;
}

/* Returns open input file descriptor (>= 0) or a negative value
 * (-SG_LIB_FILE_ERROR or -SG_LIB_CAT_OTHER) if error.
 */
static int
open_if(struct xcopy_fp_t * ifp, int verbose)
{
    int infd = -1, flags, fl, res;
    char ebuff[EBUFF_SZ];

    ifp->sg_type = dd_filetype(ifp);

    if (verbose)
        fprintf(stderr, " >> Input file type: %s, devno %d:%d\n",
                dd_filetype_str(ifp->sg_type, ebuff),
                major(ifp->devno), minor(ifp->devno));
    if (FT_ERROR & ifp->sg_type) {
        fprintf(stderr, ME "unable access %s\n", ifp->fname);
        goto file_err;
    }
    flags = O_NONBLOCK;
    if (ifp->excl)
        flags |= O_EXCL;
    fl = O_RDWR;
    if ((infd = open(ifp->fname, fl | flags)) < 0) {
        fl = O_RDONLY;
        if ((infd = open(ifp->fname, fl | flags)) < 0) {
            snprintf(ebuff, EBUFF_SZ,
                     ME "could not open %s for sg reading", ifp->fname);
            perror(ebuff);
            goto file_err;
        }
    }
    if (verbose)
        fprintf(stderr, "        open input(sg_io), flags=0x%x\n",
                fl | flags);

    if (ifp->flock) {
        res = flock(infd, LOCK_EX | LOCK_NB);
        if (res < 0) {
            close(infd);
            snprintf(ebuff, EBUFF_SZ, ME "flock(LOCK_EX | LOCK_NB) on %s "
                     "failed", ifp->fname);
            perror(ebuff);
            return -SG_LIB_FLOCK_ERR;
        }
    }
    return infd;

file_err:
    if (infd >= 0)
        close(infd);
    return -SG_LIB_FILE_ERROR;
}

/* Returns open output file descriptor (>= 0), -1 for don't
 * bother opening (e.g. /dev/null), or a more negative value
 * (-SG_LIB_FILE_ERROR or -SG_LIB_CAT_OTHER) if error.
 */
static int
open_of(struct xcopy_fp_t * ofp, int verbose)
{
    int outfd, flags, verb, res;
    char ebuff[EBUFF_SZ];
    struct sg_simple_inquiry_resp sir;

    verb = (verbose ? verbose - 1: 0);
    ofp->sg_type = dd_filetype(ofp);
    if (verbose)
        fprintf(stderr, " >> Output file type: %s, devno %d:%d\n",
                dd_filetype_str(ofp->sg_type, ebuff),
                major(ofp->devno), minor(ofp->devno));

    ofp->sg_type |= FT_SG;

    if (!(FT_DEV_NULL & ofp->sg_type)) {
        flags = O_RDWR | O_NONBLOCK;
        if (ofp->excl)
            flags |= O_EXCL;
        if ((outfd = open(ofp->fname, flags)) < 0) {
            snprintf(ebuff, EBUFF_SZ,
                     ME "could not open %s for sg writing", ofp->fname);
            perror(ebuff);
            goto file_err;
        }
        if (verbose)
            fprintf(stderr, "        open output(sg_io), flags=0x%x\n",
                    flags);
        if (sg_simple_inquiry(outfd, &sir, 0, verb)) {
            fprintf(stderr, "INQUIRY failed on %s\n", ofp->fname);
            goto other_err;
        }
        ofp->pdt = sir.peripheral_type;
        if (verbose)
            fprintf(stderr, "    %s: %.8s  %.16s  %.4s  [pdt=%d]\n",
                    ofp->fname, sir.vendor, sir.product, sir.revision, ofp->pdt);
    } else {
        outfd = -1; /* don't bother opening */
    }
    if (ofp->flock) {
        res = flock(outfd, LOCK_EX | LOCK_NB);
        if (res < 0) {
            close(outfd);
            snprintf(ebuff, EBUFF_SZ, ME "flock(LOCK_EX | LOCK_NB) on %s "
                     "failed", ofp->fname);
            perror(ebuff);
            return -SG_LIB_FLOCK_ERR;
        }
    }
    return outfd;

file_err:
    return -SG_LIB_FILE_ERROR;
other_err:
    return -SG_LIB_CAT_OTHER;
}


int
main(int argc, char * argv[])
{
    int64_t skip = 0;
    int64_t seek = 0;
    int ibs = 0;
    int obs = 0;
    int bpt = DEF_BLOCKS_PER_TRANSFER;
    int bpt_given = 0;
    char str[STR_SZ];
    char * key;
    char * buf;
    int blocks = 0;
    int res, k;
    int infd, outfd;
    int64_t in_num_sect = -1;
    int64_t out_num_sect = -1;
    int in_sect_sz, out_sect_sz;
    int ret = 0;
    unsigned long max_bytes_in, max_bytes_out;
    unsigned char list_id = 1;
    unsigned char src_desc[256];
    unsigned char dst_desc[256];
    /* int src_desc_len = 256; */
    /* int dst_desc_len = 256; */

    ifp.fname[0] = '\0';
    ofp.fname[0] = '\0';

    if (argc < 2) {
        fprintf(stderr,
                "Won't default both IFILE to stdin _and_ OFILE to stdout\n");
        fprintf(stderr, "For more information use '--help'\n");
        return SG_LIB_SYNTAX_ERROR;
    }

    for (k = 1; k < argc; k++) {
        if (argv[k]) {
            strncpy(str, argv[k], STR_SZ);
            str[STR_SZ - 1] = '\0';
        } else
            continue;
        for (key = str, buf = key; *buf && *buf != '=';)
            buf++;
        if (*buf)
            *buf++ = '\0';
        if (0 == strncmp(key, "app", 3)) {
            ifp.append = sg_get_num(buf);
            ofp.append = ifp.append;
        } else if (0 == strcmp(key, "bpt")) {
            bpt = sg_get_num(buf);
            if (-1 == bpt) {
                fprintf(stderr, ME "bad argument to 'bpt='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
            bpt_given = 1;
        } else if (0 == strcmp(key, "bs")) {
            blk_sz = sg_get_num(buf);
            if (-1 == blk_sz) {
                fprintf(stderr, ME "bad argument to 'bs='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "list_id")) {
            ret = sg_get_num(buf);
            if (-1 == ret || ret > 0xff) {
                fprintf(stderr, ME "bad argument to 'list_id='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
            list_id = (ret & 0xff);
        } else if (0 == strcmp(key, "id_usage")) {
            if (!strncmp(buf, "hold", 4))
                list_id_usage = 0;
            else if (!strncmp(buf, "discard", 7))
                list_id_usage = 2;
            else {
                fprintf(stderr, ME "bad argument to 'list_id_usage='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "conv")) {
            if (process_flags(buf, &ofp)) {
                fprintf(stderr, ME "bad argument to 'conv='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "count")) {
            if (0 != strcmp("-1", buf)) {
                dd_count = sg_get_llnum(buf);
                if (-1LL == dd_count) {
                    fprintf(stderr, ME "bad argument to 'count='\n");
                    return SG_LIB_SYNTAX_ERROR;
                }
            }   /* treat 'count=-1' as calculate count (same as not given) */
        } else if (0 == strcmp(key, "prio")) {
            priority = sg_get_num(buf);
        } else if (0 == strcmp(key, "cat")) {
            ofp.cat = sg_get_num(buf);
            ifp.cat = ofp.cat;
        } else if (0 == strcmp(key, "dc")) {
            /* t = sg_get_num(buf); */
            ofp.dc = sg_get_num(buf);
            ifp.dc = ofp.dc;
        } else if (0 == strcmp(key, "ibs")) {
            ibs = sg_get_num(buf);
        } else if (strcmp(key, "if") == 0) {
            if ('\0' != ifp.fname[0]) {
                fprintf(stderr, "Second IFILE argument??\n");
                return SG_LIB_SYNTAX_ERROR;
            } else
                strncpy(ifp.fname, buf, INOUTF_SZ);
        } else if (0 == strcmp(key, "iflag")) {
            if (process_flags(buf, &ifp)) {
                fprintf(stderr, ME "bad argument to 'iflag='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "obs")) {
            obs = sg_get_num(buf);
        } else if (strcmp(key, "of") == 0) {
            if ('\0' != ofp.fname[0]) {
                fprintf(stderr, "Second OFILE argument??\n");
                return SG_LIB_SYNTAX_ERROR;
            } else
                strncpy(ofp.fname, buf, INOUTF_SZ);
        } else if (0 == strcmp(key, "oflag")) {
            if (process_flags(buf, &ofp)) {
                fprintf(stderr, ME "bad argument to 'oflag='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "retries")) {
            ifp.retries = sg_get_num(buf);
            ofp.retries = ifp.retries;
            if (-1 == ifp.retries) {
                fprintf(stderr, ME "bad argument to 'retries='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "seek")) {
            seek = sg_get_llnum(buf);
            if (-1LL == seek) {
                fprintf(stderr, ME "bad argument to 'seek='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "skip")) {
            skip = sg_get_llnum(buf);
            if (-1LL == skip) {
                fprintf(stderr, ME "bad argument to 'skip='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "time"))
            do_time = sg_get_num(buf);
        else if (0 == strncmp(key, "verb", 4))
            verbose = sg_get_num(buf);
        else if ((0 == strncmp(key, "--help", 7)) ||
                   (0 == strcmp(key, "-?"))) {
            usage();
            return 0;
        } else if ((0 == strncmp(key, "--vers", 6)) ||
                   (0 == strcmp(key, "-V"))) {
            fprintf(stderr, ME "%s\n", version_str);
            return 0;
        } else {
            fprintf(stderr, "Unrecognized option '%s'\n", key);
            fprintf(stderr, "For more information use '--help'\n");
            return SG_LIB_SYNTAX_ERROR;
        }
    }
    if (blk_sz <= 0) {
        blk_sz = DEF_BLOCK_SIZE;
        fprintf(stderr, "Assume default 'bs' (block size) of %d bytes\n",
                blk_sz);
    }
    if ((ibs && (ibs != blk_sz)) || (obs && (obs != blk_sz))) {
        fprintf(stderr, "If 'ibs' or 'obs' given must be same as 'bs'\n");
        fprintf(stderr, "For more information use '--help'\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    if ((skip < 0) || (seek < 0)) {
        fprintf(stderr, "skip and seek cannot be negative\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    if ((ofp.append > 0) && (seek > 0)) {
        fprintf(stderr, "Can't use both append and seek switches\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    if (bpt < 1) {
        fprintf(stderr, "bpt must be greater than 0\n");
        return SG_LIB_SYNTAX_ERROR;
    }

#ifdef SG_DEBUG
    fprintf(stderr, ME "if=%s skip=%"PRId64" of=%s seek=%"PRId64" count=%"PRId64"\n",
           ifp.fname, skip, ofp.fname, seek, dd_count);
#endif
    install_handler(SIGINT, interrupt_handler);
    install_handler(SIGQUIT, interrupt_handler);
    install_handler(SIGPIPE, interrupt_handler);
    install_handler(SIGUSR1, siginfo_handler);

    infd = STDIN_FILENO;
    outfd = STDOUT_FILENO;
    ifp.pdt = -1;
    ofp.pdt = -1;
    if (ifp.fname[0] && ('-' != ifp.fname[0])) {
        infd = open_if(&ifp, verbose);
        if (infd < 0)
            return -infd;
    }

    if (ofp.fname[0] && ('-' != ofp.fname[0])) {
        outfd = open_of(&ofp, verbose);
        if (outfd < -1)
            return -outfd;
    }

    if (open_sg(&ifp, verbose) < 0)
        return SG_LIB_CAT_INVALID_OP;

    if (open_sg(&ofp, verbose) < 0)
        return SG_LIB_CAT_INVALID_OP;

    if ((STDIN_FILENO == infd) && (STDOUT_FILENO == outfd)) {
        fprintf(stderr,
                "Can't have both 'if' as stdin _and_ 'of' as stdout\n");
        fprintf(stderr, "For more information use '--help'\n");
        return SG_LIB_SYNTAX_ERROR;
    }

    res = scsi_read_capacity(ifp.sg_fd, &in_num_sect, &in_sect_sz);
    if (SG_LIB_CAT_UNIT_ATTENTION == res) {
        fprintf(stderr, "Unit attention (readcap in), continuing\n");
        res = scsi_read_capacity(ifp.sg_fd, &in_num_sect, &in_sect_sz);
    } else if (SG_LIB_CAT_ABORTED_COMMAND == res) {
        fprintf(stderr, "Aborted command (readcap in), continuing\n");
        res = scsi_read_capacity(infd, &in_num_sect, &in_sect_sz);
    }
    if (0 != res) {
        if (res == SG_LIB_CAT_INVALID_OP)
            fprintf(stderr, "read capacity not supported on %s\n",
                    ifp.fname);
        else if (res == SG_LIB_CAT_NOT_READY)
            fprintf(stderr, "read capacity failed on %s - not "
                    "ready\n", ifp.fname);
        else
            fprintf(stderr, "Unable to read capacity on %s\n", ifp.fname);
        in_num_sect = -1;
    } else if (in_sect_sz != blk_sz) {
        fprintf(stderr, ">> warning: block size on %s confusion: "
                "bs=%d, device claims=%d\n", ifp.fname, blk_sz, in_sect_sz);
    }

    res = scsi_read_capacity(outfd, &out_num_sect, &out_sect_sz);
    if (SG_LIB_CAT_UNIT_ATTENTION == res) {
        fprintf(stderr, "Unit attention (readcap out), continuing\n");
        res = scsi_read_capacity(outfd, &out_num_sect, &out_sect_sz);
    } else if (SG_LIB_CAT_ABORTED_COMMAND == res) {
        fprintf(stderr,
                "Aborted command (readcap out), continuing\n");
        res = scsi_read_capacity(outfd, &out_num_sect, &out_sect_sz);
    }
    if (0 != res) {
        if (res == SG_LIB_CAT_INVALID_OP)
            fprintf(stderr, "read capacity not supported on %s\n",
                    ofp.fname);
        else
            fprintf(stderr, "Unable to read capacity on %s\n", ofp.fname);
        out_num_sect = -1;
    } else if (blk_sz != out_sect_sz) {
        fprintf(stderr, ">> warning: block size on %s confusion: "
                "bs=%d, device claims=%d\n", ofp.fname, blk_sz,
                out_sect_sz);
    }
    if ((dd_count < 0) || ((verbose > 0) && (0 == dd_count))) {
        if (skip && in_num_sect > skip)
            in_num_sect -= skip;
        if (skip && out_num_sect > skip)
            out_num_sect -= skip;
        if (out_num_sect > seek)
            out_num_sect -= seek;

        if (dd_count < 0) {
            if (in_num_sect > 0) {
                if (out_num_sect > 0)
                    dd_count = (in_num_sect > out_num_sect) ? out_num_sect :
                                                           in_num_sect;
                else
                    dd_count = in_num_sect;
            } else
                dd_count = out_num_sect;
        }
    }
#ifdef SG_DEBUG
    fprintf(stderr,
            "Start of loop, count=%"PRId64", lba_in=%"PRId64", "
            "in_num_sect=%"PRId64", lba_out=%"PRId64", out_num_sect=%"PRId64"\n",
            dd_count, skip, in_num_sect, skip + seek, out_num_sect);
#endif

    res = scsi_operating_parameter(infd, ifp.sg_type, 0, &max_bytes_in);
    if (res < 0) {
        if (SG_LIB_CAT_UNIT_ATTENTION == -res) {
            fprintf(stderr, "Unit attention (oper parm), continuing\n");
            res = scsi_operating_parameter(infd, ifp.sg_type, 0, &max_bytes_in);
        } else {
            if (-res == SG_LIB_CAT_INVALID_OP) {
                fprintf(stderr, "receive copy results not supported on %s\n",
                        ifp.fname);
#ifndef SG_DEBUG
                return EINVAL;
#endif
            } else if (-res == SG_LIB_CAT_NOT_READY)
                fprintf(stderr, "receive copy results failed on %s - not "
                        "ready\n", ifp.fname);
            else {
                fprintf(stderr, "Unable to receive copy results on %s\n",
                        ifp.fname);
                return -res;
            }
        }
    } else if (res == 0)
        return SG_LIB_CAT_INVALID_OP;

    if (res & TD_VPD) {
        printf("  >> using VPD identification for source %s\n", ifp.fname);
        res = desc_from_vpd_id(infd, src_desc, 256, in_sect_sz);
        if (res > 256) {
            fprintf(stderr, "source descriptor too large (%d bytes)\n", res);
            return SG_LIB_CAT_MALFORMED;
        }
        /* src_desc_len = res; */
    } else {
        return SG_LIB_CAT_INVALID_OP;
    }

    res = scsi_operating_parameter(outfd, ofp.sg_type, 1, &max_bytes_out);
    if (res < 0) {
        if (SG_LIB_CAT_UNIT_ATTENTION == -res) {
            fprintf(stderr, "Unit attention (oper parm), continuing\n");
            res = scsi_operating_parameter(outfd, ofp.sg_type, 1, &max_bytes_out);
        } else {
            if (-res == SG_LIB_CAT_INVALID_OP) {
                fprintf(stderr, "receive copy results not supported on %s\n",
                        ifp.fname);
#ifndef SG_DEBUG
                return EINVAL;
#endif
            } else if (-res == SG_LIB_CAT_NOT_READY)
                fprintf(stderr, "receive copy results failed on %s - not "
                        "ready\n", ifp.fname);
            else {
                fprintf(stderr, "Unable to receive copy results on %s\n",
                        ofp.fname);
                return -res;
            }
        }
    } else if (res == 0)
        return SG_LIB_CAT_INVALID_OP;

    if (res & TD_VPD) {
        printf("  >> using VPD identification for destination %s\n",
               ofp.fname);
        res = desc_from_vpd_id(outfd, dst_desc, 256, out_sect_sz);
        if (res > 256) {
            fprintf(stderr, "destination descriptor too large (%d bytes)\n",
                    res);
            return SG_LIB_CAT_MALFORMED;
        }
        /* dst_desc_len = res; */
    } else {
        return SG_LIB_CAT_INVALID_OP;
    }

    if (dd_count < 0) {
        fprintf(stderr, "Couldn't calculate count, please give one\n");
        return SG_LIB_CAT_OTHER;
    }

    if (0 == bpt_given)
        bpt = max_bytes_in / in_sect_sz;
    if (max_bytes_out / out_sect_sz < (uint64_t)bpt)
        bpt = max_bytes_out / out_sect_sz;
    if (bpt > dd_count)
        bpt = dd_count;

    if (do_time) {
        start_tm.tv_sec = 0;
        start_tm.tv_usec = 0;
        gettimeofday(&start_tm, NULL);
        start_tm_valid = 1;
    }

    while (dd_count > 0) {
        if (dd_count > bpt)
            blocks = bpt;
        else
            blocks = dd_count;
        res = scsi_extended_copy(infd, list_id, src_desc, dst_desc,
                                 blocks, skip, skip + seek);
        if (res != 0)
            break;
        in_full += blocks;
        skip += blocks;
        dd_count -= blocks;
    }

    if (do_time)
        calc_duration_throughput(0);

    return res;
}