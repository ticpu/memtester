/*
 * memtester version 4
 *
 * Very simple but very effective user-space memory tester.
 * Originally by Simon Kirby <sim@stormix.com> <sim@neato.org>
 * Version 2 by Charles Cazabon <charlesc-memtester@pyropus.ca>
 * Version 3 not publicly released.
 * Version 4 rewrite:
 * Copyright (C) 2004-2020 Charles Cazabon <charlesc-memtester@pyropus.ca>
 * Licensed under the terms of the GNU General Public License version 2 (only).
 * See the file COPYING for details.
 *
 */

#define __version__ "4.5.1"

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

#include "types.h"
#include "sizes.h"
#include "tests.h"
#include "output.h"

#define EXIT_FAIL_NONSTARTER    0x01
#define EXIT_FAIL_ADDRESSLINES  0x02
#define EXIT_FAIL_OTHERTEST     0x04

struct test tests[] = {
    { "Random Value", test_random_value },
    { "Compare XOR", test_xor_comparison },
    { "Compare SUB", test_sub_comparison },
    { "Compare MUL", test_mul_comparison },
    { "Compare DIV",test_div_comparison },
    { "Compare OR", test_or_comparison },
    { "Compare AND", test_and_comparison },
    { "Sequential Increment", test_seqinc_comparison },
    { "Solid Bits", test_solidbits_comparison },
    { "Block Sequential", test_blockseq_comparison },
    { "Checkerboard", test_checkerboard_comparison },
    { "Bit Spread", test_bitspread_comparison },
    { "Bit Flip", test_bitflip_comparison },
    { "Walking Ones", test_walkbits1_comparison },
    { "Walking Zeroes", test_walkbits0_comparison },
#ifdef TEST_NARROW_WRITES
    { "8-bit Writes", test_8bit_wide_random },
    { "16-bit Writes", test_16bit_wide_random },
#endif
    { NULL, NULL }
};

typedef struct memory_alloc {
    volatile void *buf;
    volatile void *aligned;
    size_t wantbytes;
    int bufsize;
    int do_mlock;
    int use_hugepages;
    size_t pagesize;
    ptrdiff_t pagesizemask;
} memory_alloc_t;

/* Sanity checks and portability helper macros. */
#ifdef _SC_VERSION
void check_posix_system(void) {
    if (sysconf(_SC_VERSION) < 198808L) {
        fprintf(stderr, "A POSIX system is required.  Don't be surprised if "
            "this craps out.\n");
        fprintf(stderr, "_SC_VERSION is %lu\n", sysconf(_SC_VERSION));
    }
}
#else
#define check_posix_system()
#endif

#ifdef _SC_PAGE_SIZE
void memtester_pagesize(memory_alloc_t *alloc) {
    size_t pagesize = sysconf(_SC_PAGE_SIZE);
    if (alloc->use_hugepages) {
        pagesize = 2L * 1024 * 1024;
    } else {
        pagesize = sysconf(_SC_PAGE_SIZE);
        if (pagesize == -1) {
            perror("get page size failed");
            exit(EXIT_FAIL_NONSTARTER);
        }
    }
    printf("pagesize is %ld\n", (long) pagesize);
    alloc->pagesize = pagesize;
    alloc->pagesizemask = (ptrdiff_t) ~(pagesize - 1);
}
#else
int memtester_pagesize(memory_alloc_t *alloc) {
    printf("sysconf(_SC_PAGE_SIZE) not supported; using pagesize of 8192\n");
    alloc->pagesize = 8192;
    alloc->pagesizemask = (ptrdiff_t) ~(alloc->pagesize - 1);
}
#endif

/* Some systems don't define MAP_LOCKED.  Define it to 0 here
   so it's just a no-op when ORed with other constants. */
#ifndef MAP_LOCKED
  #define MAP_LOCKED 0
#endif

/* Function declarations */
int usage(char *me);

/* Global vars - so tests have access to this information */
int use_phys = 0;
off_t physaddrbase = 0;

/* Function definitions */
int usage(char *me) {
    fprintf(stderr, "\n"
            "Usage: %s [-H] [-p physaddrbase [-d device] [-u]] <mem>[B|K|M|G] [loops]\n",
            me);
    return EXIT_FAIL_NONSTARTER;
}

long get_free_hugepages(void) {
	FILE *file = fopen("/sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages", "r");
	long free_hugepages = 0;

	if (file == NULL) {
		perror("Error opening file");
		return -1;
	}

	if (fscanf(file, "%ld", &free_hugepages) != 1) {
		perror("Error reading from file");
		fclose(file);
		return -1;
	}

	fclose(file);

	return free_hugepages;
}

void alloc_using_hugepages(memory_alloc_t *alloc) {
	long free_hugepages = get_free_hugepages();

	if (alloc->wantbytes % alloc->pagesize != 0) {
        alloc->wantbytes = ((alloc->wantbytes / alloc->pagesize) + 1) * alloc->pagesize;
    }

    while (!alloc->buf && alloc->wantbytes) {
        alloc->buf = mmap(NULL, alloc->wantbytes, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);

        if (alloc->buf == MAP_FAILED && errno == ENOMEM) {
			alloc->buf = NULL;

			if (free_hugepages > 0 && alloc->wantbytes > (free_hugepages * alloc->pagesize)) {
				alloc->wantbytes = free_hugepages * alloc->pagesize;
			} else {
				alloc->wantbytes -= alloc->pagesize;
			}

            if (alloc->wantbytes < alloc->pagesize) {
                fprintf(stderr, "insufficient memory available for huge page allocation\n");
                break;
            }
        } else if (alloc->buf == MAP_FAILED) {
			perror("mmap failed for huge pages, ");
			exit(EXIT_FAILURE);
		} else {
			printf("got  %lluMB (%llu bytes)", (ull) alloc->wantbytes >> 20,
				   (ull) alloc->wantbytes);
		}
    }
	printf("\n");
}

int alloc_using_malloc(memory_alloc_t *alloc, const size_t wantbytes_orig) {
    while (!alloc->buf && alloc->wantbytes) {
        alloc->buf = (void volatile *) malloc(alloc->wantbytes);
        if (!alloc->buf) alloc->wantbytes -= alloc->pagesize;
    }
    alloc->bufsize = alloc->wantbytes;
    printf("got  %lluMB (%llu bytes)", (ull) alloc->wantbytes >> 20,
        (ull) alloc->wantbytes);
    fflush(stdout);
    if (alloc->do_mlock) {
        printf(", trying mlock ...");
        fflush(stdout);
        if ((size_t) alloc->buf % alloc->pagesize) {
            /* printf("aligning to page -- was 0x%tx\n", buf); */
            alloc->aligned = (void volatile *) ((size_t) alloc->buf &
                    alloc->pagesizemask) + alloc->pagesize;
            /* printf("  now 0x%tx -- lost %d bytes\n", aligned,
             *      (size_t) aligned - (size_t) buf);
             */
            alloc->bufsize -= ((size_t) alloc->aligned - (size_t) alloc->buf);
        } else {
            alloc->aligned = alloc->buf;
        }
        /* Try mlock */
        if (mlock((void *) alloc->aligned, alloc->bufsize) < 0) {
            switch(errno) {
                case EAGAIN: /* BSDs */
                    printf("over system/pre-process limit, reducing...\n");
                    free((void *) alloc->buf);
                    alloc->buf = NULL;
                    alloc->wantbytes -= alloc->pagesize;
                    break;
                case ENOMEM:
                    printf("too many pages, reducing...\n");
                    free((void *) alloc->buf);
                    alloc->buf = NULL;
                    alloc->wantbytes -= alloc->pagesize;
                    break;
                case EPERM:
                    printf("insufficient permission.\n");
                    printf("Trying again, unlocked:\n");
                    alloc->do_mlock = 0;
                    free((void *) alloc->buf);
                    alloc->buf = NULL;
                    alloc->wantbytes = wantbytes_orig;
                    break;
                default:
                    printf("failed for unknown reason.\n");
                    alloc->do_mlock = 0;
                    return 1;
            }
        } else {
            printf("locked.\n");
            return 1;
        }
    } else {
        printf("\n");
        return 1;
    }

    return 0;
}

int main(int argc, char **argv) {
    ul loops, loop, i;
    size_t wantraw, wantmb, wantbytes_orig, halflen, count;
    char *memsuffix, *addrsuffix, *loopsuffix;
    ulv *bufa, *bufb;
    int done_mem = 0;
    int exit_code = 0;
    int memfd, opt, memshift;
    size_t maxbytes = -1; /* addressable memory, in bytes */
    size_t maxmb = (maxbytes >> 20) + 1; /* addressable memory, in MB */
    /* Device to mmap memory from with -p, default is normal core */
    char *device_name = "/dev/mem";
    struct stat statbuf;
    int device_specified = 0;
    char *env_testmask;
    ul testmask = 0;
    int o_flags = O_RDWR | O_SYNC;
    memory_alloc_t alloc = {
            .buf = NULL,
            .aligned = NULL,
            .wantbytes = 0,
            .bufsize = 0,
            .use_hugepages = 0,
            .do_mlock = 1,
    };

    memtester_pagesize(&alloc);
    out_initialize();

    printf("memtester version " __version__ " (%d-bit)\n", UL_LEN);
    printf("Copyright (C) 2001-2020 Charles Cazabon.\n");
    printf("Licensed under the GNU General Public License version 2 (only).\n");
    printf("\n");
    check_posix_system();
    printf("pagesizemask is 0x%tx\n", alloc.pagesizemask);

    /* If MEMTESTER_TEST_MASK is set, we use its value as a mask of which
       tests we run.
     */
    if ((env_testmask = getenv("MEMTESTER_TEST_MASK"))) {
        errno = 0;
        testmask = strtoul(env_testmask, 0, 0);
        if (errno) {
            fprintf(stderr, "error parsing MEMTESTER_TEST_MASK %s: %s\n",
                    env_testmask, strerror(errno));
            return usage(argv[0]);
        }
        printf("using testmask 0x%lx\n", testmask);
    }

    while ((opt = getopt(argc, argv, "Hp:d:u")) != -1) {
        switch (opt) {
            case 'H':
                alloc.use_hugepages = 1;
                memtester_pagesize(&alloc);
                break;
            case 'p':
                errno = 0;
                physaddrbase = (off_t) strtoull(optarg, &addrsuffix, 16);
                if (errno != 0) {
                    fprintf(stderr,
                            "failed to parse physaddrbase arg; should be hex "
                            "address (0x123...)\n");
                    return usage(argv[0]);
                }
                if (*addrsuffix != '\0') {
                    /* got an invalid character in the address */
                    fprintf(stderr,
                            "failed to parse physaddrbase arg; should be hex "
                            "address (0x123...)\n");
                    return usage(argv[0]);
                }
                if (physaddrbase & (alloc.pagesize - 1)) {
                    fprintf(stderr,
                            "bad physaddrbase arg; does not start on page "
                            "boundary\n");
                    return usage(argv[0]);
                }
                /* okay, got address */
                use_phys = 1;
                break;
            case 'd':
                if (stat(optarg,&statbuf)) {
                    fprintf(stderr, "can not use %s as device: %s\n", optarg,
                            strerror(errno));
                    return usage(argv[0]);
                } else {
                    if (!S_ISCHR(statbuf.st_mode)) {
                        fprintf(stderr, "can not mmap non-char device %s\n",
                                optarg);
                        return usage(argv[0]);
                    } else {
                        device_name = optarg;
                        device_specified = 1;
                    }
                }
                break;
            case 'u':
                o_flags &= ~O_SYNC;
                break;
            default: /* '?' */
                return usage(argv[0]);
        }
    }

    if (device_specified && !use_phys) {
        fprintf(stderr,
                "for mem device, physaddrbase (-p) must be specified\n");
        return usage(argv[0]);
    }

    if (optind >= argc) {
        fprintf(stderr, "need memory argument, in MB\n");
        return usage(argv[0]);
    }

    errno = 0;
    wantraw = (size_t) strtoul(argv[optind], &memsuffix, 0);
    if (errno != 0) {
        fprintf(stderr, "failed to parse memory argument");
        return usage(argv[0]);
    }
    switch (*memsuffix) {
        case 'G':
        case 'g':
            memshift = 30; /* gigabytes */
            break;
        case 'M':
        case 'm':
            memshift = 20; /* megabytes */
            break;
        case 'K':
        case 'k':
            memshift = 10; /* kilobytes */
            break;
        case 'B':
        case 'b':
            memshift = 0; /* bytes*/
            break;
        case '\0':  /* no suffix */
            memshift = 20; /* megabytes */
            break;
        default:  /* bad suffix */
            return usage(argv[0]);
    }
    wantbytes_orig = alloc.wantbytes = ((size_t) wantraw << memshift);
    wantmb = (wantbytes_orig >> 20);
    optind++;
    if (wantmb > maxmb) {
        fprintf(stderr, "This system can only address %llu MB.\n", (ull) maxmb);
        exit(EXIT_FAIL_NONSTARTER);
    }
    if (alloc.wantbytes < alloc.pagesize) {
        fprintf(stderr, "bytes %ld < pagesize %ld -- memory argument too large?\n",
                alloc.wantbytes, alloc.pagesize);
        exit(EXIT_FAIL_NONSTARTER);
    }

    if (optind >= argc) {
        loops = 0;
    } else {
        errno = 0;
        loops = strtoul(argv[optind], &loopsuffix, 0);
        if (errno != 0) {
            fprintf(stderr, "failed to parse number of loops");
            return usage(argv[0]);
        }
        if (*loopsuffix != '\0') {
            fprintf(stderr, "loop suffix %c\n", *loopsuffix);
            return usage(argv[0]);
        }
    }

    printf("want %lluMB (%llu bytes)\n", (ull) wantmb, (ull) alloc.wantbytes);
    alloc.buf = NULL;

    if (use_phys) {
        memfd = open(device_name, o_flags);
        if (memfd == -1) {
            fprintf(stderr, "failed to open %s for physical memory: %s\n",
                    device_name, strerror(errno));
            exit(EXIT_FAIL_NONSTARTER);
        }
        alloc.buf = (void volatile *) mmap(0, alloc.wantbytes, PROT_READ | PROT_WRITE,
                                     MAP_SHARED | MAP_LOCKED, memfd,
                                     physaddrbase);
        if (alloc.buf == MAP_FAILED) {
            fprintf(stderr, "failed to mmap %s for physical memory: %s\n",
                    device_name, strerror(errno));
            exit(EXIT_FAIL_NONSTARTER);
        }

        if (mlock((void *) alloc.buf, alloc.wantbytes) < 0) {
            fprintf(stderr, "failed to mlock mmap'ed space\n");
            alloc.do_mlock = 0;
        }

        alloc.bufsize = alloc.wantbytes; /* accept no less */
        alloc.aligned = alloc.buf;
        done_mem = 1;
    }

    if (alloc.use_hugepages) {
        alloc_using_hugepages(&alloc);
    } else {
        while (!done_mem) {
            done_mem = alloc_using_malloc(&alloc, wantbytes_orig);
        }
    }

    if (!alloc.do_mlock) fprintf(stderr, "Continuing with unlocked memory; testing "
                           "will be slower and less reliable.\n");

    /* Do alighnment here as well, as some cases won't trigger above if you
       define out the use of mlock() (cough HP/UX 10 cough). */
    if ((size_t) alloc.buf % alloc.pagesize) {
        /* printf("aligning to page -- was 0x%tx\n", buf); */
        alloc.aligned = (void volatile *) ((size_t) alloc.buf
                & alloc.pagesizemask) + alloc.pagesize;
        /* printf("  now 0x%tx -- lost %d bytes\n", aligned,
         *      (size_t) aligned - (size_t) buf);
         */
        alloc.bufsize -= ((size_t) alloc.aligned - (size_t) alloc.buf);
    } else {
        alloc.aligned = alloc.buf;
    }

    halflen = alloc.bufsize / 2;
    count = halflen / sizeof(ul);
    bufa = (ulv *) alloc.aligned;
    bufb = (ulv *) ((size_t) alloc.aligned + halflen);

    for(loop=1; ((!loops) || loop <= loops); loop++) {
        printf("Loop %lu", loop);
        if (loops) {
            printf("/%lu", loops);
        }
        printf(":\n");
        printf("  %-20s: ", "Stuck Address");
        fflush(stdout);
        if (!test_stuck_address(alloc.aligned, alloc.bufsize / sizeof(ul))) {
             printf("ok\n");
        } else {
            exit_code |= EXIT_FAIL_ADDRESSLINES;
        }
        for (i=0;;i++) {
            if (!tests[i].name) break;
            /* If using a custom testmask, only run this test if the
               bit corresponding to this test was set by the user.
             */
            if (testmask && (!((1 << i) & testmask))) {
                continue;
            }
            printf("  %-20s: ", tests[i].name);
            fflush(stdout);
            if (!tests[i].fp(bufa, bufb, count)) {
                printf("ok\n");
            } else {
                exit_code |= EXIT_FAIL_OTHERTEST;
            }
            fflush(stdout);
            /* clear buffer */
            memset((void *) alloc.buf, 255, alloc.wantbytes);
        }
        printf("\n");
        fflush(stdout);
    }
    if (alloc.do_mlock) munlock((void *) alloc.aligned, alloc.bufsize);
    printf("Done.\n");
    fflush(stdout);
    exit(exit_code);
}
