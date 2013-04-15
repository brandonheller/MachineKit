/********************************************************************
* Description:  instance.c
*
*               the purpose is to allocate and init the
*               instance data segment, or attach to it if it exists
*               the latter is possible only with userland styles
*               because kernel styles do not allow attaching a shm
*               which was created in userland
*
*               In RTAPI
*               a module to be loaded before rtapi.so/ko
*
*               Unload of the module unmaps the shm segment. It is
*               the last action of a RTAPI session shutdown.
*
*               In ULAPI:
*               Linked into ulapi.so without module headers, it
*               just provides functions for instance initialisation
*               to be used by hal_lib
*
********************************************************************/

#include "config.h"
#include "rtapi.h"		/* RTAPI realtime OS API */
#ifdef RTAPI
#include "rtapi_app.h"		/* RTAPI realtime module decls */
#endif // RTAPI
#include "rtapi_global.h"       /* global_data_t */
#include "rtapi_common.h"
#include "rtapi/shmdrv/shmdrv.h"

#if THREAD_FLAVOR_ID == RTAPI_XENOMAI_KERNEL_ID
#include "xenomai-kernel.h"
#ifdef ULAPI
static RT_HEAP ul_global_heap_desc;
#endif
#ifdef RTAPI
static RT_HEAP global_heap;
#else
#endif // RTAPI
#endif // RTAPI_XENOMAI_KERNEL_ID

#if (THREAD_FLAVOR_ID == RTAPI_POSIX_ID) || (THREAD_FLAVOR_ID == RTAPI_RT_PREEMPT_USER_ID)
#include "rt-preempt-user.h"
#endif // posix, preempt

#if THREAD_FLAVOR_ID == RTAPI_RTAI_KERNEL_ID
#include "rtai-kernel.h"
#endif // RTAPI_RTAI_KERNEL_ID

#if defined(BUILD_SYS_USER_DSO) || (defined(ULAPI) && defined(USE_SHMDRV))
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>		/* IPC_* */
#include <sys/shm.h>		/* shmget() */

static int global_shmid;
#endif // BUILD_SYS_USER_DSO

int rtapi_instance = 0; // exported copy - module params must be static
global_data_t *global_data = NULL;
ringbuffer_t rtapi_message_buffer;   // error ring access strcuture

// this variable is referenced only during startup before global_data
// is attached so keep static

#ifdef RTAPI
static int hal_size = 262000; // HAL_SIZE XXX fixme get from global buildparams.h
static int rt_msg_level = RTAPI_MSG_INFO;	/* initial RT message printing level */ 
static int user_msg_level = RTAPI_MSG_INFO;	/* initial User message printing level */ 

#if (THREAD_FLAVOR_ID == RTAPI_POSIX_ID) || \
    (THREAD_FLAVOR_ID == RTAPI_RT_PREEMPT_USER_ID) || \
    (THREAD_FLAVOR_ID == RTAPI_XENOMAI_USER_ID) 
static int global_shm_init(key_t key, global_data_t **global_data);
static int global_shm_free(int shm_id, global_data_t *global_data);
#endif

static void init_global_data(global_data_t * data, 
			     int instance_id, int hal_size, 
			     int rtlevel, int userlevel, const char *name);
#endif

#ifdef ULAPI
#if (THREAD_FLAVOR_ID == RTAPI_POSIX_ID) || \
    (THREAD_FLAVOR_ID == RTAPI_RT_PREEMPT_USER_ID) || \
    (THREAD_FLAVOR_ID == RTAPI_XENOMAI_USER_ID) || defined(USE_SHMDRV)
static int global_shm_attach(key_t key, global_data_t **global_data);
static int global_shm_detach(global_data_t *global_data);
#endif

static int global_attach_hook();
static int global_detach_hook();
#endif

#ifdef RTAPI

MODULE_AUTHOR("Michael Haberler");
MODULE_DESCRIPTION("Instance segment initialisation");
MODULE_LICENSE("Dual BSD/GPL");

static char *instance_name = "";	/* name of thread */
RTAPI_MP_STRING(instance_name,"name of this instance")
RTAPI_MP_INT(rtapi_instance, "instance ID");
RTAPI_MP_INT(hal_size, "size of the HAL data segment");
RTAPI_MP_INT(rt_msg_level, "debug message level (default=1)");
RTAPI_MP_INT(user_msg_level, "debug message level (default=1)");
EXPORT_SYMBOL(rtapi_instance);
EXPORT_SYMBOL(global_data);

global_data_t *get_global_handle(void)
{
    return global_data;
}
EXPORT_SYMBOL(get_global_handle);

#if defined(BUILD_SYS_USER_DSO)
int rtapi_app_main(void)
#else // kernel
int global_app_main(void)
#endif
{
    int retval;
    global_data_t *result;
    struct shm_status sm;

    rtapi_print_msg(RTAPI_MSG_INFO, "INSTANCE:%d startup %s\n", 
	      rtapi_instance, GIT_VERSION);
#ifdef USE_SHMDRV
    sm.key = OS_KEY(GLOBAL_KEY, rtapi_instance);
    sm.size = sizeof(global_data_t);
    sm.flags = 0;
    if ((retval = shmdrv_create(&sm)) < 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"INSTANCE:%d ERROR: shmdrv_create() returns %d\n",
			rtapi_instance, retval);
	return -EINVAL;
    }
    if ((retval = shmdrv_attach(&sm, (void **)&result)) < 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"INSTANCE:%d ERROR: shmdrv_create() returns %d\n",
			rtapi_instance, retval);
	return -EINVAL;
    }
#else
#if THREAD_FLAVOR_ID == RTAPI_XENOMAI_KERNEL_ID
    if ((retval = rt_heap_create(&global_heap, GLOBAL_HEAP, 
			    sizeof(global_data_t), H_SHARED)) != 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
		  "INSTANCE:%d ERROR: rt_heap_create(global) returns %d\n", 
		  rtapi_instance, retval);
	return -EINVAL;
    }
    if ((retval = rt_heap_alloc(&global_heap, 0, TM_INFINITE,
			   (void **)&result)) != 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
		  "INSTANCE:%d ERROR: rt_heap_alloc(global) returns %d\n", 
		  rtapi_instance, retval);
	return -EINVAL;
    }
#endif

#if THREAD_FLAVOR_ID == RTAPI_RTAI_KERNEL_ID
    result = rtai_kmalloc(OS_KEY(GLOBAL_KEY, rtapi_instance), sizeof(global_data_t));

    // the check for -1 here is because rtai_malloc (in at least
    // rtai 3.6.1, and probably others) has a bug where it
    // sometimes returns -1 on error
    if ((result == (global_data_t*)-1) ||
	(result == NULL)) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"INSTANCE:%d ERROR: rtai_kmalloc(0x%8.8x) failed\n", 
			rtapi_instance,OS_KEY(GLOBAL_KEY, rtapi_instance));
	return -ENOMEM;
    }
#endif
#endif
#if defined(BUILD_SYS_USER_DSO)
#ifdef RUN_WITHOUT_REALTIME
    // try attaching to existing instance - probably a bad idea
    // should be a straight fail here if it exists XXX FIXME
    if ((retval = global_shm_attach(OS_KEY(GLOBAL_KEY,rtapi_instance), &result)) == -1) {
	// nope, create
	if ((retval = global_shm_init(OS_KEY(GLOBAL_KEY,rtapi_instance), &result)) < 0) {
	    return retval; // uh,oh
	}
    }
#else
    if ((retval = global_shm_init(OS_KEY(GLOBAL_KEY,rtapi_instance), &result)) < 0) {
	return retval; // uh,oh
    }
#endif
    global_shmid = retval;
#endif
    global_data = result;

    init_global_data(global_data, rtapi_instance,
		     hal_size, rt_msg_level, user_msg_level, instance_name);

#if defined(BUILD_SYS_USER_DSO)
    // init rtapi
    real_rtapi_app_main();
#endif
    return 0;
}
EXPORT_SYMBOL(global_app_main);

#if defined(BUILD_SYS_USER_DSO)
void rtapi_app_exit(void)
#else
void global_app_exit(void)
#endif
{
    struct shm_status sm;
    int retval;

    rtapi_print_msg(RTAPI_MSG_INFO, "INSTANCE:%d exit  %s\n", 
	      rtapi_instance, GIT_VERSION);

#if defined(BUILD_SYS_USER_DSO)
    real_rtapi_app_exit();
#endif
#ifdef USE_SHMDRV
    sm.key = OS_KEY(GLOBAL_KEY, rtapi_instance);
    sm.size = sizeof(global_data_t);
    sm.flags = 0;
    if ((retval = shmdrv_detach(&sm)) < 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"INSTANCE:%d ERROR: shmdrv_detach() returns %d\n",
			rtapi_instance, retval);
	return -EINVAL;
    }
#else
#if THREAD_FLAVOR_ID == RTAPI_XENOMAI_KERNEL_ID
    rt_heap_delete(&global_heap);
#endif
#if THREAD_FLAVOR_ID == RTAPI_RTAI_KERNEL_ID
    rtai_kfree(OS_KEY(GLOBAL_KEY,rtapi_instance));
#endif
#endif
#if defined(BUILD_SYS_USER_DSO)
    global_shm_free(global_shmid, global_data);
#endif
    // global_data is now dangling reference, but shutting down anyway
    global_data = NULL;
}
EXPORT_SYMBOL(global_app_exit);

#endif // RTAPI

#ifdef ULAPI

int ulapi_main(int instance, int flavor, global_data_t **global)
{
    int retval;
    struct shm_status sm;

#ifdef USE_SHMDRV
    if ((retval = global_shm_attach(OS_KEY(GLOBAL_KEY,rtapi_instance),
				    global))) {
	return retval;
    }
#else
    if ((retval = global_attach_hook())) {
	return retval;
    }
#endif
#ifdef RUN_WITHOUT_REALTIME
    init_global_data(global_data, rtapi_instance,
		     hal_size, rt_msg_level, user_msg_level, "FIXME");
#endif
    *global = global_data;


  if (rtapi_data == NULL) {
	if (shmdrv_available()) {
	    sm.driver_fd = shmdrv_driver_fd();
	    sm.key = OS_KEY(RTAPI_KEY, rtapi_instance);
	    sm.size = sizeof(rtapi_data_t);
	    sm.flags = 0;
	    retval = shmdrv_attach(&sm, (void **) &rtapi_data);
	    if (retval < 0) {
		rtapi_print_msg(RTAPI_MSG_ERR,"rtapi shmdrv attach failed\n");
		return retval;
	    }
	} else {
	    if ((rtapi_data = _rtapi_init_hook()) == NULL)
		return -ENOMEM;
	}
    }


    rtapi_print_msg(RTAPI_MSG_DBG, "ULAPI:%d startup RT msglevel=%d halsize=%d %s\n", 
	      rtapi_instance,
	      global_data->rt_msg_level,
	      global_data->hal_size,
	      GIT_VERSION);
    return 0;
}

void ulapi_exit(void)
{
    int retval;

    rtapi_print_msg(RTAPI_MSG_DBG, "ULAPI:%d %s exit\n",
	      rtapi_instance,
	      GIT_VERSION);
#ifdef USE_SHMDRV
    if ((retval = global_shm_detach(global_data))) {
	return retval;
    }
#else
    global_detach_hook();
#endif
    global_data = NULL;
}

#endif

#ifdef ULAPI

#ifdef USE_SHMDRV

#else
#if THREAD_FLAVOR_ID == RTAPI_XENOMAI_KERNEL_ID
static int global_attach_hook(void) {
    int retval;

    if ((retval = rt_heap_bind(&ul_global_heap_desc, GLOBAL_HEAP, TM_INFINITE))) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"RTAPI: ERROR: global_init: rt_heap_bind() "
			"returns %d\n", 
			retval);
	return -EINVAL;
    }
    if ((retval = rt_heap_alloc(&ul_global_heap_desc, 0,
				TM_NONBLOCK, (void **)&global_data)) != 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"RTAPI: ERROR: rt_heap_alloc(global) returns %d\n", 
			retval);
	return -EINVAL;
    }
    return 0;
}

int global_detach_hook(void) {
    rt_heap_free(&ul_global_heap_desc,global_data);
    return 0;
}
#endif


#if THREAD_FLAVOR_ID == RTAPI_RTAI_KERNEL_ID
static int global_attach_hook() {
    global_data_t *result;
    result = rtai_malloc(OS_KEY(GLOBAL_KEY,rtapi_instance), sizeof(global_data_t));

    // the check for -1 here is because rtai_malloc (in at least
    // rtai 3.6.1, and probably others) has a bug where it
    // sometimes returns -1 on error
    if ((result == (global_data_t*)-1) ||
	(result == NULL)) {
	global_data = NULL;
    }
    global_data = result;
    return 0;
}

int global_detach_hook() {
    rtai_free(OS_KEY(GLOBAL_KEY,rtapi_instance), *p);
}
#endif
#endif

#if (THREAD_FLAVOR_ID == RTAPI_POSIX_ID) || \
    (THREAD_FLAVOR_ID == RTAPI_RT_PREEMPT_USER_ID) || \
    (THREAD_FLAVOR_ID == RTAPI_XENOMAI_USER_ID) 

#ifdef ULAPI
// pretty much the same as saying "#if defined(BUILD_SYS_USER_DSO)"
// because for all three flavors shm segments are treated the same way

static int global_attach_hook() {
    int retval;

    if ((retval = global_shm_attach(GLOBAL_KEY, &global_data)) != -1) {
	rtapi_print_msg(RTAPI_MSG_DBG, "ULAPI:%d attached to existing instance",
		  rtapi_instance);
	global_shmid = retval;
        return 0;
    }
#ifdef RUN_WITHOUT_REALTIME
    if ((retval = global_shm_init(GLOBAL_KEY, &global_data)) != -1) {
	rtapi_print_msg(RTAPI_MSG_DEB, "ULAPI:%d created new instance",
		  rtapi_instance);
	global_shmid = retval;
	return 0;
    }
#endif
    return -EINVAL;
}

static int global_detach_hook() {
    int retval;
    if ((retval = global_shm_detach(global_data)) != 0) {
	rtapi_print_msg(RTAPI_MSG_ERR, "ULAPI:%d failed to detach instance",
		  rtapi_instance);
        return retval;
    }
    global_data = NULL;
    return 0;
}
#endif // USE_SHMDRV
#endif // ULAPI

#if defined(ULAPI) || defined(RUN_WITHOUT_REALTIME)
static int global_shm_attach(key_t key, global_data_t **global_data) 
{
    int shm_id, retval;
    int size = sizeof(global_data_t);
    void *rd;
    struct shm_status sm;

    if (shmdrv_available()) {
	sm.size = sizeof(global_data_t);
	sm.flags = 0;
	sm.key = key;
	sm.driver_fd = shmdrv_driver_fd();
	retval = shmdrv_attach(&sm, &rd);
	if (retval < 0) {
	    rtapi_print_msg(RTAPI_MSG_ERR,"global shmdrv attach failed\n");
	    return retval;
	}
	shm_id = retval;
    } else {

	if ((shm_id = shmget(OS_KEY(key,rtapi_instance), 
			     size, GLOBAL_DATA_PERMISSIONS)) < 0) {
	    rtapi_print_msg(RTAPI_MSG_DBG, 
			    "INSTANCE:%d attach: global data data segment does not exist (0x%8.8x)\n",
			    rtapi_instance, OS_KEY(key,rtapi_instance));
	    return shm_id;
	}

	// and map it into process space 
	rd = shmat(shm_id, 0, 0);
	if (((size_t) rd) == -1) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
			    "INSTANCE:%d: ERROR shmat(%d) failed: %d - %s\n",
			    rtapi_instance, shm_id, 
			    errno, strerror(errno));
	    return -EINVAL;
	}
    }
    *global_data = rd;
    return shm_id;
}
#endif

#ifdef ULAPI
static int global_shm_detach(global_data_t *global_data) 
{
    int retval,r1;
    struct shm_status sm;

    if (shmdrv_available()) {
	sm.size = sizeof(global_data_t);
	sm.flags = 0;
	r1 = shmdrv_detach(&sm, global_data);
	if (r1 < 0) {
	    rtapi_mutex_give(&(rtapi_data->mutex));
	    rtapi_print_msg(RTAPI_MSG_ERR,"global shmdrv detach failed\n");
	    return r1;
	}
    } else {
	/* unmap the shared memory */
	retval = shmdt(global_data);
	if (retval < 0) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
			    "INSTANCE:%d shmdt(%p) failed: %d - %s\n",
			    rtapi_instance,global_data, errno, strerror(errno));      
	    return retval;
	}
    }
    return 0;
}
#endif

#ifdef RTAPI
static int global_shm_init(key_t key, global_data_t **global_data) 
{
    int retval, shm_id;
    int size = sizeof(global_data_t);
    struct shmid_ds d;
    void *rd;
    struct shm_status sm;

    if (shmdrv_available()) {

	sm.driver_fd = shmdrv_driver_fd();
	sm.key = key;
	sm.size = size;
	sm.flags = 0;

	retval = shmdrv_status(&sm); // check if exists

	if (retval) {  // didnt exist, so create
	    retval = shmdrv_create(&sm);
	    if (retval) {
		rtapi_mutex_give(&(rtapi_data->mutex));
		rtapi_print_msg(RTAPI_MSG_ERR,"global shmdrv create failed key=0x%x size=%d\n", key, size);
		return retval;
	    }
#if 0
	} else {
	    rtapi_print_msg(RTAPI_MSG_ERR, 
			    "INSTANCE:%d global data segment already exists (key=0x%8.8x)\n", 
			    rtapi_instance, OS_KEY(key,rtapi_instance));
	    return -EEXIST;
#endif
	}

	// now attach
	retval = shmdrv_attach(&sm, &rd);
	if (retval) {
	    rtapi_mutex_give(&(rtapi_data->mutex));
	    rtapi_print_msg(RTAPI_MSG_ERR,"global shmdrv attached failed key=0x%x size=%d\n", key, size);
	    return retval;
	}
	shm_id = sm.id;
    } else { // classic method - sysv IPC
	if ((shm_id = shmget(OS_KEY(key,rtapi_instance), 
			     size, GLOBAL_DATA_PERMISSIONS)) > -1) {
	    rtapi_print_msg(RTAPI_MSG_ERR, 
			    "INSTANCE:%d global data segment already exists (key=0x%8.8x)\n", 
			    rtapi_instance, OS_KEY(key,rtapi_instance));
	    return -EEXIST;
	}
	if (errno != ENOENT) {
	    rtapi_print_msg(RTAPI_MSG_ERR, 
			    "INSTANCE:%d shmget(): unexpected - %d - %s\n", 
			    rtapi_instance, errno,strerror(errno));
	    return -EINVAL;
	}
	// nope, doesnt exist - create
	if ((shm_id = shmget(OS_KEY(key,rtapi_instance), size, GLOBAL_DATA_PERMISSIONS | IPC_CREAT)) == -1) {
	    rtapi_print_msg(RTAPI_MSG_ERR, 
			    "INSTANCE:%d shmget(key=0x%8.8x, IPC_CREAT): %d - %s\n", 
			    rtapi_instance, key, errno, strerror(errno));
	    return -EINVAL;
	}
	// get actual user/group and drop to ruid/rgid so removing is
	// always possible
	if ((retval = shmctl(shm_id, IPC_STAT, &d)) < 0) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
			    "INSTANCE:%d  shm_ctl(key=0x%8.8x, IPC_STAT) failed: %d - %s\n", 
			    rtapi_instance, key, errno, strerror(errno));  
	    return -EINVAL;
	} else {
	    // drop permissions of shmseg to real userid/group id
	    if (!d.shm_perm.uid) { // uh, root perms 
		d.shm_perm.uid = getuid();
		d.shm_perm.gid = getgid();
		if ((retval = shmctl(shm_id, IPC_SET, &d)) < 0) {
		    rtapi_print_msg(RTAPI_MSG_ERR,
				    "INSTANCE:%d shm_ctl(key=0x%8.8x, IPC_SET) "
				    "failed: %d '%s'\n", 
				    rtapi_instance, key, errno, 
				    strerror(errno));
		    return -EINVAL;
		} 
	    }
	}
  
	// and map it into process space 
	rd = shmat(shm_id, 0, 0);
	if (((size_t) rd) == -1) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
			    "INSTANCE:%d shmat(%d) failed: %d - %s\n",
			    rtapi_instance, shm_id, 
			    errno, strerror(errno));
	    return -EINVAL;
	}

    }
    // Touch each page by zeroing the whole mem
    memset(rd, 0, size);
    *global_data = rd;
    rtapi_print_msg(RTAPI_MSG_DBG, 
		    "INSTANCE:%d global data data segment created (0x%8.8x)\n",
		    rtapi_instance, OS_KEY(key,rtapi_instance));
    return shm_id;

}

static int global_shm_free(int shm_id, global_data_t *global_data) 
{
    struct shmid_ds d;
    int r1, r2;
    struct shm_status sm;

    if (shmdrv_available()) {
	sm.size = sizeof(global_data_t);
	sm.flags = 0;

	r1 = shmdrv_detach(&sm, global_data);
	if (r1) {
	    rtapi_mutex_give(&(rtapi_data->mutex));
	    rtapi_print_msg(RTAPI_MSG_ERR,"global shmdrv detach failed\n");
	    return r1;
	}
    } else {

	/* unmap the shared memory */
	r1 = shmdt(global_data);
	if (r1 < 0) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
			    "INSTANCE:%d shmdt(%p) failed: %d - %s\n",
			    rtapi_instance,global_data, errno, strerror(errno));
	    return -EINVAL;
	}
	/* destroy the shared memory */
	r2 = shmctl(shm_id, IPC_STAT, &d);
	if (r2 < 0) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
			    "INSTANCE:%d shm_ctl(%d, IPC_STAT) failed: %d - %s\n", 
			    rtapi_instance, shm_id, errno, strerror(errno));
	}
	if(r2 == 0 && d.shm_nattch == 0) {
	    r2 = shmctl(shm_id, IPC_RMID, &d);
	    if (r2 < 0) {
		rtapi_print_msg(RTAPI_MSG_ERR,
				"INSTANCE:%d shm_ctl(%d, IPC_RMID) failed: %d - %s\n", 
				rtapi_instance, shm_id, errno, strerror(errno));
	    }
	}
    }
    return 0;
}

#endif // RTAPI
#endif // BUILD_SYS_USER_DSO

// RTAPI, ULAPII, KBUILD, BUILD_SYS_USER_DSO
#ifdef RTAPI
static void init_global_data(global_data_t * data, 
			     int instance_id, int hal_size, 
			     int rt_level, int user_level,
			     const char *name)
{
    printk("init_global_data\n");
    /* has the block already been initialized? */
    if (data->magic == GLOBAL_MAGIC) {
	/* yes, nothing to do */
	printk("init_global_data GLOBAL_MAGIC already set!\n");
	return;
    }
    /* no, we need to init it, grab mutex unconditionally */
    // 'locked' should never happen since this is the first module loaded
    // but you never know what people come up with
    rtapi_mutex_try(&(data->mutex));
    /* set magic number so nobody else init's the block */
    data->magic = GLOBAL_MAGIC;
    /* set version code so other modules can check it */
    data->layout_version = GLOBAL_LAYOUT_VERSION;

    data->instance_id = instance_id;

    // passed in from rtap.so/ko args
    if (strlen(name) == 0) {
	snprintf(data->instance_name,sizeof(data->instance_name), 
		 "inst%d",rtapi_instance);
    } else {
	strncpy(data->instance_name,name, sizeof(data->instance_name));
    }

    // separate message levels for RT and userland
    data->rt_msg_level = rt_level;
    data->user_msg_level = user_level;

    // next value returned by rtapi_init (userland threads)
    // those dont use fixed sized arrays 
    data->next_module_id = 0;

    // tell the others what thread flavor this RTAPI was compiled for
    data->rtapi_thread_flavor = THREAD_FLAVOR_ID;

    // HAL segment size - module param to rtapi 'hal_size=n'
    data->hal_size = hal_size;

    // init the error ring
    rtapi_ringheader_init(&data->rtapi_messages, 0, SIZE_ALIGN(MESSAGE_RING_SIZE), 0);
    memset(&data->rtapi_messages.buf[0], 0, SIZE_ALIGN(MESSAGE_RING_SIZE));

    // sanity check
    //assert(data->rtapi_messages.rhtrailer == &data->rtapi_messages_trailer);

    // prime it
    data->rtapi_messages.refcount = 1;   // rtapi is 'attached'
    data->rtapi_messages.use_wmutex = 1; // hint only

    // demon pids
    data->rtapi_app_pid = 0;
    data->rtapi_msgd_pid = 0;

    // make it accessible
    rtapi_ringbuffer_init(&data->rtapi_messages, &rtapi_message_buffer);

    rtapi_set_logtag("rt");

    /* done, release the mutex */
    rtapi_mutex_give(&(data->mutex));
    return;
}

#endif
