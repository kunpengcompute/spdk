/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"

#include "spdk_cunit.h"

#include "util/crc16.c"
#include <inttypes.h>
#include <errno.h>

typedef enum {
    PERF_TEST = 0,
    CHECKSUM_TEST,
} Crc16TestType;

typedef struct {
    uint64_t memorySize;
    uint64_t blockSize;
    uint32_t count;
    Crc16TestType type;
} Crc16PerfParam;

enum ec_block_unit { BYTE = 1, KB = 1024, MB = 1024 * 1024, GB = 1024 * 1024 * 1024 };

struct option g_longOptions[] = {{"type", required_argument, NULL, 't'},
    {"memory", required_argument, NULL, 'm'},
    {"block", required_argument, NULL, 'b'},
    {"count", required_argument, NULL, 'c'},
    {"help", no_argument, NULL, 'h'}};

void usage(void);
uint32_t str2byte(const char *str);
long str2num(const char *str);
void parser_argument(Crc16PerfParam *param, int argc, char **argv);
void crc16_perftest(const Crc16PerfParam *param, uint8_t *buf, uint8_t **block);
void crc16_checksum_test(void);

static void
test_crc16_t10dif(void)
{
	uint16_t crc;
	char buf[] = "123456789";

	crc = spdk_crc16_t10dif(0, buf, strlen(buf));
	CU_ASSERT(crc == 0xd0db);
}

static void
test_crc16_t10dif_seed(void)
{
	uint16_t crc = 0;
	char buf1[] = "1234";
	char buf2[] = "56789";

	crc = spdk_crc16_t10dif(crc, buf1, strlen(buf1));
	crc = spdk_crc16_t10dif(crc, buf2, strlen(buf2));
	CU_ASSERT(crc == 0xd0db);
}

static void
test_crc16_t10dif_copy(void)
{
	uint16_t crc1 = 0, crc2;
	char buf1[] = "1234";
	char buf2[] = "56789";
	char *buf3 = calloc(1, strlen(buf1) + strlen(buf2) + 1);
	SPDK_CU_ASSERT_FATAL(buf3 != NULL);

	crc1 = spdk_crc16_t10dif_copy(crc1, buf3, buf1, strlen(buf1));
	crc1 = spdk_crc16_t10dif_copy(crc1, buf3 + strlen(buf1), buf2, strlen(buf2));
	CU_ASSERT(crc1 == 0xd0db);

	crc2  = spdk_crc16_t10dif(0, buf3, strlen(buf3));
	CU_ASSERT(crc2 == 0xd0db);

	free(buf3);
}

void 
usage(void)
{
    printf("\nUsage:\n");
    printf("    --type      -t      type, 0: perf test, 1: checksum compare\n");
    printf("    --block     -b      blocksize [1~]\n");
    printf("    --count     -c      running count [~]\n");
    printf("    --memory    -m      memory size eg. 1GB 500MB\n");
    printf("    --help      -h      help\n");
    return;
}

long 
str2num(const char *str) {
    char *endPtr;
    errno = 0;  
    long num = strtol(str, &endPtr, 10);
    if (errno != 0 || endPtr == str) { 
        return -1; 
    }
    return num;
}

uint32_t 
str2byte(const char *str)
{
    char num[100] = {0};
    enum ec_block_unit unit = BYTE;
    for (uint32_t i = 0; i < strlen(str); i++) {
        if (str[i] == 'k' || str[i] == 'K') {
            unit = KB;
            break;
        } else if ((str[i] == 'M' || str[i] == 'm')) {
            unit = MB;
            break;
        } else if ((str[i] == 'G' || str[i] == 'g')) {
            unit = GB;
            break;
        } else {
            num[i] = str[i];
        }
    }

    return unit * str2num(num);
}

void 
parser_argument(Crc16PerfParam *param, int argc, char **argv)
{
    int opt = 0;
    int type;
    uint64_t blockSize;
    uint64_t memory;
    int count;
    while ((opt = getopt_long(argc, argv, "ha:t:b:c:m:", g_longOptions, NULL)) != -1) {
        switch (opt) {
            case 't':
                type = str2num(optarg);
                if (type != 0 && type != 1) {
                    usage();
                    exit(0);
                }
                if (type == 0) {
                    param->type = PERF_TEST;
                } else {
                    param->type = CHECKSUM_TEST;
                }
                break;
            case 'b':
                blockSize = str2byte(optarg);
                if (blockSize > 0) {
                    param->blockSize = blockSize;
                } else {
                    usage();
                    exit(0);
                }
                break;
            case 'c':
                count = str2num(optarg);
                if (count > 0) {
                    param->count = count;
                } else {
                    usage();
                    exit(0);
                }
                break;
            case 'm':
                memory = str2byte(optarg);
                if (memory > 0) {
                    param->memorySize = memory;
                } else {
                    usage();
                    exit(0);
                }
                break;
            case 'h':
                usage();
                exit(0);
                break;
            default:
                usage();
                exit(0);
                break;
        }
    }
}

void 
crc16_perftest(const Crc16PerfParam *param, uint8_t *buf, uint8_t **block)
{
    uint32_t crc = 0;
    size_t i = 0;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start); 
    while (i < param->count) {
        uint32_t n = rand() % (param->memorySize / param->blockSize);
        crc = spdk_crc16_t10dif(crc, block[n], param->blockSize);
        i++;
    }
    clock_gettime(CLOCK_MONOTONIC, &end);  

    long seconds = end.tv_sec - start.tv_sec;
    long nanoseconds = end.tv_nsec - start.tv_nsec;
    long elapsed = seconds * 1e9 + nanoseconds;  
    printf("crc16 cost time: %.2f ns\n", (double)elapsed / param->count);
    printf("crc16 cost bandwidth: %.2f MB/s\n",
        1.0 * param->blockSize * param->count / (elapsed / 1e3));  
    printf("crc16 crc: %x\n", crc);
}


void 
crc16_checksum_test(void)
{
    CU_pSuite	suite = NULL;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("crc16", NULL, NULL);

	CU_ADD_TEST(suite, test_crc16_t10dif);
	CU_ADD_TEST(suite, test_crc16_t10dif_seed);
	CU_ADD_TEST(suite, test_crc16_t10dif_copy);

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	CU_get_number_of_failures();
	CU_cleanup_registry();

}


int
main(int argc, char **argv)
{
	
    Crc16PerfParam param = {1024 * 1024 * 1024, 4096, 100000};

    parser_argument(&param, argc, argv);
    if (param.memorySize < param.blockSize) {
        usage();
        exit(0);
    }

    uint8_t *buf = (uint8_t *)malloc(param.memorySize);
    if (buf == NULL) {
        exit(0);
    }
    uint64_t totalBlockCnt = param.memorySize / param.blockSize;
    uint8_t **blocks = (uint8_t **)malloc(sizeof(uint8_t *) * totalBlockCnt);
    for (uint64_t i = 0; i < totalBlockCnt; i++) {
        blocks[i] = buf + param.blockSize * i;
    }
    srand(time(NULL));
    int fd = open("/dev/urandom", O_RDONLY);
#define MAX_READ_LEN (1024 * 1024)
    for (uint32_t i = 0; i < param.memorySize / MAX_READ_LEN; i++) {
        uint32_t readLen = read(fd, buf + i * MAX_READ_LEN, MAX_READ_LEN);
        if (readLen != MAX_READ_LEN) {
            exit(0);
        }
    }
    if (param.type == PERF_TEST) {
        printf("start memory size %" PRIu64 ", block size %" PRIu64 ", count %u\n", param.memorySize, param.blockSize, param.count);
        crc16_perftest(&param, buf, blocks);
    } else if (param.type == CHECKSUM_TEST) {
        crc16_checksum_test();
    }

    free(buf);
    free(blocks);
    return 0;
}
