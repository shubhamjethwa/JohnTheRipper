/*
 * This software is Copyright (c) 2012 Sayantan Datta <std2048 at gmail dot com>
 * and it is hereby released to the general public under the following terms:
 * Redistribution and use in source and binary forms, with or without modification, are permitted.
 * Based on Solar Designer implementation of DES_bs_b.c in jtr-v1.7.9
 */
#include <assert.h>
#include <string.h>
#include <sys/time.h>

#include "options.h"
#include "opencl_LM_std.h"

#if !HARDCODE_SALT

#define DES_DEBUG 0

typedef unsigned WORD vtype;

/* Common parameters required for all kernels */
static cl_mem index768_gpu, index96_gpu, B_gpu;
DES_bs_vector *B;

/* Variables required for self test */
static cl_mem  opencl_DES_bs_data_gpu;
opencl_DES_bs_transfer *opencl_DES_bs_data;

/* Variables requred for cracking kernels.*/
static cl_mem loaded_hashes_gpu, transfer_keys_gpu, buffer_outKeyIdx;
static unsigned int num_loaded_hashes, min, max, cmp_out = 0, keyCount = 0; ;
static unsigned int *loaded_hashes, *outKeyIdx;
static unsigned char *input_keys;

/* Variables require for mask mode kernel */
static cl_mem mask_gpu;
static struct mask_context msk_ctx;
static struct db_main *DB;
static cl_kernel crk_kernel_mm;

/* Variable for other modes */
static cl_kernel crk_kernel_om;

/* Other housekeeping variables */
static unsigned int keys_changed = 0;
static size_t DES_local_work_size = WORK_GROUP_SIZE;
static size_t DES_global_work_size = MULTIPLIER;
static int self_test = 1; // This flag is reset to zero befor craking
static int mask_mode = 0;
static unsigned int int_keys = 1;

static int opencl_LM_mm(int *pcount, struct db_salt *salt);
static int opencl_LM_om(int *pcount, struct db_salt *salt);
static char *opencl_LM_get_key_mm(int index);
static char *opencl_LM_get_key_om(int index);
static void opencl_LM_set_key_mm(char *key, int index);

void opencl_LM_clean_all_buffer()
{
	const char* errMsg = "Release Memory Object :Failed";
	MEM_FREE(opencl_DES_bs_all);
	MEM_FREE(opencl_DES_bs_data);
	MEM_FREE(B);
	HANDLE_CLERROR(clReleaseMemObject(index768_gpu),errMsg);
	HANDLE_CLERROR(clReleaseMemObject(index96_gpu), errMsg);
	HANDLE_CLERROR(clReleaseMemObject(opencl_DES_bs_data_gpu), errMsg);
	HANDLE_CLERROR(clReleaseMemObject(B_gpu), errMsg);
	HANDLE_CLERROR(clReleaseMemObject(mask_gpu), errMsg);

	if(!self_test) {
		HANDLE_CLERROR(clReleaseMemObject(transfer_keys_gpu), errMsg);
		HANDLE_CLERROR(clReleaseMemObject(loaded_hashes_gpu), errMsg);
		HANDLE_CLERROR(clReleaseMemObject(buffer_outKeyIdx), errMsg);
		HANDLE_CLERROR(clReleaseMemObject(mask_gpu), errMsg);
		MEM_FREE(loaded_hashes);
		MEM_FREE(outKeyIdx);
	}

	MEM_FREE(input_keys);
	HANDLE_CLERROR(clReleaseKernel(crypt_kernel), "Error releasing self test kernel.");
	HANDLE_CLERROR(clReleaseKernel(crk_kernel_mm), "Error releasing mask mode kernel.");
	HANDLE_CLERROR(clReleaseKernel(crk_kernel_om), "Error releasing mask mode kernel.");
}

void opencl_LM_reset(struct db_main *db) {

	if(db) {
		int ctr = 0, length;
		struct db_salt *salt = db -> salts;

		do {
			salt -> sequential_id = ctr++;
			/*
			 * During cracking the maximum return count for crypt_all is num_loaded_hashes * 32.
			 * However the space is allocated only for MULTIPLIER * 32 number of stuffs.
			 */
			if((salt->count) > MULTIPLIER) {
				fprintf(stderr, "Reduce the number of hashs and try again..\n");
				exit(0);
			}
		} while((salt = salt->next));

		/* Each work item receives one key, so set the following parameters to tuned GWS for format. */
		db -> format -> params.max_keys_per_crypt = DES_global_work_size;
		db -> format -> params.min_keys_per_crypt = WORK_GROUP_SIZE * DES_BS_DEPTH;

		loaded_hashes = (unsigned int*)mem_alloc((db->password_count) * sizeof(int) * 2);
		outKeyIdx     = (unsigned int*)mem_calloc((db->password_count) * sizeof(unsigned int) * 2);
		length = ((db->format->params.max_keys_per_crypt) > ((db->password_count) * sizeof(unsigned int) * 2)) ?
			  (db->format->params.max_keys_per_crypt) : ((db->password_count) * sizeof(unsigned int) * 2);

		loaded_hashes_gpu = clCreateBuffer(context[ocl_gpu_id], CL_MEM_READ_WRITE, (db->password_count) * sizeof(unsigned int) * 2, NULL, &ret_code);
		HANDLE_CLERROR(ret_code, "Error creating buffer for loaded_hashes.\n");
		/* buffer_outKeyIdx is multiplexed for use as mask_offset input and keyIdx output */
		buffer_outKeyIdx = clCreateBuffer(context[ocl_gpu_id], CL_MEM_READ_WRITE, length, NULL, &ret_code);
		HANDLE_CLERROR(ret_code, "Error creating buffer compare output\n");
		transfer_keys_gpu = clCreateBuffer(context[ocl_gpu_id], CL_MEM_READ_WRITE, 8 * MULTIPLIER , NULL, &ret_code);
		HANDLE_CLERROR(ret_code, "Error creating buffer input keys.\n");

		self_test = 0;

		if(mask_mode) {

			HANDLE_CLERROR(clSetKernelArg(crk_kernel_mm, 0, sizeof(cl_mem), &index768_gpu), "Set Kernel Arg FAILED arg0\n");
			HANDLE_CLERROR(clSetKernelArg(crk_kernel_mm, 1, sizeof(cl_mem), &index96_gpu), "Set Kernel Arg FAILED arg1\n");
			HANDLE_CLERROR(clSetKernelArg(crk_kernel_mm, 2, sizeof(cl_mem), &B_gpu), "Set Kernel Arg FAILED arg2\n");
			HANDLE_CLERROR(clSetKernelArg(crk_kernel_mm, 3, sizeof(cl_mem), &loaded_hashes_gpu), "Set Kernel krnl Arg 3 :FAILED") ;
			HANDLE_CLERROR(clSetKernelArg(crk_kernel_mm, 5, sizeof(cl_mem), &transfer_keys_gpu), "Set Kernel Arg krnl FAILED arg5\n");
			HANDLE_CLERROR(clSetKernelArg(crk_kernel_mm, 6, sizeof(cl_mem), &mask_gpu), "Set Kernel Arg krnl FAILED arg6\n");
			HANDLE_CLERROR(clSetKernelArg(crk_kernel_mm, 7, sizeof(cl_mem), &buffer_outKeyIdx), "Set Kernel Arg krnl FAILED arg7\n");

			/* Expected number of keys to be generated on GPU per work item. Actual number will vary depending on the mask but it should be close */
			//db -> max_int_keys = 1500;

			DB = db;

			db->format->methods.crypt_all = opencl_LM_mm;
			db->format->methods.set_key = opencl_LM_set_key_mm;
			db->format->methods.get_key = opencl_LM_get_key_mm;

			db -> format -> params.max_keys_per_crypt = DES_global_work_size / 4  ;
			db -> format -> params.min_keys_per_crypt = DES_global_work_size / 4  ;
		}
		/* For other modes */
		else {
			HANDLE_CLERROR(clSetKernelArg(crk_kernel_om, 0, sizeof(cl_mem), &index768_gpu), "Set Kernel Arg FAILED arg0\n");
			HANDLE_CLERROR(clSetKernelArg(crk_kernel_om, 1, sizeof(cl_mem), &index96_gpu), "Set Kernel Arg FAILED arg1\n");
			HANDLE_CLERROR(clSetKernelArg(crk_kernel_om, 2, sizeof(cl_mem), &opencl_DES_bs_data_gpu), "Set Kernel Arg FAILED arg2\n");
			HANDLE_CLERROR(clSetKernelArg(crk_kernel_om, 3, sizeof(cl_mem), &B_gpu), "Set Kernel Arg FAILED arg3\n");
			HANDLE_CLERROR(clSetKernelArg(crk_kernel_om, 4, sizeof(cl_mem), &loaded_hashes_gpu), "Set Kernel krnl Arg 4 :FAILED");
			HANDLE_CLERROR(clSetKernelArg(crk_kernel_om, 6, sizeof(cl_mem), &buffer_outKeyIdx), "Set Kernel Arg krnl FAILED arg6\n");


			db->format->methods.crypt_all = opencl_LM_om;
			db->format->methods.set_key = opencl_LM_set_key_self_test;
			db->format->methods.get_key = opencl_LM_get_key_om;
		}
	}
}

static void check_mask_lm(struct mask_context *msk_ctx) {
	int i, j, k ;
	if(msk_ctx -> count > 7) msk_ctx -> count = 7;

  /* Assumes msk_ctx -> activeRangePos[] is sorted. Check if any range exceeds des key limit */
	for( i = 0; i < msk_ctx->count; i++)
		if(msk_ctx -> activeRangePos[i] >= 7) {
			msk_ctx->count = i;
			break;
		}
	j = 0;
	i = 0;
	k = 0;
 /* Append non-active portion to activeRangePos[] for ease of computation inside GPU. Inside GPU it will always load
  * the first three ranges even if not all of them are active. So the first three ranges must contain some desirable
  *  value */
	while((j <= msk_ctx -> activeRangePos[k]) && (k < msk_ctx -> count)) {
		if(j == msk_ctx -> activeRangePos[k]) {
			k++;
			j++;
			continue;
		}
		msk_ctx -> activeRangePos[msk_ctx -> count + i] = j;
		i++;
		j++;
	}
	while ((i+msk_ctx->count) < 7) {
		msk_ctx -> activeRangePos[msk_ctx -> count + i] = j;
		i++;
		j++;
	}

	/* Zeroes the character count for non-active portion. This is helpful in regenerating password in host. */
	for(i = msk_ctx->count; i < 7; i++) {
		msk_ctx->ranges[msk_ctx -> activeRangePos[i]].count = 0;
	}

	/* Consecutive charachters in ranges that have all the charachters consective are
	 * arranged in ascending order. This is to make password generation on host and device
	 * match each other when consective character optimization is used*/
	for( i = 0; i < msk_ctx->count; i++)
		if(msk_ctx->ranges[msk_ctx -> activeRangePos[i]].start != 0) {
			for (j = 0; j < msk_ctx->ranges[msk_ctx -> activeRangePos[i]].count; j++)
				msk_ctx->ranges[msk_ctx -> activeRangePos[i]].chars[j] =
					msk_ctx->ranges[msk_ctx -> activeRangePos[i]].start + j;
		}
}

void opencl_LM_init_global_variables() {

	B = (DES_bs_vector*) mem_alloc ((MULTIPLIER + 15)* 2 * sizeof(DES_bs_vector));
	opencl_DES_bs_all = (opencl_DES_bs_combined*) mem_alloc (((MULTIPLIER >> DES_BS_LOG2) + 15) * sizeof(opencl_DES_bs_combined));
	opencl_DES_bs_data = (opencl_DES_bs_transfer*) mem_alloc (((MULTIPLIER >> DES_BS_LOG2) + 15) * sizeof(opencl_DES_bs_transfer));
	input_keys = (unsigned char *) mem_alloc( MULTIPLIER * 7);
}

/* Mask mode */
void opencl_LM_set_key_mm(char *key, int index)
{
	keyCount++;
	if(!keys_changed) {
		keys_changed = 1;
		memset(input_keys, 0 , 7 * MULTIPLIER);
	}
	memcpy(input_keys + 7 * index, key , 7);


}

static void passgen(int ctr, int mask_offset, char *key) {
	int i, j, k;

	mask_offset = msk_ctx.flg_wrd ? mask_offset : 0;

	i =  ctr % msk_ctx.ranges[msk_ctx.activeRangePos[0]].count;
	key[msk_ctx.activeRangePos[0] + mask_offset] = msk_ctx.ranges[msk_ctx.activeRangePos[0]].chars[i];

	if (msk_ctx.ranges[msk_ctx.activeRangePos[1]].count) {
		j = (ctr / msk_ctx.ranges[msk_ctx.activeRangePos[0]].count) % msk_ctx.ranges[msk_ctx.activeRangePos[1]].count;
		key[msk_ctx.activeRangePos[1] + mask_offset] = msk_ctx.ranges[msk_ctx.activeRangePos[1]].chars[j];
	}
	if (msk_ctx.ranges[msk_ctx.activeRangePos[2]].count) {
		k = (ctr / (msk_ctx.ranges[msk_ctx.activeRangePos[0]].count * msk_ctx.ranges[msk_ctx.activeRangePos[1]].count)) % msk_ctx.ranges[msk_ctx.activeRangePos[2]].count;
		key[msk_ctx.activeRangePos[2] + mask_offset] = msk_ctx.ranges[msk_ctx.activeRangePos[2]].chars[k];
	}
}

char *opencl_LM_get_key_self_test(int index)
{
	static char out[8];
	unsigned int section,block;
	unsigned char *src;
	char *dst;

	section = index/DES_BS_DEPTH;
	block  = index%DES_BS_DEPTH;
	init_t();

	src = opencl_DES_bs_all[section].pxkeys[block];
	dst = out;
	while (dst < &out[7] && (*dst = *src)) {
		src += sizeof(DES_bs_vector) * 8;
		dst++;
	}
	*dst = 0;
	return out;
}

char *opencl_LM_get_key_mm(int index)
{
	static char out[PLAINTEXT_LENGTH + 1];

	int keyIdx = 0;
	int section = index >> 5;
/* There is a possiblity of wrong status check when
 * status is checked just after a successfult password crack. */
	if((section < num_loaded_hashes) && cmp_out) {
		int section = index >> 5;
		//fprintf(stderr, "InGetKey%0x", index);
		keyIdx = outKeyIdx[section + num_loaded_hashes] + index % DES_BS_DEPTH;
		index = outKeyIdx[section] & 0x7fffffff;
		//fprintf(stderr, "InGetKey%0x %0x ", index, MAX_KEYS_PER_CRYPT);
	}

	index = (index > (MULTIPLIER - 1))? MULTIPLIER - 1 : index;
	memcpy(out, input_keys + 7 * index, 7);

	if(cmp_out && mask_mode)
		passgen(keyIdx, 0, out);

	out[7] = '\0';
	return out;
}

static char *opencl_LM_get_key_om(int index)
{
	static char out[8];
	unsigned int section,block;
	unsigned char *src;
	char *dst;

	section = index >> 5;
/* There is a possiblity of wrong status check when
 * status is checked just after a successfult password crack. */
	if((section < num_loaded_hashes) && cmp_out)
		index = ((outKeyIdx[section] & 0x7fffffff) << 5) + index % DES_BS_DEPTH;

	index = (index > (MULTIPLIER - 1))? MULTIPLIER - 1 : index;

	section = index/DES_BS_DEPTH;
	block  = index%DES_BS_DEPTH;
	init_t();

	src = opencl_DES_bs_all[section].pxkeys[block];
	dst = out;
	while (dst < &out[7] && (*dst = *src)) {
		src += sizeof(DES_bs_vector) * 8;
		dst++;
	}
	*dst = 0;
	return out;
}

static int opencl_LM_cmp_one_b(WORD *binary, int count, int index)
{
	int bit;
	DES_bs_vector *b;
	int depth;
	unsigned int section;
	//if(count == 64) printf("cmp exact%d\n",index);
	section = index >> DES_BS_LOG2;
	index &= (DES_BS_DEPTH - 1);
	depth = index >> 3;
	index &= 7;

	b = (DES_bs_vector *)((unsigned char *)&B[section * 64] + depth);

#define GET_BIT \
	((unsigned WORD)*(unsigned char *)&b[0] >> index)

	for (bit = 0; bit < 31; bit++, b++)
		if ((GET_BIT ^ (binary[0] >> bit)) & 1)
			return 0;

	for (; bit < count; bit++, b++)
		if ((GET_BIT ^ (binary[bit >> 5] >> (bit & 0x1F))) & 1)
			return 0;

#undef GET_BIT
	return 1;
}

inline int opencl_LM_cmp_one(void *binary, int index)
{
	int section = (index >> 5) ;
	if(self_test) return opencl_LM_cmp_one_b((WORD*)binary, 64, index);
	if(section < min) return 0;
	if(section > max) return 0;
	if(outKeyIdx[section] > 0) return opencl_LM_cmp_one_b((WORD*)binary, 64, index);

	return 0;
}

static void find_best_gws(struct fmt_main *fmt)
{
	struct timeval start, end;
	double savetime;
	unsigned int count = WORK_GROUP_SIZE * DES_BS_DEPTH;
	double speed = 999999, diff;

	gettimeofday(&start, NULL);
	opencl_LM_self_test((int*)&count, NULL);
	gettimeofday(&end, NULL);
	savetime = (end.tv_sec - start.tv_sec) + (double)(end.tv_usec - start.tv_usec) / 1000000.000;
	speed = ((double)count) / savetime;
	do {
		count *= 2;
		if ((count) > MULTIPLIER) {
			count = count >> 1;
			break;

		}
		gettimeofday(&start, NULL);
		opencl_LM_self_test((int*)&count, NULL);
		gettimeofday(&end, NULL);
		savetime = (end.tv_sec - start.tv_sec) + (double)(end.tv_usec - start.tv_usec) / 1000000.000;
		diff = (((double)count) / savetime) / speed;
		if (diff < 1) {
			count = count >> 1;
			break;
		}
		diff = diff - 1;
		diff = (diff < 0) ? (-diff) : diff;
		speed = ((double)count) / savetime;
	} while(diff > 0.01);

	if (options.verbosity > 1)
		fprintf(stderr, "Optimal Global Work Size:%d\n",
		        count);

	fmt -> params.max_keys_per_crypt = DES_global_work_size = count;
	fmt -> params.min_keys_per_crypt = DES_BS_DEPTH * WORK_GROUP_SIZE;
}

void LM_select_device(struct fmt_main *fmt)
{
	const char *errMsg;

	opencl_prepare_dev(ocl_gpu_id);
	opencl_read_source("$JOHN/kernels/LM_kernel_0.cl") ;
	opencl_build(ocl_gpu_id, "-fno-bin-amdil -fno-bin-source -fbin-exe", 0, NULL, 1);

	crk_kernel_mm = clCreateKernel(program[ocl_gpu_id], "lm_mm", &ret_code) ;
	HANDLE_CLERROR(ret_code,"Error creating kernel DES_bs_25_mm");
	crk_kernel_om = clCreateKernel(program[ocl_gpu_id], "lm_om", &ret_code) ;
	HANDLE_CLERROR(ret_code,"Error creating kernel DES_bs_25_om");

	crypt_kernel = clCreateKernel(program[ocl_gpu_id], "lm_self_test", &ret_code) ;
	HANDLE_CLERROR(ret_code,"Error creating kernel DES_bs_25_self_test");

	errMsg = "Create Buffer FAILED.";
	opencl_DES_bs_data_gpu = clCreateBuffer(context[ocl_gpu_id], CL_MEM_READ_WRITE, ((MULTIPLIER >> DES_BS_LOG2) + 15) * sizeof(opencl_DES_bs_transfer), NULL, &ret_code);
	HANDLE_CLERROR(ret_code, errMsg);
	index768_gpu = clCreateBuffer(context[ocl_gpu_id], CL_MEM_READ_WRITE, 768 * sizeof(unsigned int), NULL, &ret_code);
	HANDLE_CLERROR(ret_code, errMsg);
	index96_gpu = clCreateBuffer(context[ocl_gpu_id], CL_MEM_READ_WRITE, 96 * sizeof(unsigned int), NULL, &ret_code);
	HANDLE_CLERROR(ret_code, errMsg);
	B_gpu = clCreateBuffer(context[ocl_gpu_id], CL_MEM_READ_WRITE, 2 * (MULTIPLIER + 15) * sizeof(DES_bs_vector), NULL, &ret_code);
	HANDLE_CLERROR(ret_code, errMsg);

	HANDLE_CLERROR(clSetKernelArg(crypt_kernel, 0, sizeof(cl_mem), &index768_gpu), "Set Kernel Arg FAILED arg0\n");
	HANDLE_CLERROR(clSetKernelArg(crypt_kernel, 1, sizeof(cl_mem), &index96_gpu), "Set Kernel Arg FAILED arg1\n");
	HANDLE_CLERROR(clSetKernelArg(crypt_kernel, 2, sizeof(cl_mem), &opencl_DES_bs_data_gpu), "Set Kernel Arg FAILED arg2\n");
	HANDLE_CLERROR(clSetKernelArg(crypt_kernel, 3, sizeof(cl_mem), &B_gpu), "Set Kernel Arg FAILED arg4\n");

	HANDLE_CLERROR(clEnqueueWriteBuffer(queue[ocl_gpu_id], index768_gpu, CL_TRUE, 0, 768*sizeof(unsigned int), index768, 0, NULL, NULL ), "Failed Copy data to gpu");

	mask_gpu = clCreateBuffer(context[ocl_gpu_id], CL_MEM_READ_WRITE, sizeof(struct mask_context) , NULL, &ret_code);
	HANDLE_CLERROR(ret_code, "Error creating buffer for mask transfer.");

	/* Check if the mask is being used */
	if(options.mask) {
		int i;
		mask_mode = 1;
		if(options.wordlist) {
			fprintf(stderr, "mask + wordlist not supported by this format.\n");
			exit(EXIT_FAILURE);
		}

		if(!fmt->private.msk_ctx) {
			fprintf(stderr, "No given mask.Exiting...\n");
			exit(EXIT_FAILURE);
		}
		memcpy(&msk_ctx, fmt->private.msk_ctx, sizeof(struct mask_context));
		check_mask_lm(&msk_ctx);
		int_keys = 1;
		for (i = 0; i < msk_ctx.count; i++)
			int_keys *= msk_ctx.ranges[msk_ctx.activeRangePos[i]].count;
#if DES_DEBUG
		fprintf(stderr, "Multiply the end c/s with:%d\n", int_keys);
		int j;
		for(i = 0; i < 8; i++)
			printf("%d ",msk_ctx.activeRangePos[i]);
			printf("\n");
			for(i = 0; i < msk_ctx.count; i++){
				for(j = 0; j < msk_ctx.ranges[msk_ctx.activeRangePos[i]].count; j++)
					printf("%c ",msk_ctx.ranges[msk_ctx.activeRangePos[i]].chars[j]);
				printf("\n");
				printf("START:%c",msk_ctx.ranges[msk_ctx.activeRangePos[i]].start);
			      printf("\n");
			}
#endif
		HANDLE_CLERROR(clEnqueueWriteBuffer(queue[ocl_gpu_id], mask_gpu, CL_TRUE, 0, sizeof(struct mask_context), &msk_ctx, 0, NULL, NULL ), "Failed Copy data to gpu");
	}

	if (!global_work_size)
		find_best_gws(fmt);
	else {
		if (options.verbosity > 3)
			fprintf(stderr, "Global worksize (GWS) forced to %zu\n",
			        global_work_size);
		fmt -> params.max_keys_per_crypt = DES_global_work_size = global_work_size;
		fmt -> params.min_keys_per_crypt = WORK_GROUP_SIZE * DES_BS_DEPTH ;
	}

	fmt->methods.crypt_all = opencl_LM_self_test;
	fmt->methods.set_key = opencl_LM_set_key_self_test;
	fmt->methods.get_key = opencl_LM_get_key_self_test;
}

int opencl_LM_self_test(int *pcount, struct db_salt *salt)
{
	int keys_count = *pcount;
	unsigned int section = 0, keys_count_multiple;
	cl_event evnt;
	size_t N, M;

	if (keys_count % DES_BS_DEPTH == 0)
		keys_count_multiple = keys_count;
	else
		keys_count_multiple = (keys_count / DES_BS_DEPTH + 1) * DES_BS_DEPTH;

	section = keys_count_multiple / DES_BS_DEPTH;
	M = DES_local_work_size;

	if (section % DES_local_work_size != 0)
		N = (section / DES_local_work_size + 1) * DES_local_work_size ;
	else
		N = section;

	HANDLE_CLERROR(clEnqueueWriteBuffer(queue[ocl_gpu_id], opencl_DES_bs_data_gpu, CL_TRUE, 0, N * sizeof(opencl_DES_bs_transfer), opencl_DES_bs_data, 0, NULL, NULL ), "Failed Copy data to gpu");

	ret_code = clEnqueueNDRangeKernel(queue[ocl_gpu_id], crypt_kernel, 1, NULL, &N, &M, 0, NULL, &evnt);
	HANDLE_CLERROR(ret_code, "Enque Kernel Failed");

	clWaitForEvents(1, &evnt);

	HANDLE_CLERROR(clEnqueueReadBuffer(queue[ocl_gpu_id], B_gpu, CL_TRUE, 0, N * 64 * sizeof(DES_bs_vector), B, 0, NULL, NULL), "Write FAILED\n");
	clFinish(queue[ocl_gpu_id]);
	return keys_count;
}

static int opencl_LM_mm(int *pcount, struct db_salt *salt)
{
	int keys_count = *pcount;
	unsigned int sections = 0;
	int i = 0, *bin;
	struct db_password *pw;
	cl_event evnt;
	size_t N, M;

	sections = keys_count;
	M = DES_local_work_size;
	if (sections % DES_local_work_size != 0)
		N = (sections / DES_local_work_size + 1) * DES_local_work_size ;
	else
		N = sections;
	N = N > MULTIPLIER ? MULTIPLIER : N;

		pw = salt -> list;
	do {
		bin = (int *)pw -> binary;
		loaded_hashes[i] = bin[0] ;
		loaded_hashes[i + salt -> count] = bin[1];
		i++ ;
		//printf("%d %d\n", i++, bin[0]);
	} while ((pw = pw -> next)) ;
	num_loaded_hashes = (salt -> count);
	//printf("%d\n",loaded_hashes[salt->count-1 + salt -> count]);
	HANDLE_CLERROR(clEnqueueWriteBuffer(queue[ocl_gpu_id], loaded_hashes_gpu, CL_TRUE, 0, (salt -> count) * sizeof(int) * 2, loaded_hashes, 0, NULL, NULL ), "Failed Copy data to gpu");
	HANDLE_CLERROR(clEnqueueWriteBuffer(queue[ocl_gpu_id], transfer_keys_gpu, CL_TRUE, 0, 8 * N, input_keys, 0, NULL, NULL ), "Failed Copy data to gpu");
	HANDLE_CLERROR(clSetKernelArg(crk_kernel_mm, 4, sizeof(int), &(salt->count)), "Set Kernel krnl Arg 5 :FAILED") ;

	*pcount = (sections * int_keys) ;

	ret_code = clEnqueueNDRangeKernel(queue[ocl_gpu_id], crk_kernel_mm, 1, NULL, &N, &M, 0, NULL, &evnt);
	HANDLE_CLERROR(ret_code, "Enque Kernel Failed");

	clWaitForEvents(1, &evnt);

	HANDLE_CLERROR(clEnqueueReadBuffer(queue[ocl_gpu_id], buffer_outKeyIdx, CL_TRUE, 0, (salt->count) * sizeof(unsigned int), outKeyIdx, 0, NULL, NULL), "Write FAILED\n");
	cmp_out = 0;

	// If a positive match is found outKeyIdx contains some positive (gid | 0x80000000) value else contains 0
	for(i = 0; i < ((salt->count) & (~cmp_out)); i++)
		cmp_out = outKeyIdx[i]?0xffffffff:0;
#if DES_DEBUG
	printf("CMP out %d %d %d\n", cmp_out, (salt->sequential_id), keyCount);
#endif
	if (cmp_out) {
		max = 0;
		min = salt->count;
		HANDLE_CLERROR(clEnqueueReadBuffer(queue[ocl_gpu_id], buffer_outKeyIdx, CL_TRUE, 0, 2 * (salt->count) * sizeof(unsigned int), outKeyIdx, 0, NULL, NULL), "Write FAILED\n");
		for (i = 0; i < salt->count ;i++) {
			if (outKeyIdx[i] > 0) {
				max = i;
				if(max < min)
					min = max;
			}
		}
		HANDLE_CLERROR(clEnqueueReadBuffer(queue[ocl_gpu_id], B_gpu, CL_TRUE, 0, (salt -> count) * 64 * sizeof(DES_bs_vector), B, 0, NULL, NULL), "Write FAILED\n");
		clFinish(queue[ocl_gpu_id]);
#if DES_DEBUG
		printf("crypt all %d\n",max + 1);
#endif
		return (max + 1) * DES_BS_DEPTH ;
	}
	else return 0;
}

int opencl_LM_om(int *pcount, struct db_salt *salt)
{
	int keys_count = *pcount;
	unsigned int section = 0, keys_count_multiple;
	int i = 0, *bin;
	struct db_password *pw;
	cl_event evnt;
	size_t N, M;

	if (keys_count % DES_BS_DEPTH == 0)
		keys_count_multiple = keys_count;
	else
		keys_count_multiple = (keys_count / DES_BS_DEPTH + 1) * DES_BS_DEPTH;

	section = keys_count_multiple / DES_BS_DEPTH;
	M = DES_local_work_size;

	if (section % DES_local_work_size != 0)
		N = (section / DES_local_work_size + 1) * DES_local_work_size ;
	else
		N = section;

	pw = salt -> list;
	do {
		bin = (int *)pw -> binary;
		loaded_hashes[i] = bin[0] ;
		loaded_hashes[i + salt -> count] = bin[1];
		i++ ;
		//printf("%d %d\n", i++, bin[0]);
	} while ((pw = pw -> next)) ;
	num_loaded_hashes = (salt -> count);
	//printf("%d\n",loaded_hashes[salt->count-1 + salt -> count]);
	HANDLE_CLERROR(clEnqueueWriteBuffer(queue[ocl_gpu_id], loaded_hashes_gpu, CL_TRUE, 0, (salt -> count) * sizeof(int) * 2, loaded_hashes, 0, NULL, NULL ), "Failed Copy data to gpu");
	HANDLE_CLERROR(clEnqueueWriteBuffer(queue[ocl_gpu_id], opencl_DES_bs_data_gpu, CL_TRUE, 0, N * sizeof(opencl_DES_bs_transfer), opencl_DES_bs_data, 0, NULL, NULL ), "Failed Copy data to gpu");

	HANDLE_CLERROR(clSetKernelArg(crk_kernel_om, 5, sizeof(int), &(salt->count)), "Set Kernel krnl Arg 5 :FAILED") ;

	ret_code = clEnqueueNDRangeKernel(queue[ocl_gpu_id], crk_kernel_om, 1, NULL, &N, &M, 0, NULL, &evnt);
	HANDLE_CLERROR(ret_code, "Enque Kernel Failed");

	clWaitForEvents(1, &evnt);

	HANDLE_CLERROR(clEnqueueReadBuffer(queue[ocl_gpu_id], buffer_outKeyIdx, CL_TRUE, 0, (salt->count) * sizeof(unsigned int), outKeyIdx, 0, NULL, NULL), "Write FAILED\n");
	cmp_out = 0;

	// If a positive match is found outKeyIdx contains some positive (gid | 0x80000000) value else contains 0
	for(i = 0; i < ((salt->count) & (~cmp_out)); i++)
		cmp_out = outKeyIdx[i]?0xffffffff:0;
#if DES_DEBUG
	printf("CMP out %d %d %d\n", cmp_out, (salt->sequential_id), keyCount);
#endif
	if (cmp_out) {
		max = 0;
		min = salt->count;
		HANDLE_CLERROR(clEnqueueReadBuffer(queue[ocl_gpu_id], buffer_outKeyIdx, CL_TRUE, 0, 2 * (salt->count) * sizeof(unsigned int), outKeyIdx, 0, NULL, NULL), "Write FAILED\n");
		for (i = 0; i < salt->count ;i++) {
			if (outKeyIdx[i] > 0) {
				max = i;
				if(max < min)
					min = max;
			}
		}
		HANDLE_CLERROR(clEnqueueReadBuffer(queue[ocl_gpu_id], B_gpu, CL_TRUE, 0, (salt -> count) * 64 * sizeof(DES_bs_vector), B, 0, NULL, NULL), "Write FAILED\n");
		clFinish(queue[ocl_gpu_id]);
#if DES_DEBUG
		printf("crypt all %d\n",max + 1);
#endif
		return (max + 1) * DES_BS_DEPTH ;
	}
	else return 0;
}
#endif