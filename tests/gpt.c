// SPDX-License-Identifier: GPL-2.0-or-later

#define _GNU_SOURCE
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h> /* BLKGETSIZE64, BLKSSZGET */
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h> /* memfd_create */
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <endian.h>

#include "gpt.h"    /* gpt_header, gpt_entry, legacy_mbr, read_gpt_pt */
#include "crc32.h"  /* crc32() macro -> crc32_le() */
#include "kpartx.h" /* struct slice */
#include "dos.h"    /* struct partition (needed by gpt.h -> legacy_mbr) */
#include "efi.h"    /* efi_guid_t */
#include "wrap64.h"

/* ------------------------------------------------------------------ */
/* External symbols required by gpt.o                                  */
/* ------------------------------------------------------------------ */

int force_gpt = 1;

#define SECTOR_SIZE 512U
int get_sector_size(int fd __attribute__((unused)))
{
	return SECTOR_SIZE;
}

int aligned_malloc(void **mem_p, size_t align, size_t *size_p)
{
	size_t sz;

	if (!mem_p || !align || (size_p && !*size_p))
		return EINVAL;
	if (size_p)
		sz = ((*size_p + align - 1) / align) * align;
	else
		sz = (size_t)getpagesize();
	if (posix_memalign(mem_p, (size_t)getpagesize(), sz))
		return ENOMEM;
	if (size_p)
		*size_p = sz;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Block-device simulation                                             */
/* ------------------------------------------------------------------ */

static uint64_t test_disk_size_bytes;

int REAL_FSTAT_FUNC(int v, int fd, struct stat *buf);
int WRAP_FSTAT_FUNC(int v, int fd, struct stat *buf)
{
	int rc = REAL_FSTAT_FUNC(v, fd, buf);

	if (!rc && test_disk_size_bytes)
		buf->st_mode = (buf->st_mode & ~S_IFMT) | S_IFBLK;
	return rc;
}

int REAL_IOCTL(int fd, unsigned long req, void *p);
int WRAP_IOCTL(int fd, unsigned long req, void *p)
{
	if (test_disk_size_bytes) {
		if (req == BLKGETSIZE64) {
			*(uint64_t *)p = test_disk_size_bytes;
			return 0;
		}
		if (req == BLKSSZGET) {
			*(int *)p = SECTOR_SIZE;
			return 0;
		}
	}
	return REAL_IOCTL(fd, req, p);
}

/* ------------------------------------------------------------------ */
/* CRC helpers                                                          */
/* ------------------------------------------------------------------ */

static uint32_t efi_crc32(const void *buf, size_t len)
{
	return crc32(~0L, buf, len) ^ ~0L;
}

#define NS 256

/* ------------------------------------------------------------------ */
/* Disk image builder                                                   */
/* ------------------------------------------------------------------ */

#define DISK_SECTORS   2048U
#define DISK_BYTES     ((size_t)DISK_SECTORS * SECTOR_SIZE)
#define PE_SIZE        128U   /* sizeof(gpt_entry) */

/* Flags for make_gpt_fd */
#define FLAG_BAD_PRIMARY_HDR_CRC  (1U << 0)
#define FLAG_BAD_ALT_HDR_CRC      (1U << 1)
#define FLAG_BAD_PTE_CRC          (1U << 2)
#define FLAG_BAD_SIGNATURE        (1U << 3)
#define FLAG_WRONG_SIZEOF_PE      (1U << 4)
#define FLAG_VALID_PMBR           (1U << 5)

/* Linux data partition type GUID */
static const efi_guid_t linux_data_guid =
	EFI_GUID(0x0FC63DAF, 0x8483, 0x4772, 0x8E, 0x79, 0x3D, 0x69, 0xD8,
		 0x47, 0x7D, 0xE4);

/*
 * Build an in-memory GPT disk image and return an open memfd.
 *
 * Disk layout (DISK_SECTORS = 2048, sector_size = 512):
 *   LBA 0             Protective MBR
 *   LBA 1             Primary GPT header
 *   LBA 2..2+pe-1     Primary partition entry array (pe sectors)
 *   LBA L-pe..L-1     Backup partition entry array
 *   LBA L             Backup GPT header   (L = DISK_SECTORS-1 = 2047)
 *
 * For very large num_entries (overflow test), pe is capped at 64.
 */
static int make_gpt_fd(uint32_t num_entries, unsigned int num_parts,
		       const struct slice *parts, unsigned int flags)
{
	uint8_t *disk;
	uint64_t pe_bytes = (uint64_t)num_entries * PE_SIZE;
	uint32_t pe_sectors;
	uint64_t primary_pe_lba, backup_hdr_lba, backup_pe_lba;
	uint64_t first_usable, last_usable;
	gpt_entry *pte;
	uint32_t pte_crc, hdr_pte_crc;
	gpt_header *phdr, *ahdr;
	uint32_t pcrc, acrc;
	int fd;
	ssize_t written;

	/* Cap pe_sectors for very large num_entries to keep disk valid */
	if (pe_bytes > (uint64_t)(DISK_SECTORS - 4) * SECTOR_SIZE)
		pe_sectors = 64; /* enough for 256 entries */
	else
		pe_sectors = (uint32_t)((pe_bytes + SECTOR_SIZE - 1) / SECTOR_SIZE);

	primary_pe_lba = 2;
	backup_hdr_lba = DISK_SECTORS - 1;	     /* LBA 2047 */
	backup_pe_lba = backup_hdr_lba - pe_sectors; /* varies */
	first_usable = primary_pe_lba + pe_sectors;
	last_usable = backup_pe_lba > 0 ? backup_pe_lba - 1 : 0;

	disk = calloc(DISK_BYTES, 1);
	if (!disk)
		return -1;

	/* --- PMBR (LBA 0) --- */
	if (flags & FLAG_VALID_PMBR) {
		legacy_mbr *pmbr = (legacy_mbr *)disk;

		pmbr->signature = htole16(MSDOS_MBR_SIGNATURE);
		pmbr->partition[0].sys_type = EFI_PMBR_OSTYPE_EFI_GPT;
	}

	/* --- Primary partition entry array (LBA 2) --- */
	pte = (gpt_entry *)(disk + primary_pe_lba * SECTOR_SIZE);
	for (unsigned int i = 0; i < num_parts; i++) {
		pte[i].partition_type_guid = linux_data_guid;
		memset(&pte[i].unique_partition_guid, (int)(i + 1), 16);
		pte[i].starting_lba = htole64(parts[i].start);
		pte[i].ending_lba = htole64(parts[i].start + parts[i].size - 1);
	}

	/*
	 * Compute PTE CRC matching alloc_read_gpt_entries().
	 * In the overflow case, compute CRC over one entry,
	 * causing test failure in test_invalid_overflow_num_entries_1
	 * if the overflow test is not correct.
	 */
	if (num_entries == 0 || pe_bytes > (uint64_t)(DISK_SECTORS - 4) * SECTOR_SIZE)
		pte_crc = efi_crc32(disk + primary_pe_lba * SECTOR_SIZE,
				    sizeof(gpt_entry));
	else
		pte_crc = efi_crc32(disk + primary_pe_lba * SECTOR_SIZE,
				    num_entries * sizeof(gpt_entry));

	hdr_pte_crc = (flags & FLAG_BAD_PTE_CRC) ? (pte_crc ^ 1) : pte_crc;

	/* --- Primary GPT header (LBA 1) --- */
	phdr = (gpt_header *)(disk + 1 * SECTOR_SIZE);
	phdr->signature = htole64(GPT_HEADER_SIGNATURE);
	phdr->revision = htole32(GPT_HEADER_REVISION_V1_00);
	phdr->header_size = htole32(92);
	phdr->my_lba = htole64(1);
	phdr->alternate_lba = htole64(backup_hdr_lba);
	phdr->first_usable_lba = htole64(first_usable);
	phdr->last_usable_lba = htole64(last_usable);
	phdr->partition_entry_lba = htole64(primary_pe_lba);
	phdr->num_partition_entries = htole32(num_entries);
	phdr->sizeof_partition_entry =
		htole32((flags & FLAG_WRONG_SIZEOF_PE) ? 256 : PE_SIZE);
	phdr->partition_entry_array_crc32 = htole32(hdr_pte_crc);

	if (flags & FLAG_BAD_SIGNATURE)
		phdr->signature = 0;

	phdr->header_crc32 = 0;
	pcrc = efi_crc32(phdr, 92);
	phdr->header_crc32 = htole32(pcrc);
	if (flags & FLAG_BAD_PRIMARY_HDR_CRC)
		phdr->header_crc32 ^= htole32(0xFF);

	/* --- Copy primary PE to backup PE location --- */
	if (pe_sectors > 0 &&
	    (backup_pe_lba + pe_sectors) * SECTOR_SIZE <= DISK_BYTES) {
		memcpy(disk + backup_pe_lba * SECTOR_SIZE,
		       disk + primary_pe_lba * SECTOR_SIZE,
		       pe_sectors * SECTOR_SIZE);
	}

	/* --- Backup GPT header (LBA 2047) --- */
	ahdr = (gpt_header *)(disk + backup_hdr_lba * SECTOR_SIZE);
	*ahdr = *phdr; /* inherit all flags applied to primary */
	ahdr->header_crc32 = 0;
	ahdr->my_lba = htole64(backup_hdr_lba);
	ahdr->alternate_lba = htole64(1);
	ahdr->partition_entry_lba = htole64(backup_pe_lba);

	acrc = efi_crc32(ahdr, 92);
	ahdr->header_crc32 = htole32(acrc);
	if (flags & FLAG_BAD_ALT_HDR_CRC)
		ahdr->header_crc32 ^= htole32(0xFF);

	/* --- Write to memfd --- */
	fd = memfd_create("gpt-test", 0);
	if (fd < 0) {
		free(disk);
		return -1;
	}

	written = write(fd, disk, DISK_BYTES);
	free(disk);

	if (written != (ssize_t)DISK_BYTES) {
		close(fd);
		return -1;
	}
	if (lseek(fd, 0, SEEK_SET) < 0) {
		close(fd);
		return -1;
	}

	test_disk_size_bytes = DISK_BYTES;
	return fd;
}

/* ------------------------------------------------------------------ */
/* Partition data arrays                                                */
/* ------------------------------------------------------------------ */

static const struct slice parts3[] = {
	{ .start = 100, .size = 100 },
	{ .start = 200, .size = 100 },
	{ .start = 300, .size = 100 },
};

static const struct slice parts2[] = {
	{ .start = 100, .size = 100 },
	{ .start = 200, .size = 100 },
};

static const struct slice parts5[] = {
	{ .start = 100, .size = 50 },
	      { .start = 150, .size = 50 },
	{ .start = 200, .size = 50 },
	      { .start = 250, .size = 50 },
	{ .start = 300, .size = 50 },
};

/* ------------------------------------------------------------------ */
/* Individual test functions                                            */
/* ------------------------------------------------------------------ */

static void test_valid_gpt_128(void **state __attribute__((unused)))
{
	struct slice all = { 0 };
	struct slice sp[NS];
	int fd, ret;

	memset(sp, 0, sizeof(sp));
	fd = make_gpt_fd(128, 3, parts3, 0);
	assert_true(fd >= 0);

	ret = read_gpt_pt(fd, all, sp, NS);
	close(fd);
	test_disk_size_bytes = 0;

	assert_int_equal(ret, 3);
	assert_int_equal((int)sp[0].start, 100);
	assert_int_equal((int)sp[0].size, 100);
	assert_int_equal((int)sp[1].start, 200);
	assert_int_equal((int)sp[1].size, 100);
	assert_int_equal((int)sp[2].start, 300);
	assert_int_equal((int)sp[2].size, 100);
}

static void test_valid_gpt_no_partitions(void **state __attribute__((unused)))
{
	struct slice all = { 0 };
	struct slice sp[NS];
	int fd, ret;

	memset(sp, 0, sizeof(sp));
	fd = make_gpt_fd(128, 0, NULL, 0);
	assert_true(fd >= 0);

	ret = read_gpt_pt(fd, all, sp, NS);
	close(fd);
	test_disk_size_bytes = 0;

	assert_int_equal(ret, 0);
}

static void test_invalid_zero_declared(void **state __attribute__((unused)))
{
	struct slice all = { 0 };
	struct slice sp[NS];
	int fd, ret;

	memset(sp, 0, sizeof(sp));
	fd = make_gpt_fd(0, 0, NULL, 0);
	assert_true(fd >= 0);

	ret = read_gpt_pt(fd, all, sp, NS);
	close(fd);
	test_disk_size_bytes = 0;

	/* alloc_read_gpt_entries returns NULL for 0 entries -> failure */
	assert_true(ret <= 0);
}

static void test_invalid_bad_signature(void **state __attribute__((unused)))
{
	struct slice all = { 0 };
	struct slice sp[NS];
	int fd, ret;

	memset(sp, 0, sizeof(sp));
	fd = make_gpt_fd(128, 0, NULL, FLAG_BAD_SIGNATURE);
	assert_true(fd >= 0);

	ret = read_gpt_pt(fd, all, sp, NS);
	close(fd);
	test_disk_size_bytes = 0;

	assert_true(ret <= 0);
}

static void test_invalid_bad_header_crc(void **state __attribute__((unused)))
{
	struct slice all = { 0 };
	struct slice sp[NS];
	int fd, ret;

	memset(sp, 0, sizeof(sp));
	fd = make_gpt_fd(128, 0, NULL,
			 FLAG_BAD_PRIMARY_HDR_CRC | FLAG_BAD_ALT_HDR_CRC);
	assert_true(fd >= 0);

	ret = read_gpt_pt(fd, all, sp, NS);
	close(fd);
	test_disk_size_bytes = 0;

	assert_true(ret <= 0);
}

static void test_invalid_bad_pte_crc(void **state __attribute__((unused)))
{
	struct slice all = { 0 };
	struct slice sp[NS];
	int fd, ret;

	memset(sp, 0, sizeof(sp));
	fd = make_gpt_fd(128, 0, NULL, FLAG_BAD_PTE_CRC);
	assert_true(fd >= 0);

	ret = read_gpt_pt(fd, all, sp, NS);
	close(fd);
	test_disk_size_bytes = 0;

	assert_true(ret <= 0);
}

static void test_invalid_wrong_sizeof_pe(void **state __attribute__((unused)))
{
	struct slice all = { 0 };
	struct slice sp[NS];
	int fd, ret;

	memset(sp, 0, sizeof(sp));
	fd = make_gpt_fd(128, 0, NULL, FLAG_WRONG_SIZEOF_PE);
	assert_true(fd >= 0);

	ret = read_gpt_pt(fd, all, sp, NS);
	close(fd);
	test_disk_size_bytes = 0;

	/* is_gpt_valid rejects sizeof_partition_entry != PE_SIZE */
	assert_true(ret <= 0);
}

static void test_invalid_overflow_num_entries(void **state __attribute__((unused)))
{
	struct slice all = { 0 };
	struct slice sp[NS];
	int fd, ret;

	/*
	 * 0x02000000 * 128 = 0x100000000 (overflows uint32_t on 32-bit).
	 * On 64-bit: alloc_read_gpt_entries sets n_alloc=256, reads 64
	 * sectors, then the extra-entries loop tries to read beyond the disk
	 * and fails. The accumulated CRC will not match the placeholder stored
	 * in the header, so is_gpt_valid returns 0 for both primary and
	 * alternate.
	 */
	memset(sp, 0, sizeof(sp));
	fd = make_gpt_fd(0x02000000, 0, NULL, 0);
	assert_true(fd >= 0);

	ret = read_gpt_pt(fd, all, sp, NS);
	close(fd);
	test_disk_size_bytes = 0;

	assert_true(ret <= 0);
}

static void
test_invalid_overflow_num_entries_1(void **state __attribute__((unused)))
{
	struct slice all = { 0 };
	struct slice sp[NS];
	int fd, ret;

	memset(sp, 0, sizeof(sp));
	fd = make_gpt_fd(0x02000001, 0, NULL, 0);
	assert_true(fd >= 0);

	ret = read_gpt_pt(fd, all, sp, NS);
	close(fd);
	test_disk_size_bytes = 0;

	assert_true(ret <= 0);
}

static void test_fallback_to_alternate(void **state __attribute__((unused)))
{
	struct slice all = { 0 };
	struct slice sp[NS];
	int fd, ret;

	memset(sp, 0, sizeof(sp));
	/* Only primary header CRC is corrupted; alternate remains valid */
	fd = make_gpt_fd(128, 2, parts2, FLAG_BAD_PRIMARY_HDR_CRC);
	assert_true(fd >= 0);

	ret = read_gpt_pt(fd, all, sp, NS);
	close(fd);
	test_disk_size_bytes = 0;

	/* find_valid_gpt uses alternate GPT, returns 2 partitions */
	assert_int_equal(ret, 2);
	assert_int_equal((int)sp[0].start, 100);
	assert_int_equal((int)sp[0].size, 100);
	assert_int_equal((int)sp[1].start, 200);
	assert_int_equal((int)sp[1].size, 100);
}

static void test_gpt_1024_entries(void **state __attribute__((unused)))
{
	struct slice all = { 0 };
	struct slice sp[NS];
	int fd, ret;

	memset(sp, 0, sizeof(sp));
	fd = make_gpt_fd(1024, 3, parts3, 0);
	assert_true(fd >= 0);

	ret = read_gpt_pt(fd, all, sp, NS);
	close(fd);
	test_disk_size_bytes = 0;

	/* 1024 > NS=256: warning printed, but 3 slices returned */
	assert_int_equal(ret, 3);
	assert_int_equal((int)sp[0].start, 100);
	assert_int_equal((int)sp[0].size, 100);
	assert_int_equal((int)sp[1].start, 200);
	assert_int_equal((int)sp[1].size, 100);
	assert_int_equal((int)sp[2].start, 300);
	assert_int_equal((int)sp[2].size, 100);
}

static void test_pmbr_required(void **state __attribute__((unused)))
{
	struct slice all = { 0 };
	struct slice sp[NS];
	int fd, ret;

	force_gpt = 0;
	memset(sp, 0, sizeof(sp));
	/* No FLAG_VALID_PMBR: PMBR is all zeros, is_pmbr_valid() returns 0 */
	fd = make_gpt_fd(128, 0, NULL, 0);
	assert_true(fd >= 0);

	ret = read_gpt_pt(fd, all, sp, NS);
	close(fd);
	test_disk_size_bytes = 0;
	force_gpt = 1;

	/* force_gpt=0 and invalid PMBR -> find_valid_gpt fails */
	assert_true(ret <= 0);
}

static void test_pmbr_accepted(void **state __attribute__((unused)))
{
	struct slice all = { 0 };
	struct slice sp[NS];
	int fd, ret;

	force_gpt = 0;
	memset(sp, 0, sizeof(sp));
	fd = make_gpt_fd(128, 0, NULL, FLAG_VALID_PMBR);
	assert_true(fd >= 0);

	ret = read_gpt_pt(fd, all, sp, NS);
	close(fd);
	test_disk_size_bytes = 0;
	force_gpt = 1;

	/* force_gpt=0 but valid PMBR -> success (0 partitions) */
	assert_int_equal(ret, 0);
}

/*
 * 3 entries (< 4 = entries_per_sector): count_rounded = 512,
 * CRC covers the partial sector including zero padding.
 * Tests the "num_entries <= ns, count rounds up" path in
 * alloc_read_gpt_entries().
 */
static void test_valid_gpt_3_entries(void **state __attribute__((unused)))
{
	struct slice all = { 0 };
	struct slice sp[NS];
	int fd, ret;

	memset(sp, 0, sizeof(sp));
	fd = make_gpt_fd(3, 3, parts3, 0);
	assert_true(fd >= 0);

	ret = read_gpt_pt(fd, all, sp, NS);
	close(fd);
	test_disk_size_bytes = 0;

	assert_int_equal(ret, 3);
	assert_int_equal((int)sp[0].start, 100);
	assert_int_equal((int)sp[0].size, 100);
	assert_int_equal((int)sp[1].start, 200);
	assert_int_equal((int)sp[1].size, 100);
	assert_int_equal((int)sp[2].start, 300);
	assert_int_equal((int)sp[2].size, 100);
}

/*
 * 5 entries (5 % 4 != 0): count_rounded = 1024,
 * CRC covers two full sectors (5 entries + 3 zero entries of padding).
 */
static void test_valid_gpt_5_entries(void **state __attribute__((unused)))
{
	struct slice all = { 0 };
	struct slice sp[NS];
	int fd, ret;

	memset(sp, 0, sizeof(sp));
	fd = make_gpt_fd(5, 5, parts5, 0);
	assert_true(fd >= 0);

	ret = read_gpt_pt(fd, all, sp, NS);
	close(fd);
	test_disk_size_bytes = 0;

	assert_int_equal(ret, 5);
	assert_int_equal((int)sp[0].start, 100);
	assert_int_equal((int)sp[0].size, 50);
	assert_int_equal((int)sp[1].start, 150);
	assert_int_equal((int)sp[1].size, 50);
	assert_int_equal((int)sp[2].start, 200);
	assert_int_equal((int)sp[2].size, 50);
	assert_int_equal((int)sp[3].start, 250);
	assert_int_equal((int)sp[3].size, 50);
	assert_int_equal((int)sp[4].start, 300);
	assert_int_equal((int)sp[4].size, 50);
}

/*
 * 257 entries (257 % 4 != 0): triggers the extra-entries path in
 * alloc_read_gpt_entries() with a partial final sector (1 entry,
 * CRC over 128 bytes instead of 512).
 */
static void test_gpt_257_entries(void **state __attribute__((unused)))
{
	struct slice all = { 0 };
	struct slice sp[NS];
	int fd, ret;

	memset(sp, 0, sizeof(sp));
	fd = make_gpt_fd(257, 3, parts3, 0);
	assert_true(fd >= 0);

	ret = read_gpt_pt(fd, all, sp, NS);
	close(fd);
	test_disk_size_bytes = 0;

	assert_int_equal(ret, 3);
	assert_int_equal((int)sp[0].start, 100);
	assert_int_equal((int)sp[0].size, 100);
	assert_int_equal((int)sp[1].start, 200);
	assert_int_equal((int)sp[1].size, 100);
	assert_int_equal((int)sp[2].start, 300);
	assert_int_equal((int)sp[2].size, 100);
}

/*
 * 1025 entries (1025 % 4 != 0): extra-entries path with partial last sector
 * (1 entry = 128 bytes) at the very end of a 256-sector PE block.
 */
static void test_gpt_1025_entries(void **state __attribute__((unused)))
{
	struct slice all = { 0 };
	struct slice sp[NS];
	int fd, ret;

	memset(sp, 0, sizeof(sp));
	fd = make_gpt_fd(1025, 3, parts3, 0);
	assert_true(fd >= 0);

	ret = read_gpt_pt(fd, all, sp, NS);
	close(fd);
	test_disk_size_bytes = 0;

	assert_int_equal(ret, 3);
	assert_int_equal((int)sp[0].start, 100);
	assert_int_equal((int)sp[0].size, 100);
	assert_int_equal((int)sp[1].start, 200);
	assert_int_equal((int)sp[1].size, 100);
	assert_int_equal((int)sp[2].start, 300);
	assert_int_equal((int)sp[2].size, 100);
}

/* ------------------------------------------------------------------ */
/* Group setup/teardown                                                 */
/* ------------------------------------------------------------------ */

static int group_setup(void **state __attribute__((unused)))
{
	return init_crc32() ? -1 : 0;
}

static int group_teardown(void **state __attribute__((unused)))
{
	cleanup_crc32();
	return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_valid_gpt_128),
		cmocka_unit_test(test_valid_gpt_no_partitions),
		cmocka_unit_test(test_invalid_zero_declared),
		cmocka_unit_test(test_invalid_bad_signature),
		cmocka_unit_test(test_invalid_bad_header_crc),
		cmocka_unit_test(test_invalid_bad_pte_crc),
		cmocka_unit_test(test_invalid_wrong_sizeof_pe),
		cmocka_unit_test(test_invalid_overflow_num_entries),
		cmocka_unit_test(test_invalid_overflow_num_entries_1),
		cmocka_unit_test(test_fallback_to_alternate),
		cmocka_unit_test(test_gpt_1024_entries),
		cmocka_unit_test(test_pmbr_required),
		cmocka_unit_test(test_pmbr_accepted),
		cmocka_unit_test(test_valid_gpt_3_entries),
		cmocka_unit_test(test_valid_gpt_5_entries),
		cmocka_unit_test(test_gpt_257_entries),
		cmocka_unit_test(test_gpt_1025_entries),
	};
	return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
