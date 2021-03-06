#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <endian.h>
#include <unistd.h>
#include <fcntl.h>

#include "gxlimg.h"
#include "fip.h"
#include "amlcblk.h"

#define FOUT_MODE_DFT (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP)

#define BL31_ENTRY_MAGIC (0x87654321)
#define BL31_MAGIC (0x12348765)
#define BL2SZ (0xc000)

/**
 * Read a block of data from a file
 *
 * @param fd: File descriptor to read a block from
 * @param blk: Filled with read data
 * @param sz: Size of block to read from file
 * @return: Negative number on error, read size otherwise. The only reason that
 * return value could be different from sz on success is when EOF has been
 * encountered while reading the file.
 */
static ssize_t gi_fip_read_blk(int fd, uint8_t *blk, size_t sz)
{
	size_t i;
	ssize_t nr = 1;

	for(i = 0; (i < sz) && (nr != 0); i += nr) {
		nr = read(fd, blk + i, sz - i);
		if(nr < 0)
			goto out;
	}
	nr = i;
out:
	return nr;
}

/**
 * Write a block of data into a file
 *
 * @param fd: File descriptor to write a block into
 * @param blk: Actual block data
 * @param sz: Size of block to write into file
 * @return: Negative number on error, sz otherwise.
 */
static ssize_t gi_fip_write_blk(int fd, uint8_t *blk, size_t sz)
{
	size_t i;
	ssize_t nr;

	for(i = 0; i < sz; i += nr) {
		nr = write(fd, blk + i, sz - i);
		if(nr < 0)
			goto out;
	}
	nr = i;
out:
	return nr;
}

/**
 * List of supported boot image
 */
enum FIP_BOOT_IMG {
	FBI_BL2,
	FBI_BL30,
	FBI_BL31,
	FBI_BL32,
	FBI_BL33,
};

typedef uint8_t uuid_t[16];

/**
 * Default uuid for each boot image
 */
static uuid_t const uuid_list[] = {
	[FBI_BL2] = {
		0x5f, 0xf9, 0xec, 0x0b, 0x4d, 0x22, 0x3e, 0x4d,
		0xa5, 0x44, 0xc3, 0x9d, 0x81, 0xc7, 0x3f, 0x0a
	},
	[FBI_BL30] = {
		0x97, 0x66, 0xfd, 0x3d, 0x89, 0xbe, 0xe8, 0x49,
		0xae, 0x5d, 0x78, 0xa1, 0x40, 0x60, 0x82, 0x13
	},
	[FBI_BL31] = {
		0x47, 0xd4, 0x08, 0x6d, 0x4c, 0xfe, 0x98, 0x46,
		0x9b, 0x95, 0x29, 0x50, 0xcb, 0xbd, 0x5a, 0x00
	},
	[FBI_BL32] = {
		0x05, 0xd0, 0xe1, 0x89, 0x53, 0xdc, 0x13, 0x47,
		0x8d, 0x2b, 0x50, 0x0a, 0x4b, 0x7a, 0x3e, 0x38
	},
	[FBI_BL33] = {
		0xd6, 0xd0, 0xee, 0xa7, 0xfc, 0xea, 0xd5, 0x4b,
		0x97, 0x82, 0x99, 0x34, 0xf2, 0x34, 0xb6, 0xe4
	},
};

/**
 * FIP header structure
 */
#pragma pack(push, 1)
struct  fip_toc_header {
	/**
	 * FIP magic
	 */
	uint32_t name;
	/**
	 * Vendor specific number
	 */
	uint32_t serial_number;
	/**
	 * Flags, reserved for later use
	 */
	uint64_t flags;
};
#pragma pack(pop)
#define FT_NAME 0xaa640001
#define FT_SERIAL 0x12345678
#define FIP_TOC_HEADER							\
	&((struct fip_toc_header){					\
		.name = htole32(FT_NAME),				\
		.serial_number = htole32(FT_SERIAL)			\
	})

/**
 * Header for a FIP table of content entry
 */
#pragma pack(push, 1)
struct fip_toc_entry {
	/**
	 * uuid of the image entry
	 */
	uuid_t uuid;
	/**
	 * Offset of the image from the FIP base address
	 */
	uint64_t offset;
	/**
	 * Size of the FIP entry image
	 */
	uint64_t size;
	/**
	 * Flags for the FIP entry image
	 */
	uint64_t flags;
};
#pragma pack(pop)

#define FTE_BL31HDR_SZ 0x50
#define FTE_BL31HDR_OFF(nr) (0x430 + FTE_BL31HDR_SZ * (nr))
/* Get fip entry offset from fip base */
#define FTE_OFF(nr)							\
	(sizeof(struct fip_toc_header) + nr * sizeof(struct fip_toc_entry))

#define FIP_SZ 0x4000
#define FIP_TEMPPATH "/tmp/fip.bin.XXXXXX"
#define FIP_TEMPPATHSZ sizeof(FIP_TEMPPATH)
/**
 * FIP handler
 */
struct fip {
	/**
	 * Current image copied data size
	 */
	size_t cursz;
	/**
	 * Number of entry in FIP table of content
	 */
	size_t nrentries;
	/**
	 * Temporary Fip file descriptor
	 */
	int fd;
	/**
	 * Temporary file path
	 */
	char path[FIP_TEMPPATHSZ];
};

/**
 * Init a FIP handler
 *
 * @param fip: FIP handler to init
 * @return: 0 on success, negative number otherwise
 */
static inline int fip_init(struct fip *fip)
{
	unsigned long long init = 0xffffffffffffffffULL;
	ssize_t nr;
	size_t i;
	int ret;

	strncpy(fip->path, FIP_TEMPPATH, sizeof(fip->path));
	fip->cursz = FIP_SZ;
	fip->nrentries = 0;
	fip->fd = mkstemp(fip->path);
	if(fip->fd < 0) {
		PERR("Cannot create fip temp: ");
		return -errno;
	}

	ret = ftruncate(fip->fd, FIP_SZ - 0x200);
	if(ret < 0) {
		PERR("Cannot truncate fip toc header: ");
		close(fip->fd);
		return -errno;
	}

	nr = gi_fip_write_blk(fip->fd, (uint8_t *)FIP_TOC_HEADER,
			sizeof(FIP_TOC_HEADER));
	if(nr < 0) {
		PERR("Cannot write fip toc header: ");
		close(fip->fd);
		return -errno;
	}

	/* End of toc entry */
	lseek(fip->fd, 0xc00, SEEK_SET);
	for(i = 0; i < 0x80 / sizeof(init); ++i) {
		nr = gi_fip_write_blk(fip->fd, (uint8_t *)&init, sizeof(init));
		if(nr < 0) {
			PERR("Cannot write fip toc last entry: ");
			close(fip->fd);
			return -errno;
		}
	}
	return 0;
}

/**
 * Cleanup a FIP handler
 *
 * @param fip: FIP handler to clean
 */
static inline void fip_cleanup(struct fip *fip)
{
	close(fip->fd);
	remove(fip->path);
}

/**
 * Copy a file as-is in another file at specific offset
 *
 * @param fdin: Src file to copy
 * @param fdout: Dest file to copy into
 * @param off: Offset at which src file should be copy into dest file
 * @return: 0 on success, negative number otherwise
 */
static int gi_fip_dump_img(int fdin, int fdout, size_t off)
{
	ssize_t nrd, nwr;
	int ret;
	uint8_t block[512];

	lseek(fdout, off, SEEK_SET);

	do {
		nrd = gi_fip_read_blk(fdin, block, sizeof(block));
		if(nrd < 0)
			continue;
		nwr = gi_fip_write_blk(fdout, block, nrd);
		if(nwr < 0) {
			PERR("Cannot write to file\n");
			ret = -errno;
			goto out;
		}
	} while(nrd > 0);

	if(nrd < 0) {
		PERR("Cannot read file\n");
		ret = -errno;
		goto out;
	}
	ret = 0;
out:
	return ret;
}

/**
 * Add a bootloder image in boot image
 *
 * @param fip: Fip handler
 * @param fdout: Final boot image file
 * @param fdin: Bootloader image to add
 * @param type: Type of bootloader image
 */
static int gi_fip_add(struct fip *fip, int fdout, int fdin,
		enum FIP_BOOT_IMG type)
{
	static uint32_t const bl31magic[] = {
		BL31_ENTRY_MAGIC,
		0x1,
	};
	struct fip_toc_entry entry;
	size_t sz;
	ssize_t nr;
	int ret;
	uint8_t buf[FTE_BL31HDR_SZ];

	sz = lseek(fdin, 0, SEEK_END);
	memcpy(entry.uuid, uuid_list[type], sizeof(entry.uuid));
	entry.offset = fip->cursz;
	entry.flags = 0;
	entry.size = sz;

	lseek(fip->fd, FTE_OFF(fip->nrentries), SEEK_SET);
	nr = gi_fip_write_blk(fip->fd, (uint8_t *)&entry, sizeof(entry));
	if(nr < 0) {
		PERR("Cannot write FIP entry\n");
		ret = -errno;
		goto out;
	}

	lseek(fdin, 256, SEEK_SET);
	nr = gi_fip_read_blk(fdin, buf, sizeof(buf));
	if(nr <= 0) {
		PERR("Cannot read BL image entry\n");
		ret = -errno;
		goto out;
	}

	if(le32toh(*(uint32_t *)buf) == BL31_MAGIC) {
		lseek(fip->fd, 1024, SEEK_SET);
		nr = gi_fip_write_blk(fip->fd, (uint8_t *)&bl31magic,
				sizeof(bl31magic));
		if(nr < 0) {
			PERR("Cannot write BL31 entry header\n");
			ret = -errno;
			goto out;
		}
		lseek(fip->fd, FTE_BL31HDR_OFF(fip->nrentries), SEEK_SET);
		nr = gi_fip_write_blk(fip->fd, buf, sizeof(buf));
		if(nr < 0) {
			PERR("Cannot write BL31 entry header data\n");
			ret = -errno;
			goto out;
		}
	}

	lseek(fdin, 0, SEEK_SET);
	gi_fip_dump_img(fdin, fdout, BL2SZ + entry.offset);
	fip->cursz += ROUNDUP(sz, 0x4000);
	++fip->nrentries;
	ret = 0;

out:
	return ret;
}

/**
 * Create a Amlogic bootable image
 *
 * @param bl2: BL2 boot image to add
 * @param bl30: BL30 boot image to add
 * @param bl31: BL31 boot image to add
 * @param bl33: BL33 boot image to add
 * @return: 0 on success, negative number otherwise
 */
int gi_fip_create(char const *bl2, char const *bl30, char const *bl31,
		char const *bl33, char const *fout)
{
	struct fip fip;
	struct amlcblk acb;
	struct {
		char const *path;
		enum FIP_BOOT_IMG type;
	} fip_bin_path[] = {
		{
			.path = bl30,
			.type = FBI_BL30,
		},
		{
			.path = bl31,
			.type = FBI_BL31,
		},
		{
			.path = bl33,
			.type = FBI_BL33,
		},
	};
	size_t i;
	int fdin = -1, fdout = -1, tmpfd = -1, ret;
	char fippath[] = "/tmp/fip.enc.XXXXXX";

	DBG("Create FIP final image in %s\n", fout);

	ret = fip_init(&fip);
	if(ret < 0)
		goto exit;

	fdout = open(fout, O_RDWR | O_CREAT, FOUT_MODE_DFT);
	if(fdout < 0) {
		PERR("Cannot open file %s", fout);
		ret = -errno;
		goto out;
	}

	ret = ftruncate(fdout, 0);
	if(ret < 0)
		goto out;

	fdin = open(bl2, O_RDONLY);
	if(fdin < 0) {
		PERR("Cannot open bl2 %s", bl2);
		ret = -errno;
		goto out;
	}

	ret = gi_fip_dump_img(fdin, fdout, 0);
	if(ret < 0)
		goto out;

	/* Add all BL3* images */
	for(i = 0; i < ARRAY_SIZE(fip_bin_path); ++i) {
		close(fdin);
		fdin = open(fip_bin_path[i].path, O_RDONLY);
		if(fdin < 0) {
			PERR("Cannot open bl %s: ", fip_bin_path[i].path);
			ret = -errno;
			goto out;
		}
		ret = gi_fip_add(&fip, fdout, fdin, fip_bin_path[i].type);
		if(ret < 0)
			goto out;
	}

	tmpfd = mkstemp(fippath);
	if(tmpfd < 0)
		goto out;

	ret = gi_amlcblk_init(&acb, fip.fd);
	if(ret < 0)
		goto out;

	ret = gi_amlcblk_aes_enc(&acb, tmpfd, fip.fd);
	if(ret < 0)
		goto out;

	ret = gi_amlcblk_dump_hdr(&acb, tmpfd);
	if(ret < 0)
		goto out;

	lseek(tmpfd, 0, SEEK_SET);
	ret = gi_fip_dump_img(tmpfd, fdout, BL2SZ);
out:
	if(tmpfd >= 0) {
		close(tmpfd);
		remove(fippath);
	}
	if(fdout >= 0)
		close(fdout);
	if(fdin >= 0)
		close(fdin);
	fip_cleanup(&fip);
exit:
	return ret;
}
