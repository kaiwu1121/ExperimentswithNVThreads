/*
(c) Copyright [2017] Hewlett Packard Enterprise Development LP

This program is free software; you can redistribute it and/or modify it under 
the terms of the GNU General Public License, version 2 as published by the 
Free Software Foundation.

This program is distributed in the hope that it will be useful, but WITHOUT ANY 
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A 
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with 
this program; if not, write to the Free Software Foundation, Inc., 59 Temple 
Place, Suite 330, Boston, MA 02111-1307 USA

*/


// -*- C++ -*-

/*
 Author: Emery Berger, http://www.cs.umass.edu/~emery
 
 Copyright (c) 2007-8 Emery Berger, University of Massachusetts Amherst.

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

 */
/*
 * @file   xrun.h
 * @brief  The main engine for consistency management, etc.
 * @author Emery Berger <http://www.cs.umass.edu/~emery>
 * @author Tongping Liu <http://www.cs.umass.edu/~tonyliu>
 */

#include <assert.h>
#include <stdint.h>
#include <errno.h>

#include "debug.h"
#include "prof.h"

#include "xrun.h"

#include "logger.h"
#include "nvrecovery.h"

#if defined(__GNUG__)
void initialize() __attribute__((constructor(101)));
void finalize()__attribute__((destructor));
#endif

runtime_data_t *global_data;
runtime_metadata_t *global_metadata;

// Dummy mutex for checkpointing
pthread_mutex_t global_sync_mutex;
pthread_t tid;

static bool initialized = false;
static int stats_fd;
static char stats_filename[FILENAME_MAX]; 
static int metadata_fd;
static char metadata_filename[FILENAME_MAX];

void metadata_init(void){
    // Initialize global metadata
    memset(&global_metadata->metadata, 0, sizeof(struct metadata_t));
    global_metadata->thread_index = 1;
}

void initialize() {
    lprintf("intializing libnvthread");

    init_real_functions();


    // Global stats data shared among threads
    sprintf(stats_filename, "/tmp/statsXXXXXX");
    stats_fd = mkstemp(stats_filename);
    global_data = (runtime_data_t *)mmap(NULL, xdefines::PageSize * 128, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, stats_fd, 0);
    global_data->thread_index = 1;

    // Global metadata shared among threads
    sprintf(metadata_filename, "/tmp/metadataXXXXXX");
    metadata_fd = mkstemp(metadata_filename);
    global_metadata = (runtime_metadata_t *)mmap(NULL, xdefines::PageSize * 128, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, metadata_fd, 0);
    metadata_init();

#ifdef PAGE_DENSITY
    INIT_COUNTER_ARRAY(pagedensity, 4097UL);
#endif
      
    xrun::initialize();
    initialized = true;

    // Mutex for checkpointing            
    pthread_mutex_init(&global_sync_mutex, NULL);
}

void dump_stats(void){
    fprintf(stderr, "\nStatistics information:\n");
    PRINT_LOG_COUNTER(logtimer);
    PRINT_TIMER(serial);
    PRINT_TIMER(logging);
    PRINT_TIMER(diff_calculation);
    PRINT_TIMER(diff_logging);
    PRINT_COUNTER(commit);
    PRINT_COUNTER(transactions);
    PRINT_COUNTER(loggedpages);
    PRINT_COUNTER(dirtypage_modified);
    PRINT_COUNTER(dirtypage_owned);
    PRINT_COUNTER(dirtypage_inserted);
    PRINT_COUNTER(faults);
    PRINT_COUNTER(twinpage);
    PRINT_COUNTER(suspectpage);
    PRINT_COUNTER(slowpage);
    PRINT_COUNTER(lazypage);
    PRINT_COUNTER(shorttrans);

#ifdef PAGE_DENSITY
    PRINT_COUNTER_ARRAY(pagedensity, 4096UL);
    PRINT_COUNTER(pdcount);
    PRINT_COUNTER(dummy);
    char fn[FILENAME_MAX];
    sprintf(fn, "/tmp/pagedensity.csv");
    FILE *fp = fopen(fn, "w");
    if ( fp != NULL) {
        OUTPUT_COUNTER_ARRAY_FILE(pagedensity, 4097UL, fp);
    }
    fclose(fp);
#endif

}

void finalize() {

#ifdef ENABLE_PROFILING
    dump_stats();
#endif
    lprintf("finalizing libnvthread");
    initialized = false;
    xrun::finalize();
    unlink(stats_filename);
    unlink(metadata_filename);
}

extern "C"
{
    bool isCrashed(void){
        return xrun::isCrashed();
    }

    void* nvmalloc(size_t size, char *name) {
        void *ptr;
        ptr = xrun::nvmalloc(size, name);
        lprintf("nvmalloc-ed %s for %zu bytes at %p\n", name, size, ptr);
        return ptr;
    }

    unsigned long nvrecover(void *dest, size_t size, char *name) {
        unsigned long addr;
        addr = xrun::nvrecover(dest, size, name);
        return addr;
    }


    void nvcheckpoint(void){
        pthread_mutex_lock(&global_sync_mutex);       
        pthread_mutex_unlock(&global_sync_mutex);
    }

    void* malloc(size_t sz) {
        void *ptr;
        if ( !initialized ) {
            DEBUG("Pre-initialization malloc call forwarded to mmap");
            ptr = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        } else {
            ptr = xrun::malloc(sz);
        }

        if ( ptr == NULL ) {
            fprintf(stderr, "%d: Out of memory!\n", getpid());
            ::abort();
        }
        return ptr;
    }

    void* calloc(size_t nmemb, size_t sz) {
        void *ptr;
        if ( !initialized ) {
            DEBUG("Pre-initialization calloc call forwarded to mmap");
            ptr = mmap(NULL, sz * nmemb, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
            memset(ptr, 0, sz * nmemb);
        } else {
            ptr = xrun::calloc(nmemb, sz);
        }

        if ( ptr == NULL ) {
            fprintf(stderr, "%d: Out of memory!\n", getpid());
            ::abort();
        }

        memset(ptr, 0, sz * nmemb);

        return ptr;
    }

    void free(void *ptr) {
        //assert(initialized);
        if ( initialized ) {
            xrun::free(ptr);
        } else {
            DEBUG("Pre-initialization free call ignored");
        }
    }

    void* memalign(size_t boundary, size_t size) {
        DEBUG("memalign is not supported");
        return NULL;
    }

    size_t malloc_usable_size(void *ptr) {
        //assert(initialized);
        if ( initialized ) {
            return xrun::getSize(ptr);
        } else {
            DEBUG("Pre-initialization malloc_usable_size call ignored");
        }
        return 0;
    }

    void* realloc(void *ptr, size_t sz) {
        //assert(initialized);
        if ( initialized ) {
            return xrun::realloc(ptr, sz);
        } else {
            DEBUG("Pre-initialization realloc call ignored");
        }
        return NULL;
    }

    int getpid(void) {
        if ( initialized ) {
            return xrun::id();
        }
        return 0;
    }

    int sched_yield(void) {
        return 0;
    }

    void pthread_exit(void *value_ptr) {
        if ( initialized ) {
            xrun::threadDeregister();
        }
        _exit(0);
    }

    int pthread_cancel(pthread_t thread) {
        if ( initialized ) {
            xrun::cancel((void *)thread);
        }
        return 0;
    }

    int pthread_setconcurrency(int) {
        return 0;
    }

    int pthread_attr_init(pthread_attr_t *) {
        return 0;
    }

    int pthread_attr_destroy(pthread_attr_t *) {
        return 0;
    }

    pthread_t pthread_self(void) {
        if ( initialized ) {
            return (pthread_t)xrun::id();
        }
        return 0;
    }

    int pthread_kill(pthread_t thread, int sig) {
        DEBUG("pthread_kill is not supported");
        return 0;
    }

    int sigwait(const sigset_t *set, int *sig) {
        return xrun::sig_wait(set, sig);
    }

    int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *) {
        if ( initialized ) {
            return xrun::mutex_init(mutex);
        }
        return 0;
    }

    int pthread_mutex_lock(pthread_mutex_t *mutex) {
        if ( initialized ) {
            ajlockprintf("%d: pthread locking %p\n", getpid(), mutex);
            xrun::mutex_lock(mutex);
            ajlockprintf("%d: pthread locked %p\n", getpid(), mutex);
        }
        return 0;
    }

    int pthread_mutex_trylock(pthread_mutex_t *mutex) {
        DEBUG("pthread_mutex_trylock is not supported");
        return 0;
    }

    int pthread_mutex_unlock(pthread_mutex_t *mutex) {
        if ( initialized ) {
            ajlockprintf("%d: pthread unlocking %p\n", getpid(), mutex);
            //printf("%d: AJ pthread unlocking %p\n", getpid(), mutex);
            xrun::mutex_unlock(mutex);
            ajlockprintf("%d: pthread unlocked %p\n", getpid(), mutex);
            //printf("%d: AJ pthread unlocked %p\n", getpid(), mutex);
        }        
        if ( xrun::readyToCommitCache() ) {
            ajlockprintf("Ready to commit tmp pageInfo stored in cache to the actual pageInfo\n");
           // printf("AJ Ready to commit tmp pageInfo stored in cache to the actual pageInfo\n");
            xrun::commitCacheBuffer();
        }        
        return 0;
    }

    int pthread_mutex_destory(pthread_mutex_t *mutex) {
        if ( initialized ) {
            return xrun::mutex_destroy(mutex);
        }
        return 0;
    }

    int pthread_attr_getstacksize(const pthread_attr_t *, size_t *s) {
        *s = 1048576UL; // really? FIX ME
        return 0;
    }

    int pthread_mutexattr_destroy(pthread_mutexattr_t *) {
        return 0;
    }
    int pthread_mutexattr_init(pthread_mutexattr_t *) {
        return 0;
    }
    int pthread_mutexattr_settype(pthread_mutexattr_t *, int) {
        return 0;
    }
    int pthread_mutexattr_gettype(const pthread_mutexattr_t *, int *) {
        return 0;
    }
    int pthread_attr_setstacksize(pthread_attr_t *, size_t) {
        return 0;
    }

    int pthread_create(pthread_t *tid, const pthread_attr_t *attr, void* (*fn)(void *), void *arg) {
//      assert(initialized);
        if ( initialized ) {
            *tid = (pthread_t)xrun::spawn(fn, arg);
        }
        return 0;
    }

    int pthread_join(pthread_t tid, void **val) {
//      assert(initialized);
        if ( initialized ) {
            xrun::join((void *)tid, val);
        }
        return 0;
    }

    int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
//      assert(initialized);
        if ( initialized ) {
            xrun::cond_init((void *)cond);
        }
        return 0;
    }

    int pthread_cond_broadcast(pthread_cond_t *cond) {
        //assert(initialized);
        if ( initialized ) {
            xrun::cond_broadcast((void *)cond);
        }
        return 0;
    }

    int pthread_cond_signal(pthread_cond_t *cond) {
        //assert(initialized);
        if ( initialized ) {
            xrun::cond_signal((void *)cond);
        }
        return 0;
    }

    int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
        //assert(initialized);
        if ( initialized ) {
            xrun::cond_wait((void *)cond, (void *)mutex);
        }
        return 0;
    }

    int pthread_cond_destroy(pthread_cond_t *cond) {
        //assert(initialized);
        if ( initialized ) {
            xrun::cond_destroy(cond);
        }
        return 0;
    }

    // Add support for barrier functions
    int pthread_barrier_init(pthread_barrier_t *barrier, const pthread_barrierattr_t *attr, unsigned int count) {
        //assert(initialized);
        if ( initialized ) {
            return xrun::barrier_init(barrier, count);
        }
        return 0;
    }

    int pthread_barrier_destroy(pthread_barrier_t *barrier) {
        //assert(initialized);
        if ( initialized ) {
            return xrun::barrier_destroy(barrier);
        }
        return 0;
    }

    int pthread_barrier_wait(pthread_barrier_t *barrier) {
        //assert(initialized);
        if ( initialized ) {
            return xrun::barrier_wait(barrier);
        }
        return 0;
    }

    ssize_t write(int fd, const void *buf, size_t count) {
        uint8_t *start = (uint8_t *)buf;
        volatile int temp;

        for (size_t i = 0; i < count; i += xdefines::PageSize) {
            temp = start[i];
        }

        temp = start[count - 1];

        return WRAP(write)(fd, buf, count);
    }

    ssize_t read(int fd, void *buf, size_t count) {
        uint8_t *start = (uint8_t *)buf;

        for (size_t i = 0; i < count; i += xdefines::PageSize) {
            start[i] = 0;
        }

        start[count - 1] = 0;

        return WRAP(read)(fd, buf, count);
    }

    // DISABLED
#if 0
    void * mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
        int newflags = flags;

        if (initialized == true && (flags & MAP_PRIVATE)) {
            //		newflags = (flags & ~MAP_PRIVATE) | MAP_SHARED;
            printf("flags %x and newflags %x\n", flags, newflags);
        }
        return WRAP(mmap)(addr, length, prot, newflags, fd, offset);
    }
#endif
}
