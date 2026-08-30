// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/module.h>
#include <linux/scatterlist.h>
#include <crypto/aead.h>
#include <soc/qcom/qcom_hibernation.h>
#include <soc/qcom/qcom_hibernation.h>	/* for QCOM_CRYPTO_PARAMS_VERSION */
#include <../../../kernel/power/power.h>
#include <misc/qseecom_kernel.h>
#include <trace/hooks/bl_hib.h>
#include <linux/reboot.h>
#include <soc/qcom/smci_clientenv.h>
#include <soc/qcom/smci_low_power_key_mgr.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/crypto.h>
#include <crypto/hash.h>
#include <linux/slab.h>
#include <linux/jhash.h>


#define SHA1_DIGEST_SIZE 20
#define MAX_BIO_AUTHPAGES	200
#define ILOWPOWERKEYMANAGER_ERROR_INVALID_OPERATION_CHECK 11
#define AUTH_SIZE		16
#define AUTH_TAG		0xFF
#define QSEECOM_ALIGN_SIZE      0x40
#define QSEECOM_ALIGN_MASK      (QSEECOM_ALIGN_SIZE - 1)
#define QSEECOM_ALIGN(x)        \
	((x + QSEECOM_ALIGN_MASK) & (~QSEECOM_ALIGN_MASK))

typedef __u32 __bitwise blk_opf_t;

spinlock_t authtag_lock;
spinlock_t iv_lock;

static bool gethibkey_value = false;
static struct kobject *gethibkey_kobj;
static struct smci_object client_env = {0};
static struct smci_object key_mgr_object = {0};


struct auth_entry {
	uint8_t sha1[SHA1_DIGEST_SIZE];
	uint8_t auth_tag[AUTH_SIZE];
	uint8_t iv[IV_SIZE];
	struct hlist_node node;
};

DEFINE_HASHTABLE(auth_map, 20);

struct s4app_time {
	uint16_t year;
	uint8_t month;
	uint8_t day;
	uint8_t hour;
	uint8_t minute;
};

struct wrap_req {
	struct s4app_time save_time;
};

struct wrap_rsp {
	uint8_t wrapped_key_buffer[WRAPPED_KEY_SIZE];
	uint32_t wrapped_key_size;
	uint8_t key_buffer[PAYLOAD_KEY_SIZE];
	uint32_t key_size;
};

struct unwrap_req {
	uint8_t wrapped_key_buffer[WRAPPED_KEY_SIZE];
	uint32_t wrapped_key_size;
	struct s4app_time curr_time;
};

struct unwrap_rsp {
	uint8_t key_buffer[PAYLOAD_KEY_SIZE];
	uint32_t key_size;
};

enum cmd_id {
	WRAP_KEY_CMD = 0,
	UNWRAP_KEY_CMD = 1,
};

struct cmd_req {
	enum cmd_id cmd;
	union {
		struct wrap_req wrapkey_req;
		struct unwrap_req unwrapkey_req;
	};
};

struct cmd_rsp {
	enum cmd_id cmd;
	union {
		struct wrap_rsp wrapkey_rsp;
		struct unwrap_rsp unwrapkey_rsp;
	};
	uint32_t status;
};

sector_t base_offset;
sector_t pre_offset;
static struct auth_entry *entry;
static int save_hashmap = 0;
static struct qcom_crypto_params *params, *decrypt_params;
static struct crypto_aead *tfm;
static struct aead_request *req;
static u8 iv_size;
static bool tfm_configured;
static uint8_t *decrypt_authtags;
static int decrypt_auth_idx;
static u8 key[AES256_KEY_SIZE];
#ifndef CONFIG_QCOM_KERNEL_SEC_KEY
static struct qseecom_handle *app_handle;
#endif
static int first_encrypt, first_decrypt;
static void *temp_out_buf;
static int pos;
static uint8_t *authslot_start;
static unsigned short root_swap_dev;
static struct work_struct save_params_work;
static struct completion write_done;
static unsigned char iv[IV_SIZE];
static uint8_t *compressed_blk_array;
static int blk_array_pos;
static unsigned long nr_pages;
static void *auth_slot;
static int auth_slot_offset;
void get_authtag(int index, uint8_t *tag_out);
static int success_pages=0, fail_pages=0;

#ifndef CONFIG_QCOM_KERNEL_SEC_KEY
static void generate_random_key(void)
{
	get_random_bytes(key, AES256_KEY_SIZE);
}
#endif

int compute_sha1(const void *data, size_t len, uint8_t *out)
{
	struct crypto_shash *tfm_sha;
	struct shash_desc *desc;
	int ret;

	tfm_sha = crypto_alloc_shash("sha1", 0, 0);
	if (IS_ERR(tfm_sha))
		return PTR_ERR(tfm_sha);

	desc = kmalloc(sizeof(*desc) + crypto_shash_descsize(tfm_sha), GFP_KERNEL);
	if (!desc) {
		crypto_free_shash(tfm_sha);
		return -ENOMEM;
	}

	desc->tfm = tfm_sha;

	ret = crypto_shash_digest(desc, data, len, out);

	kfree(desc);
	crypto_free_shash(tfm_sha);
	return ret;
}

static void init_sg(struct scatterlist *sg, void *data, unsigned int size)
{
	sg_init_table(sg, 2);
	sg_set_buf(&sg[0], params->aad, sizeof(params->aad));
	sg_set_buf(&sg[1], data, size);
}

static void save_auth(uint8_t *out_buf, unsigned int sec_pos)
{

	uint8_t sha1_digest[SHA1_DIGEST_SIZE];
	uint32_t pos_offset;

	if (save_hashmap) {
		pos_offset = pos * (AUTH_SIZE + SHA1_DIGEST_SIZE + IV_SIZE);
	} else {
		pos_offset = sec_pos * AUTH_SIZE;
	}

	if (!save_hashmap && params && sec_pos >= params->authslot_count)
		return;

	memcpy(authslot_start + pos_offset, out_buf + PAGE_SIZE, AUTH_SIZE);
	if (save_hashmap) {
		compute_sha1(out_buf, PAGE_SIZE, sha1_digest);
		memcpy(authslot_start + pos_offset + AUTH_SIZE, sha1_digest, SHA1_DIGEST_SIZE);
		memcpy(authslot_start + pos_offset + AUTH_SIZE + SHA1_DIGEST_SIZE, iv, IV_SIZE);
	}

	pos++;
}

static void skip_swap_map_write(void *data, bool *skip)
{
	*skip = false;
}

static void store_auth_slot_num(void *data, uint32_t *auth_slot_num)
{
	*auth_slot_num = (uint32_t) auth_slot_offset;
}

void increment_iv(uint8_t *iv, uint8_t size, uint64_t val)
{
	int i = size - 1;
	uint64_t num;
	uint64_t mask = 0xFF;

	while (i >= 0 && val != 0) {
		num = (uint64_t)iv[i];
		num += val;
		iv[i] = (uint8_t)(num & mask);
		val = (num > mask) ? ((num & ~mask) >> 8) : 0;
		i--;
	}
}

static void encrypt_page(void *data, void *buf, sector_t offset)
{
	struct scatterlist sg_in[2], sg_out[2];
	struct crypto_wait wait;
	int ret = 0;

	offset = offset / 8;
	/* Allocate a request object */
	req = aead_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		ret = -ENOMEM;
		goto err_aead;
	}

	crypto_init_wait(&wait);
	aead_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				crypto_req_done, &wait);

	ret = crypto_aead_setauthsize(tfm, AUTH_SIZE);
	iv_size = crypto_aead_ivsize(tfm);
	if (iv_size && first_encrypt) {
		get_random_bytes(params->iv, iv_size);
		memcpy((void *)iv, params->iv, IV_SIZE);
		base_offset = offset;
		pre_offset = offset;
	}

	ret = crypto_aead_setkey(tfm, key, AES256_KEY_SIZE);
	if (ret) {
		pr_err("Error setting key: %d\n", ret);
		goto out;
	}
	crypto_aead_clear_flags(tfm, ~0);

	memset(temp_out_buf, 0, 2 * PAGE_SIZE);
	init_sg(sg_in, buf, PAGE_SIZE);
	init_sg(sg_out, temp_out_buf, PAGE_SIZE + AUTH_SIZE);
	aead_request_set_ad(req, sizeof(params->aad));

	/*
	 * Dynamic IV:
	 * Treat params->iv as the IV for the first encrypted page (base_offset),
	 * and derive subsequent IVs as: IV = base_iv + (offset - base_offset).
	 *
	 * Fast-path uses deltas for increasing offsets; recompute for backwards
	 * offsets.
	 */
	if (offset >= pre_offset) {
		increment_iv(iv, IV_SIZE, offset - pre_offset);
	} else {
		memcpy((void *)iv, params->iv, IV_SIZE);
		increment_iv(iv, IV_SIZE, offset - base_offset);
	}

	aead_request_set_crypt(req, sg_in, sg_out, PAGE_SIZE, iv);
	ret = crypto_aead_encrypt(req);
	if (ret)
		pr_err("Failed in encrypting page:%d with ret%d..\n", pos, ret);

	ret = crypto_wait_req(ret, &wait);
	if (ret) {
		pr_err("Error encrypting data: %d\n", ret);
		goto out;
	}

	pre_offset = offset;
	memcpy(buf, temp_out_buf, PAGE_SIZE);
	save_auth(temp_out_buf, offset - base_offset);

	if (first_encrypt)
		first_encrypt = 0;

out:
	aead_request_free(req);
	return;
err_aead:
	free_pages((unsigned long)temp_out_buf, 1);

}

static int read_swap_page(sector_t sector, void *buffer)
{
	struct bio *bio;
	struct page *page = virt_to_page(buffer);
	int ret = 0;

	bio = bio_alloc(GFP_NOIO | __GFP_HIGH, 1);
	if (!bio)
		return -ENOMEM;

	bio_set_dev(bio, hib_resume_bdev);
	bio->bi_iter.bi_sector = sector;
	bio_set_op_attrs(bio, REQ_OP_READ, 0);

	if (bio_add_page(bio, page, PAGE_SIZE, 0) < PAGE_SIZE) {
		bio_put(bio);
		return -EIO;
	}

	submit_bio_wait(bio);
	if (bio->bi_status) {
		pr_err("BIO read failed at sector %llu\n", (unsigned long long)sector);
		ret = -EIO;
	}

	bio_put(bio);
	return ret;
}

int read_auth_params(sector_t sector, int nr_pages, void *buffer)
{
	int i, ret = 0;
	int pages_remaining = nr_pages;
	int page_offset = 0;

	while (pages_remaining > 0) {
		int pages_to_read = min(pages_remaining, BIO_MAX_PAGES);
		struct bio *bio = bio_alloc(GFP_NOIO | __GFP_HIGH, pages_to_read);
		if (!bio)
			return -ENOMEM;

		bio_set_dev(bio, hib_resume_bdev);
		bio->bi_iter.bi_sector = sector;
		bio_set_op_attrs(bio, REQ_OP_READ, 0);

		for (i = 0; i < pages_to_read; i++) {
			void *page_ptr = buffer + (page_offset + i) * PAGE_SIZE;
			struct page *page = vmalloc_to_page(page_ptr);

			if (!page) {
				pr_err("vmalloc_to_page failed for offset %d\n", page_offset + i);
				bio_put(bio);
				return -EFAULT;
			}

			if (bio_add_page(bio, page, PAGE_SIZE, offset_in_page(page_ptr)) < PAGE_SIZE) {
				pr_err("bio_add_page failed at offset %d\n", page_offset + i);
				bio_put(bio);
				return -EIO;
			}
		}

		submit_bio_wait(bio);
		if (bio->bi_status) {
			pr_err("BIO read failed at sector %llu\n", (unsigned long long)sector);
			ret = -EIO;
		}

		bio_put(bio);

		sector += (pages_to_read * (PAGE_SIZE >> 9));
		page_offset += pages_to_read;
		pages_remaining -= pages_to_read;
	}

	return ret;
}


static void hib_init_batch(struct hib_bio_batch *hb)
{
	atomic_set(&hb->count, 0);
	init_waitqueue_head(&hb->wait);
	hb->error = BLK_STS_OK;
	blk_start_plug(&hb->plug);
}

int init_aes_decrypt(void)
{

	int authslot_base, params_slot, ret;
	unsigned long total_size, offset;
	unsigned int num_pages;
	unsigned int sha_val;
	sector_t params_slot_sector, authtag_slot_sector;
	uint8_t *decrypt_params_buf;

	authslot_base = (int) swap_auth_slot_offset;
	if (!authslot_base) {
		if (auth_slot_offset)
			authslot_base = auth_slot_offset;
		else
			return -EINVAL;
	}

	params_slot = authslot_base - 1;
	params_slot_sector =  params_slot * (PAGE_SIZE >> 9);
	pr_info("%s: authslot_base: %d, params_slot: %d", __func__, authslot_base, params_slot);

	decrypt_params_buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!decrypt_params_buf)
		return -ENOMEM;
	ret = read_swap_page(params_slot_sector, decrypt_params_buf);
	if (ret) {
		pr_err("Failed to read crypto params from swap\n");
		return ret;
	}
	decrypt_params = (struct qcom_crypto_params *)decrypt_params_buf;
	/* verify params version for backward compatibility */
	if (decrypt_params->version != QCOM_CRYPTO_PARAMS_VERSION) {
		pr_err("qcom_secure_hibernation: unsupported params version %u\n",
		       decrypt_params->version);
		return -EINVAL;
	}
	pr_info("Read crypto params: authsize=%u, authslot_count=%u\n",
					decrypt_params->authsize, decrypt_params->authslot_count);

	if (!decrypt_params->authslot_count)
		return -EINVAL;
	if (save_hashmap)
		total_size = decrypt_params->authslot_count * (AUTH_SIZE + SHA1_DIGEST_SIZE + IV_SIZE);
	else
		total_size = decrypt_params->authslot_count * AUTH_SIZE;

	num_pages = total_size / PAGE_SIZE;
	if (total_size % PAGE_SIZE)
		num_pages += 1;

	decrypt_authtags = vmalloc(num_pages * PAGE_SIZE);
	if (!decrypt_authtags)
		return -ENOMEM;
	pr_err("Audi: num_pages:%u\n", num_pages);

	authtag_slot_sector = authslot_base * (PAGE_SIZE >> 9);
	ret = read_auth_params(authtag_slot_sector, num_pages, decrypt_authtags);
	if (ret) {
		pr_err("Failed to read_auth_params:%d\n", ret);
		return ret;
	}

	if (save_hashmap) {
		offset = 0;
		pr_info("Total Authtags size %lu", total_size);
		while (offset < total_size) {
			entry = kmalloc(sizeof(struct auth_entry), GFP_KERNEL);
			if (!entry) {
				pr_err("kmalloc for auth_entry failed at offset %zu\n", offset);
				return -ENOMEM;
			}
			if (!(decrypt_authtags + offset))
				break;
			memcpy(entry->auth_tag, decrypt_authtags + offset, AUTH_SIZE);
			offset += AUTH_SIZE;
			if (!(decrypt_authtags + offset))
				break;
			memcpy(entry->sha1, decrypt_authtags + offset, SHA1_DIGEST_SIZE);
			offset += SHA1_DIGEST_SIZE;
			if (!(decrypt_authtags + offset))
				break;
			memcpy(entry->iv, decrypt_authtags + offset, IV_SIZE);
			offset += IV_SIZE;
			sha_val = jhash(entry->sha1, SHA1_DIGEST_SIZE, 0);
			hash_add(auth_map, &entry->node, sha_val);
		}
	}

	pr_info("Read %zu pages of AuthTags from swap\n", num_pages);
	first_decrypt = 1;
	pre_offset = 0;
	base_offset = 0;
	decrypt_auth_idx = 0;
	success_pages = 0;
	fail_pages = 0;

	pr_info("Hibernation: AES init done\n");
	return 0;
}

void get_authtag(int index, uint8_t *tag_out)
{
	unsigned long flags;

	spin_lock_irqsave(&authtag_lock, flags);
	if (save_hashmap)
		memcpy(tag_out, decrypt_authtags + index * (AUTH_SIZE + SHA1_DIGEST_SIZE + IV_SIZE), AUTH_SIZE);
	else
		memcpy(tag_out, decrypt_authtags + index * AUTH_SIZE, AUTH_SIZE);
	spin_unlock_irqrestore(&authtag_lock, flags);

}

static void init_sg_decrypt(struct scatterlist *sg, void *data, unsigned int size)
{
	sg_init_table(sg, 2);
	sg_set_buf(&sg[0], decrypt_params->aad, sizeof(decrypt_params->aad));
	sg_set_buf(&sg[1], data, size);
}

static void decrypt_page(void *data, void *buf, sector_t offset)
{
	struct scatterlist sg_in[2], sg_out[2];
	int ret = 0;
	struct aead_request *decrypt_req = NULL;
	uint8_t *decrypt_temp_out_buf = kmalloc(PAGE_SIZE + AUTH_SIZE, GFP_ATOMIC);
	uint8_t *auth_tag = kmalloc(AUTH_SIZE, GFP_ATOMIC);
	uint8_t sha1_digest[SHA1_DIGEST_SIZE];
	unsigned int sha_val;
	sector_t tag_idx = 0;

	offset = offset / 8;
	if (save_hashmap) {
		compute_sha1(buf, PAGE_SIZE, sha1_digest);
		sha_val = jhash(sha1_digest, SHA1_DIGEST_SIZE, 0);
		hash_for_each_possible(auth_map, entry, node, sha_val) {
			if (!memcmp(entry->sha1, sha1_digest, SHA1_DIGEST_SIZE)) {
				memcpy(auth_tag, entry->auth_tag, AUTH_SIZE);
				memcpy((void *)iv, entry->iv, IV_SIZE);
				break;
			}
		}
	}

	/* Allocate a request object */
	decrypt_req = aead_request_alloc(tfm, GFP_ATOMIC);
	if (!decrypt_req) {
		pr_err("[%s]: Error allocating aead req\n", __func__);
		ret = -ENOMEM;
		goto fail;
	}

	aead_request_set_callback(decrypt_req, 0, NULL, NULL);
	iv_size = crypto_aead_ivsize(tfm);
	if (iv_size && first_decrypt && !save_hashmap) {
		base_offset = offset;
		memcpy((void *)iv, decrypt_params->iv, IV_SIZE);
		pre_offset = offset;
	}
	crypto_aead_clear_flags(tfm, ~0);

	if (!decrypt_temp_out_buf || !auth_tag) {
		ret = -ENOMEM;
		goto fail;
	}

	if (!save_hashmap) {
		tag_idx = offset - base_offset;
		if (unlikely(tag_idx >= decrypt_params->authslot_count)) {
			ret = -EINVAL;
			goto fail;
		}
		get_authtag(tag_idx, auth_tag);
	}

	memcpy(decrypt_temp_out_buf, buf, PAGE_SIZE);
	memcpy(decrypt_temp_out_buf + PAGE_SIZE, (void *)auth_tag, AUTH_SIZE);
	init_sg_decrypt(sg_in, decrypt_temp_out_buf, PAGE_SIZE + AUTH_SIZE);
	init_sg_decrypt(sg_out, buf, PAGE_SIZE);
	aead_request_set_ad(decrypt_req, 12);
	if (!save_hashmap) {
		/*
		 * Dynamic IV:
		 * Treat decrypt_params->iv as the IV for the first decrypted page
		 * (base_offset) and derive: IV = base_iv + (offset - base_offset).
		 * Mirror encrypt_page() derivation.
		 */
		if (!first_decrypt && offset >= pre_offset) {
			increment_iv(iv, IV_SIZE, offset - pre_offset);
		} else {
			memcpy((void *)iv, decrypt_params->iv, IV_SIZE);
			increment_iv(iv, IV_SIZE, offset - base_offset);
		}
	}

	aead_request_set_crypt(decrypt_req, sg_in, sg_out, PAGE_SIZE + AUTH_SIZE, iv);
	ret = crypto_aead_decrypt(decrypt_req);
	if (ret) {
		fail_pages++;
	} else {
		success_pages++;
	}
	pre_offset = offset;
	decrypt_auth_idx++;

	if (first_decrypt)
		first_decrypt = 0;

fail:
	if (decrypt_temp_out_buf)
		kfree(decrypt_temp_out_buf);
	if (auth_tag)
		kfree(auth_tag);
	if (decrypt_req)
		aead_request_free(decrypt_req);
	return;
}

static int read_authpage_count(void)
{
	unsigned long total_auth_size;
	unsigned int num_auth_pages;

	if (save_hashmap)
		total_auth_size = params->authslot_count * (AUTH_SIZE + SHA1_DIGEST_SIZE + IV_SIZE);
	else
		total_auth_size = params->authslot_count * AUTH_SIZE;

	num_auth_pages = total_auth_size / PAGE_SIZE;
	if (total_auth_size % PAGE_SIZE)
		num_auth_pages += 1;

	return num_auth_pages;
}

static void hib_finish_batch(struct hib_bio_batch *hb)
{
	blk_finish_plug(&hb->plug);
}

static void hib_end_io(struct bio *bio)
{
	struct hib_bio_batch *hb = bio->bi_private;
	struct page *page = bio_first_page_all(bio);

	if (bio->bi_status) {
		pr_alert("Read-error on swap-device (%u:%u:%lu)\n",
			MAJOR(bio_dev(bio)), MINOR(bio_dev(bio)),
			(unsigned long long)bio->bi_iter.bi_sector);
	}

	if (bio_data_dir(bio) == WRITE)
		put_page(page);

	if (bio->bi_status && !hb->error)
		hb->error = bio->bi_status;
	if (atomic_dec_and_test(&hb->count))
		wake_up(&hb->wait);

	bio_put(bio);
}

static int hib_submit_io(blk_opf_t opf, int op_flags, sector_t page_off, void *addr, int sectorize,
				struct hib_bio_batch *hb)
{
	struct page *page = virt_to_page(addr);
	struct bio *bio;
	int error = 0;

	bio = bio_alloc(GFP_NOIO | __GFP_HIGH, 1);
	if (sectorize)
		bio->bi_iter.bi_sector = page_off * (PAGE_SIZE >> 9);
	else
		bio->bi_iter.bi_sector = page_off;
	bio_set_dev(bio, hib_resume_bdev);
	bio_set_op_attrs(bio, opf, op_flags);

	if (bio_add_page(bio, page, PAGE_SIZE, 0) < PAGE_SIZE) {
		pr_err("Adding page to bio failed at %llu\n",
			(unsigned long long)bio->bi_iter.bi_sector);
		bio_put(bio);
		return -EFAULT;
	}

	if (hb) {
		bio->bi_end_io = hib_end_io;
		bio->bi_private = hb;
		atomic_inc(&hb->count);
		submit_bio(bio);
	} else {
		error = submit_bio_wait(bio);
		bio_put(bio);
	}

	return error;
}

static int hib_wait_io(struct hib_bio_batch *hb)
{
	/*
	 * We are relying on the behavior of blk_plug that a thread with
	 * a plug will flush the plug list before sleeping.
	 */
	wait_event(hb->wait, atomic_read(&hb->count) == 0);
	return blk_status_to_errno(hb->error);
}

static int write_page(void *buf, sector_t offset, struct hib_bio_batch *hb)
{
	void *src;
	int ret;

	if (!offset)
		return -ENOSPC;

	if (hb) {
		src = (void *)__get_free_page(GFP_NOIO | __GFP_NOWARN |
						__GFP_NORETRY);
		if (src) {
			copy_page(src, buf);
		} else {
			ret = hib_wait_io(hb); /* Free pages */
			if (ret)
				return ret;
			src = (void *)__get_free_page(GFP_NOIO | __GFP_NOWARN |
							__GFP_NORETRY);
			if (src) {
				copy_page(src, buf);
			} else {
				WARN_ON_ONCE(1);
				hb = NULL;/* Go synchronous */
				src = buf;
			}
		}
	} else {
		src = buf;
	}
	return hib_submit_io(REQ_OP_WRITE, REQ_SYNC, offset, src, 1, hb);
}

/*
 * Number of pages compressed at one time. This is inline with UNC_PAGES
 * in kernel/power/swap.c.
 */
#define UNCMP_PAGES   32

static uint32_t get_size_of_compression_block_array(unsigned long pages)
{
	/*
	 * Get the max index based on total no. of pages. Current compression
	 * algorithm compresses each UNC_PAGES pages to x pages. Use this logic to
	 * get the max index.
	 */
	uint32_t max_index = DIV_ROUND_UP(pages, UNCMP_PAGES);

	uint32_t size = ALIGN((max_index * sizeof(*compressed_blk_array)), PAGE_SIZE);

	return size;
}

static void save_auth_and_params_to_disk(struct work_struct *work)
{
	int cur_slot;
	void *authpage;
	int params_slot;
	int authslot_count = 0;
	int authpage_count = read_authpage_count();
	struct hib_bio_batch hb;
	int i, err2;

	pr_info("Queued save_auth_and_params_to_disk..");
	hib_init_batch(&hb);

	/*
	 * Allocate a page to save the encryption params
	 */
	params_slot = alloc_swapdev_block(root_swap_dev);
	auth_slot_offset = params_slot + 1;
	pr_info("root_swap_dev:%hu",root_swap_dev);
	pr_info("params_slot:%d",params_slot);
	pr_info("auth_slot_offset:%d\n",auth_slot_offset);

	if (auth_slot) {
		*(int *)auth_slot = params_slot + 1;

		/* Currently bootloader code does the following to
		 * calculate the authentication slot index.
		 * authslot = NrMetaPages + NrCopyPages + NrSwapMapPages +
		 * HDR_SWP_INFO_NUM_PAGES;
		 *
		 * However, with compression enabled, we cannot apply the
		 * above logic to get the authentication slot. So this
		 * data should be provided to the BL for decryption to work.
		 *
		 * In the current implementation, BL doesn't make use of
		 * the swap_map_pages for restoring the hibernation image. So these pages
		 * could be used for other purposes. Use this to store the
		 * authentication slot number. This data will be stored at index as
		 * that of the first swap_map_page.
		 */
		write_page(auth_slot, 1, &hb);
	}

	authpage = authslot_start;
	pr_info("authpage_count:%d", authpage_count);
	while (authslot_count < authpage_count) {
		cur_slot = alloc_swapdev_block(root_swap_dev);
		write_page(authpage, cur_slot, &hb);
		authpage = (unsigned char *)authpage + PAGE_SIZE;
		authslot_count++;
	}
	pr_info("write_page params_slot, authslot_count:%d", authslot_count);
	pr_info("writing params_slot:%d",params_slot);
	pr_info("Write crypto params: authsize=%u, authslot_count=%u\n",
                        params->authsize, params->authslot_count);
	pr_info("params->iv:");
	for (i = 0; i < IV_SIZE; ++i)
		pr_cont("%02x", params->iv[i]);
	pr_info("params->aad:");
	for (i = 0; i < 12; ++i)
		pr_cont("%02x", params->aad[i]);

	write_page(params, params_slot, &hb);

	// Write the array holding the compressed block count to disk
	if (compressed_blk_array) {
		uint32_t size = get_size_of_compression_block_array(nr_pages);
		pr_info("get_size_of_compression_block_array size:%u",size);
		for (i = 0; i < size / PAGE_SIZE; i++) {
			cur_slot = alloc_swapdev_block(root_swap_dev);
			pr_err("write_page compressed_blk_array, cur_slot:%d", cur_slot);
			write_page(compressed_blk_array + (i * PAGE_SIZE), cur_slot, &hb);
		}
	}

	err2 = hib_wait_io(&hb);
	hib_finish_batch(&hb);
	complete_all(&write_done);
}

static void save_params_to_disk(void *data, unsigned short root_swap)
{
	pr_info("save_params_to_disk..");
	root_swap_dev = root_swap;
	queue_work(system_wq, &save_params_work);
}

static int poweroff_notifier(struct notifier_block *nb,
				unsigned long event, void *unused)
{
	switch (event) {

	case (SYS_POWER_OFF):
		if (authslot_start) {
			complete_all(&write_done);
			wait_for_completion(&write_done);
		}
		break;

	default:
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block poweroff_nb = {
	.notifier_call = poweroff_notifier,
};

#ifndef CONFIG_QCOM_KERNEL_SEC_KEY
static int get_key_from_ta(void)
{
	int ret;
	int req_len, rsp_len;

	struct cmd_req *req = (struct cmd_req *)app_handle->sbuf;
	struct cmd_rsp *rsp = NULL;

	req_len = sizeof(struct cmd_req);
	if (req_len & QSEECOM_ALIGN_MASK)
		req_len = QSEECOM_ALIGN(req_len);

	rsp = (struct cmd_rsp *)(app_handle->sbuf + req_len);
	rsp_len = sizeof(struct cmd_rsp);
	if (rsp_len & QSEECOM_ALIGN_MASK)
		rsp_len = QSEECOM_ALIGN(rsp_len);

	memset(req, 0, req_len);
	memset(rsp, 0, rsp_len);

	req->cmd = WRAP_KEY_CMD;
	req->wrapkey_req.save_time.hour = 4;
	rsp->wrapkey_rsp.wrapped_key_size = WRAPPED_KEY_SIZE;

	ret = qseecom_send_command(app_handle, req, req_len, rsp, rsp_len);
	if (!ret) {
		memcpy(params->key_blob, rsp->wrapkey_rsp.wrapped_key_buffer,
			WRAPPED_KEY_SIZE);
		memcpy(key, rsp->wrapkey_rsp.key_buffer, AES256_KEY_SIZE);
	}
	return ret;
}
#endif

static int init_aead(void)
{
	if (!tfm) {
		/*
		 * Prefer synchronous generic implementation: decrypt hook runs
		 * from bio end_io context and must not sleep waiting for async
		 * crypto completion.
		 */
		tfm = crypto_alloc_aead("gcm(aes-generic)", 0, 0);
		if (IS_ERR(tfm))
			tfm = crypto_alloc_aead("gcm(aes)", 0, 0);
		if (IS_ERR(tfm)) {
			pr_err("Error crypto_alloc_aead: %d\n",	PTR_ERR(tfm));
			return PTR_ERR(tfm);
		}
		tfm_configured = false;
	}
	return 0;
}

static int configure_tfm(void)
{
	int ret;

	if (tfm_configured)
		return 0;

	ret = crypto_aead_setauthsize(tfm, AUTH_SIZE);
	if (ret)
		return ret;

	ret = crypto_aead_setkey(tfm, key, AES256_KEY_SIZE);
	if (ret)
		return ret;

	tfm_configured = true;
	return 0;
}

#ifndef CONFIG_QCOM_KERNEL_SEC_KEY
static int init_ta_and_set_key(void)
{
	const uint32_t shared_buffer_len = 4096;
	int ret;

	#ifdef CONFIG_QCOM_HIB_USE_STATIC_KEY
		generate_random_key();
		return 0;
	#endif
	ret = qseecom_start_app(&app_handle, "secs2d", shared_buffer_len);
	if (ret) {
		pr_err("qseecom_start_app failed: %d\n", ret);
		return ret;
	}

	ret = get_key_from_ta();
	if (ret)
		pr_err("set_key returned %d\n", ret);

	ret = qseecom_shutdown_app(&app_handle);
	if (ret)
		pr_err("qseecom_shutdown_app failed: %d\n", ret);

	return ret;
}

#endif

static int alloc_auth_memory(void)
{
	unsigned long total_auth_size;

	/* Number of Auth slots is equal to the number of image pages */
	params->authslot_count = snapshot_get_image_size();
	if (save_hashmap) {
		total_auth_size = params->authslot_count * (AUTH_SIZE + SHA1_DIGEST_SIZE + IV_SIZE);
	} else {
		total_auth_size = params->authslot_count * AUTH_SIZE;
	}

	authslot_start = vmalloc(total_auth_size);
	if (!authslot_start)
		return -ENOMEM;
	pr_info("%s: authslot_count:%u, total_auth_size:%lu",__func__,params->authslot_count,total_auth_size);
	return 0;
}

void deinit_aes_encrypt(void)
{
	if (temp_out_buf) {
		free_pages((unsigned long)temp_out_buf, 1);
		temp_out_buf = NULL;
	}

	if (tfm) {
		crypto_free_aead(tfm);
		tfm = NULL;
	}
	tfm_configured = false;

	memset(key, 0, AES256_KEY_SIZE);
	memset(params->key_blob, 0, WRAPPED_KEY_SIZE);
	kfree(params);
}

static void cleanup_cmp_blk_array(void)
{
	blk_array_pos = 0;
	if (compressed_blk_array) {
		kvfree((void *)compressed_blk_array);
		compressed_blk_array = NULL;
	}
	if (auth_slot) {
		free_page((unsigned long)auth_slot);
		auth_slot = NULL;

	}
}

static int setup(void)
{
	int ret = 0;

	ret =  get_client_env_object(&client_env);
	if (ret) {
		pr_err("Failed to get client env object, ret = %d\n", ret);
		return ret;
	}

	ret = smci_clientenv_open(client_env, CLOWPOWERKEYMANAGER_UID,
				  &key_mgr_object);
	if (ret)
		pr_err("Failed to get Key Manager object, ret = %d\n", ret);

	return ret;
}

static void cleanup(void)
{
	SMCI_OBJECT_ASSIGN_NULL(key_mgr_object);
	SMCI_OBJECT_ASSIGN_NULL(client_env);
}

int key_mgr_get_key(uint32_t event, void *key, size_t key_len,
		    size_t *key_len_out)
{
	int ret = setup();

	if (ret)
		goto exit;

	ret = ILowPowerKeyManager_getKey(key_mgr_object, event, key,
					 key_len, key_len_out);
exit:
	cleanup();
	return ret;
}

int key_mgr_prepare(uint32_t event, const ILOWPOWERKEYMANAGER_key_info *key_info)
{
	int ret = setup();

	if (ret)
		goto exit;

	ret = ILowPowerKeyManager_prepare(key_mgr_object, event, key_info);
exit:
	cleanup();
	return ret;
}

#ifdef CONFIG_QCOM_KERNEL_SEC_KEY


void print_key(void) {
	int i;
	for (i = 0; i < AES256_KEY_SIZE; i++) {
		pr_debug("%u ", key[i]);
	}
}

int get_key_for_hib_exp(void)
{
        int qtee_ret = 0;
        size_t key_len_out;

        ILOWPOWERKEYMANAGER_key_info *key_info;

        pr_info("%s: Getting key from SSG",__func__);
        key_info = kmalloc(sizeof(ILOWPOWERKEYMANAGER_key_info), GFP_KERNEL);
        key_info->key_size = AES256_KEY_SIZE;

        qtee_ret = key_mgr_prepare(ILOWPOWERKEYMANAGER_HIBERNATE_WITH_ENCRYPTION, key_info);
	if(qtee_ret){
	        if (qtee_ret == ILOWPOWERKEYMANAGER_ERROR_INVALID_OPERATION_CHECK) {
			pr_info("%s: Thrashing the old key.. %d %d", qtee_ret, ILOWPOWERKEYMANAGER_ERROR_INVALID_OPERATION);
			qtee_ret = key_mgr_get_key(ILOWPOWERKEYMANAGER_HIBERNATE_WITH_ENCRYPTION, key,
					AES256_KEY_SIZE, &key_len_out);
			if (qtee_ret) {
				pr_err("%s: Failed to init QTEE: key_mgr_get_key: %d\n", __func__, qtee_ret);
				return qtee_ret;
			}
			qtee_ret = key_mgr_prepare(ILOWPOWERKEYMANAGER_HIBERNATE_WITH_ENCRYPTION, key_info);
			if (qtee_ret) {
	        	        pr_err("%s: Failed to init QTEE: key_mgr_prepare: %d\n", __func__, qtee_ret);
				return qtee_ret;
			}
		} else {
			pr_err("%s: Failed to init QTEE: key_mgr_prepare: %d\n", __func__, qtee_ret);
			return qtee_ret;
		}
	}

        qtee_ret = key_mgr_get_key(ILOWPOWERKEYMANAGER_HIBERNATE_WITH_ENCRYPTION, key,
				AES256_KEY_SIZE, &key_len_out);
        if (qtee_ret) {
                pr_err("%s: Failed to get Key: %d\n", __func__, qtee_ret);
	} else {
	        print_key();
	}

	return qtee_ret;
}
EXPORT_SYMBOL_GPL(get_key_for_hib_exp);
#endif

static int hibernate_pm_notifier(struct notifier_block *nb,
				unsigned long event, void *unused)
{
	int ret = NOTIFY_DONE;
#ifdef CONFIG_QCOM_KERNEL_SEC_KEY
	size_t key_len_out;
#endif 
        switch (event) {

	case (PM_HIBERNATION_PREPARE):
		params = kmalloc(sizeof(struct qcom_crypto_params), GFP_KERNEL);
		if (!params)
			return NOTIFY_BAD;

		ret = init_aead();
		if (ret) {
			pr_err("%s: Failed init_aead(): %d\n", __func__, ret);
			goto err_aead;
		}

		#ifndef CONFIG_QCOM_KERNEL_SEC_KEY
		ret = init_ta_and_set_key();
		if (ret) {
			pr_err("%s: Failed to init TA: %d\n", __func__, ret);
			goto err_setkey;
		}
		ret = configure_tfm();
		if (ret) {
			pr_err("%s: Failed to configure AEAD: %d\n", __func__, ret);
			goto err_setkey;
		}
		#endif
		temp_out_buf = (void *)__get_free_pages(GFP_KERNEL, 1);
		if (!temp_out_buf) {
			pr_err("%s: Failed alloc_auth_memory %d\n", __func__, ret);
			ret = -1;
			goto err_setkey;
		}
		init_completion(&write_done);
		break;

	case (PM_POST_HIBERNATION):
		deinit_aes_encrypt();
		cleanup_cmp_blk_array();
		break;

	case (PM_RESTORE_PREPARE):
		ret = init_aead();
		if (ret) {
			pr_err("%s: Failed init_aead(): %d\n", __func__, ret);
		}
		#ifdef CONFIG_QCOM_KERNEL_SEC_KEY
			ret = init_aes_decrypt();
			if (ret) {
				pr_err("%s: PM_RESTORE_PREPARE: Unable to read params & AuthTags from swap slot %d\n",
						__func__, ret);
				return NOTIFY_STOP;
			}
				pr_err("%s: PM_RESTORE_PREPARE: INIT_AES_DECRYPT: %s\n",__func__);
			ret = key_mgr_get_key(ILOWPOWERKEYMANAGER_HIBERNATE_WITH_ENCRYPTION, key,
					AES256_KEY_SIZE, &key_len_out);
			if (ret) {
				pr_err("%s: PM_RESTORE_PREPARE: Unable to restore key from QTEE: %d\n",
						__func__, ret);
				return NOTIFY_STOP;
			}
				pr_err("%s: PM_RESTORE_PREPARE: key_mgr_get_key: %s\n",__func__);
			ret = configure_tfm();
			if (ret) {
				pr_err("%s: PM_RESTORE_PREPARE: Unable to configure AEAD: %d\n",
				       __func__, ret);
				return NOTIFY_STOP;
			}
				pr_err("%s: PM_RESTORE_PREPARE: configure_tfm: %s\n",__func__);
			print_key();
		#endif
		break;
	case (PM_POST_RESTORE):
		deinit_aes_encrypt();
		cleanup_cmp_blk_array();
		break;
	default:
		WARN_ONCE(1, "Invalid PM Notifier\n");
		break;
	}

	return NOTIFY_DONE;

err_setkey:
	memset(params->key_blob, 0, WRAPPED_KEY_SIZE);
	memset(key, 0, AES256_KEY_SIZE);
	crypto_free_aead(tfm);
err_aead:
	kfree(params);
	return NOTIFY_BAD;
}

static struct notifier_block pm_nb = {
	.notifier_call = hibernate_pm_notifier,
};

static void init_aes_encrypt(void *data, void *unused)
{
	int ret;

	/*
	 * Encryption results in two things:
	 * 1. Encrypted data
	 * 2. Auth
	 * Save the Auth data of all pages locally and return only the
	 * encrypted page to the caller. Allocate memory to save the auth.
	 */
	#ifdef CONFIG_QCOM_USE_STATIC_KEY
		generate_random_key();
	#endif
	/* mark on-disk params struct version for compatibility check */
	params->version = QCOM_CRYPTO_PARAMS_VERSION;
	ret = alloc_auth_memory();
	if (ret) {
		pr_err("%s: Failed alloc_auth_memory %d\n", __func__, ret);
		goto err_auth;
	}

	first_encrypt = 1;
	pos = 0;
	pre_offset = 0;
	base_offset = 0;
	success_pages = 0;
	fail_pages = 0;
	memcpy(params->aad, "SECURE_S2D!!", sizeof(params->aad));
	params->authsize = AUTH_SIZE;
	memset(params->key_blob, 0, WRAPPED_KEY_SIZE);
	return;
err_auth:
	memset(params->key_blob, 0, WRAPPED_KEY_SIZE);
	memset(key, 0, AES256_KEY_SIZE);
	crypto_free_aead(tfm);
	kfree(params);
}

/*
 * Bit(part of swsusp_header_flags) to indicate if the image is uncompressed
 * or not. This is inline with SF_NOCOMPRESS_MODE defined in
 * kernel/power/power.h.
 */
#define SF_NOCOMPRESS_MODE      2

static void hibernated_do_mem_alloc(void *data, unsigned long pages,
	unsigned int swsusp_header_flags, int *ret)
{
	uint32_t size;

	/* total no. of pages in the snapshot image */
	nr_pages = pages;
	pr_info("%s: total_snapshot_pages:%lu..",__func__, pages);

	if (!(swsusp_header_flags & SF_NOCOMPRESS_MODE)) {
		size = get_size_of_compression_block_array(pages);

		compressed_blk_array = kvzalloc(size, GFP_KERNEL);
		if (!compressed_blk_array) {
			*ret = -ENOMEM;
			return;
		}

		/* Allocate memory to hold authentication slot start */
		auth_slot = (void *)get_zeroed_page(GFP_KERNEL);
		if (!auth_slot) {
			pr_err("Failed to allocate page for storing authentication tag slot number\n");
			*ret = -ENOMEM;
		}
	}
}

static void hibernate_save_cmp_len(void *data, size_t cmp_len)
{
	uint8_t pages;

	pages = DIV_ROUND_UP(cmp_len, PAGE_SIZE);
	compressed_blk_array[blk_array_pos++] = pages;
}


static ssize_t gethibkey_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, AES256_KEY_SIZE, "%d\n", gethibkey_value);
}

static ssize_t gethibkey_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	bool new_value;
	int ret = kstrtobool(buf, &new_value);
	if (ret < 0)
		return ret;

	gethibkey_value = new_value;

	if (gethibkey_value) {
		pr_err("Calling get_key_for_hib_exp..");
		get_key_for_hib_exp();
	}

	pr_err("Received: %s", buf);
	return count;
}

static struct kobj_attribute gethibkey_attr = __ATTR(gethibkey, 0664, gethibkey_show, gethibkey_store);

static int __init qcom_secure_hibernattion_init(void)
{
	int ret;

#ifndef CONFIG_HIBERNATION
	return 0;
#endif
	register_trace_android_vh_encrypt_page(encrypt_page, NULL);
	register_trace_android_vh_init_aes_encrypt(init_aes_encrypt, NULL);
	register_trace_android_vh_skip_swap_map_write(skip_swap_map_write, NULL);
	register_trace_android_vh_store_auth_slot_num(store_auth_slot_num, NULL);
	register_trace_android_vh_post_image_save(save_params_to_disk, NULL);
	register_trace_android_vh_hibernate_save_cmp_len(hibernate_save_cmp_len, NULL);
	register_trace_android_vh_hibernated_do_mem_alloc(hibernated_do_mem_alloc, NULL);
	register_trace_android_vh_decrypt_page(decrypt_page, NULL);

	spin_lock_init(&authtag_lock);
	spin_lock_init(&iv_lock);

	gethibkey_kobj = kobject_create_and_add("gethibkey_kobject", kernel_kobj);
	if (!gethibkey_kobj)
		return -ENOMEM;

	ret = sysfs_create_file(gethibkey_kobj, &gethibkey_attr.attr);
	if (ret)
		kobject_put(gethibkey_kobj);

	ret = register_pm_notifier(&pm_nb);
	if (ret) {
		pr_err("%s: Failed to register nb: %d\n", __func__, ret);
		return ret;
	}
	ret = register_reboot_notifier(&poweroff_nb);
	if (ret) {
		pr_err("%s: Failed to register nb: %d\n", __func__, ret);
		return ret;
	}
	INIT_WORK(&save_params_work, save_auth_and_params_to_disk);
	return 0;
}

module_init(qcom_secure_hibernattion_init);

MODULE_DESCRIPTION("Framework to encrypt a page using a trusted application");
MODULE_LICENSE("GPL");
