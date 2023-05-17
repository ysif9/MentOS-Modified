/// @file sem.c
/// @brief
/// @copyright (c) 2014-2023 This file is distributed under the MIT License.
/// See LICENSE.md for details.
/// @details
/// # 03/04/2023
/// At the moment we have various functions with their description (see
/// comments). The first time that the function semget is called, we are going
/// to generate the list and the first semaphore set with the assumption that we
/// are not given a IPC_PRIVATE key (temporary ofc).
/// We are able to create new semaphores set and generate unique keys
/// (IPC_PRIVATE) with a counter that searches for the first possible key to
/// assign.
/// Temporary idea:
/// - IPC_CREAT flag, it should be working properly, it creates a semaphore set
///   with the given key.
/// - IPC_EXCL flag,  it should be working properly, it returns -1 and sets the
///   errno if the key is already used.
///
/// # 11/04/2023
/// Right now we have a first version of working semaphores in MentOS.
/// We have completed the semctl function and we have implemented the first
/// version of the semop function both user and kernel side.
/// The way it works is pretty straightforward, the user tries to perform an
/// operation and based on the value of the semaphore the kernel returns certain
/// values. If the operation cannot be performed then the user will stay in a
/// while loop. The cycle ends with a positive return value (the operation has
/// been taken care of) or in case of errors.
/// For testing purposes -> you can try the t_semget and the t_sem1 tests. They
/// both use semaphores and blocking / non blocking operations. t_sem1 is also
/// an exercise that was assingned by Professor Drago in the OS course.

// ============================================================================
// Setup the logging for this file (do this before any other include).
#include "sys/kernel_levels.h"          // Include kernel log levels.
#define __DEBUG_HEADER__ "[IPCsem]"     ///< Change header.
#define __DEBUG_LEVEL__  LOGLEVEL_DEBUG ///< Set log level.
#include "io/debug.h"                   // Include debugging functions.
// ============================================================================

#include "sys/sem.h"
#include "ipc/ipc.h"

#include "process/scheduler.h"
#include "process/process.h"
#include "klib/list.h"
#include "sys/errno.h"
#include "stdlib.h"
#include "string.h"
#include "assert.h"
#include "stdio.h"
#include "fcntl.h"

///@brief A value to compute the semid value.
int semid_assign = 0;

/// @brief List of all current active semaphores.
list_t semaphores_list = {
    .head = NULL,
    .tail = NULL,
    .size = 0
};

/// Seed used to generate random numbers.
static int ipc_sem_rseed = 0;
/// The maximum value returned by the rand function.
#define IPC_SEM_RAND_MAX ((1U << 31U) - 1U)
static inline void ipc_sem_srand(int x)
{
    ipc_sem_rseed = x;
}
static inline int ipc_sem_rand()
{
    return ipc_sem_rseed = (ipc_sem_rseed * 1103515245U + 12345U) & IPC_SEM_RAND_MAX;
}

/// @brief Allocates the memory for an array of semaphores.
/// @param nsems number of semaphores in the array.
/// @return a pointer to the allocated array of semaphores.
static inline struct sem *__sem_alloc(int nsems)
{
    // Allocate the memory.
    struct sem *ptr = (struct sem *)kmalloc(sizeof(struct sem) * nsems);
    // Check the allocated memory.
    assert(ptr && "Failed to allocate memory for the array of semaphores.");
    // Clean the memory.
    memset(ptr, 0, sizeof(struct sem));
    return ptr;
}

/// @brief Frees the memory of an array of semaphores.
/// @param ptr the pointer to the array of semaphores.
static inline void __sem_dealloc(struct sem *ptr)
{
    assert(ptr && "Received a NULL pointer.");
    kfree(ptr);
}

/// @brief Initializes a single semaphore.
/// @param ptr the pointer to the single semaphore.
static inline void __sem_init(struct sem *ptr)
{
    ptr->sem_val  = 0;
    ptr->sem_pid  = sys_getpid();
    ptr->sem_zcnt = 0;
}

/// @brief Allocates the memory for a semid structure.
/// @return a pointer to the allocated semid structure.
static inline struct semid_ds *__semid_alloc()
{
    // Allocate the memory.
    struct semid_ds *ptr = (struct semid_ds *)kmalloc(sizeof(struct semid_ds));
    // Check the allocated memory.
    assert(ptr && "Failed to allocate memory for a semid structure.");
    // Clean the memory.
    memset(ptr, 0, sizeof(struct semid_ds));
    return ptr;
}

/// @brief Frees the memory of a semid structure.
/// @param ptr pointer to the semid structure.
static inline void __semid_dealloc(struct semid_ds *ptr)
{
    assert(ptr && "Received a NULL pointer.");
    kfree(ptr);
}

/// @brief Initializes a semid struct.
/// @param ptr the pointer to the semid struct.
/// @param key IPC_KEY associated with the set of semaphores
/// @param nsems number of semaphores to initialize
/// @todo The way we compute the semid is a temporary solution.
static inline void __semid_init(struct semid_ds *ptr, key_t key, int nsems, int semflg)
{
    assert(ptr && "Received a NULL pointer.");
    ptr->sem_perm  = register_ipc(key, semflg & 0x1FF);
    ptr->sem_otime = 0;
    ptr->sem_ctime = 0;
    ptr->sem_nsems = nsems;
    ptr->sems      = __sem_alloc(nsems);
    for (int i = 0; i < nsems; i++) {
        __sem_init(&ptr->sems[i]);
    }
}

/// @brief Searches for the semaphore with the given id.
/// @param semid the id we are searching.
/// @return the semaphore with the given id.
static inline struct semid_ds *__find_semaphore_by_id(int semid)
{
    struct semid_ds *sem_set;
    // Iterate through the list of semaphore set.
    listnode_foreach(listnode, &semaphores_list)
    {
        // Get the current list of semaphore set.
        sem_set = (struct semid_ds *)listnode->value;
        // If semaphore set is valid, check the id.
        if (sem_set && (sem_set->semid == semid))
            return sem_set;
    }
    return NULL;
}

/// @brief Searches for the semaphore with the given key.
/// @param key the key we are searching.
/// @return the semaphore with the given key.
static inline struct semid_ds *__find_semaphore_by_key(key_t key)
{
    struct semid_ds *sem_set;
    // Iterate through the list of semaphore set.
    listnode_foreach(listnode, &semaphores_list)
    {
        // Get the current list of semaphore set.
        sem_set = (struct semid_ds *)listnode->value;
        // If semaphore set is valid, check the id.
        if (sem_set && (sem_set->sem_perm.key == key))
            return sem_set;
    }
    return NULL;
}

long sys_semget(key_t key, int nsems, int semflg)
{
    struct semid_ds *sem_set = NULL;
    // Check if nsems is less than 0 or greater than the maximum number of
    // semaphores per semaphore set.
    if ((nsems < 0) || (nsems > SEM_SET_MAX)) {
        pr_err("Wrong number of semaphores for semaphore set.\n");
        return -EINVAL;
    }

    // Need to find a unique key.
    if (key == IPC_PRIVATE) {
        // Exit when i find a unique key.
        do {
            key = -ipc_sem_rand();
        } while (__find_semaphore_by_key(key));
        // We have a unique key, create the semaphore set.
        sem_set = __semid_alloc();
        // Initialize the semaphore set.
        __semid_init(sem_set, key, nsems, semflg);
        // Add the semaphore set to the list.
        list_insert_front(&semaphores_list, sem_set);
        // Return the id of the semaphore set.
        return sem_set->semid;
    }

    // Get the semaphore set if it exists.
    sem_set = __find_semaphore_by_key(key);

    // Check if a semaphore set with the given key already exists, but nsems is
    // larger than the number of semaphores in that set.
    if (sem_set && (nsems > sem_set->sem_nsems)) {
        pr_err("Wrong number of semaphores for and existing semaphore set.\n");
        return -EINVAL;
    }

    // Check if no semaphore set exists for the given key and semflg did not
    // specify IPC_CREAT.
    if (!sem_set && !(semflg & IPC_CREAT)) {
        pr_err("No semaphore set exists for the given key and semflg did not specify IPC_CREAT.\n");
        return -ENOENT;
    }

    // Check if IPC_CREAT and IPC_EXCL were specified in semflg, but a semaphore
    // set already exists for key.
    if (sem_set && (semflg & IPC_CREAT) && (semflg & IPC_EXCL)) {
        pr_err("IPC_CREAT and IPC_EXCL were specified in semflg, but a semaphore set already exists for key.\n");
        return -EEXIST;
    }

    // Check if the semaphore set exists for the given key, but the calling
    // process does not have permission to access the set.
    if (sem_set && !ipc_valid_permissions(semflg, &sem_set->sem_perm)) {
        pr_err("The semaphore set exists for the given key, but the calling process does not have permission to access the set.\n");
        return -EACCES;
    }

    // If the semaphore set does not exist we need to create a new one.
    if (sem_set == NULL) {
        // Create the semaphore set.
        sem_set = __semid_alloc();
        // Initialize the semaphore set.
        __semid_init(sem_set, key, nsems, semflg);
        // Add the semaphore set to the list.
        list_insert_front(&semaphores_list, sem_set);
    }
    // Return the id of the semaphore set.
    return sem_set->semid;
}

long sys_semop(int semid, struct sembuf *sops, unsigned nsops)
{
    struct semid_ds *sem_set;
    // The semid is less than zero.
    if (semid < 0) {
        pr_err("The semid is less than zero.\n");
        return -EINVAL;
    }
    // The value of nsops is negative.
    if (nsops <= 0) {
        pr_err("The value of nsops is negative.\n");
        return -EINVAL;
    }
    // Search for the semaphore.
    sem_set = __find_semaphore_by_id(semid);
    // The semaphore set doesn't exist.
    if (!sem_set) {
        pr_err("The semaphore set doesn't exist.\n");
        return -EINVAL;
    }
    // The value of sem_num is less than 0 or greater than or equal to the number of semaphores in the set.
    if ((sops->sem_num < 0) || (sops->sem_num >= sem_set->sem_nsems)) {
        pr_err("The value of sem_num is less than 0 or greater than or equal to the number of semaphores in the set.\n");
        return -EFBIG;
    }
    // Check if the semaphore set exists for the given key, but the calling
    // process does not have permission to access the set.
    if (sem_set && !ipc_valid_permissions(O_RDWR, &sem_set->sem_perm)) {
        pr_err("The semaphore set exists for the given key, but the calling process does not have permission to access the set.\n");
        return -EACCES;
    }
    // Update semop time.
    sem_set->sem_otime = sys_time(NULL);

    if (sops->sem_op < 0) {
        // If the operation is negative then we need to check for possible
        // blocking operation. If the value of the sem were to become negative
        // then we return a special value.
        if (sem_set->sems[sops->sem_num].sem_val < (-(sops->sem_op))) {
            // Not allowed.
            return OPERATION_NOT_ALLOWED;
        } else {
            // Otherwise, we can modify the sem_val and all the other parameters
            // of the semaphore.
            sem_set->sems[sops->sem_num].sem_val += (sops->sem_op);
            sem_set->sems[sops->sem_num].sem_pid = sys_getpid();
            sem_set->sem_ctime                   = sys_time(NULL);
            return 1;
        }
    } else {
        // The operation is non negative so we can always do it.
        sem_set->sems[sops->sem_num].sem_val += (sops->sem_op);
        sem_set->sems[sops->sem_num].sem_pid = sys_getpid();
        sem_set->sem_ctime                   = sys_time(NULL);
        return 1;
    }
    return 0;
}

long sys_semctl(int semid, int semnum, int cmd, union semun *arg)
{
    struct semid_ds *sem_set;

    // Search for the semaphore.
    sem_set = __find_semaphore_by_id(semid);
    // The semaphore set doesn't exist.
    if (!sem_set) {
        pr_err("The semaphore set doesn't exist.\n");
        return -EINVAL;
    }

    // Get the calling task.
    task_struct *task = scheduler_get_current_process();
    assert(task && "Failed to get the current running process.");

    switch (cmd) {
    // Remove the semaphore set; any processes blocked is awakened (errno set to
    // EIDRM); no argument required.
    case IPC_RMID:
        if ((sem_set->sem_perm.uid != task->uid) && (sem_set->sem_perm.cuid != task->uid)) {
            pr_err("The calling process is not the creator or the owner of the semaphore set.\n");
            return -EPERM;
        }
        list_remove_node(&semaphores_list, list_find(&semaphores_list, sem_set));
        break;

    // Place a copy of the semid_ds data structure in the buffer pointed to by
    // arg.buf.
    case IPC_STAT:
        if (arg->buf == NULL || arg->buf->sems == NULL) { /*checking the parameters*/
            return -EINVAL;
        }
        //copying all the data
        arg->buf->sem_perm  = sem_set->sem_perm;
        arg->buf->semid     = sem_set->semid;
        arg->buf->sem_otime = sem_set->sem_otime;
        arg->buf->sem_ctime = sem_set->sem_ctime;
        arg->buf->sem_nsems = sem_set->sem_nsems;
        for (int i = 0; i < sem_set->sem_nsems; i++) {
            arg->buf->sems[i].sem_val  = sem_set->sems[i].sem_val;
            arg->buf->sems[i].sem_pid  = sem_set->sems[i].sem_pid;
            arg->buf->sems[i].sem_zcnt = sem_set->sems[i].sem_zcnt;
        }

        return 0;

        //update selected fields of the semid_ds using values in the buffer pointed to by arg.buf.
        //case IPC_SET:
        /* code */
        //break;

    //the value of the semnum-th semaphore in the set is initialized to the value specified in arg.val.
    case SETVAL:
        if (semnum < 0 || semnum >= (sem_set->sem_nsems)) { //if the index is valid
            return -EINVAL;
        }

        if (arg->val < 0) { //checking if the value is valid
            return -EINVAL;
        }
        //setting the values
        sem_set->sem_ctime            = sys_time(NULL);
        sem_set->sems[semnum].sem_val = arg->val;
        return 0;

    //returns the value of the semnum-th semaphore in the set specified by semid; no argument required.
    case GETVAL:
        if (semnum < 0 || semnum >= (sem_set->sem_nsems)) { //if the index is valid
            return -EINVAL;
        }

        return sem_set->sems[semnum].sem_val;

    //initialize all semaphore in the set referred to by semid, using the values supplied in the array pointed to by arg.array.
    case SETALL:
        if (arg->array == NULL) { /*checking parameters*/
            return -EINVAL;
        }
        for (int i = 0; i < sem_set->sem_nsems; i++) { //setting all the values
            sem_set->sems[i].sem_val = arg->array[i];
        }
        sem_set->sem_ctime = sys_time(NULL);
        return 0;

    //retrieve the values of all of the semaphores in the set referred to by semid, placing them in the array pointed to by arg.array.
    case GETALL:
        if (arg->array == NULL) { //checking if the argument passed is valid
            return -EINVAL;
        }
        for (int i = 0; i < sem_set->sem_nsems; i++) {
            arg->array[i] = sem_set->sems[i].sem_val;
        }
        return 0;

    //return the process ID of the last process to perform a semop on the semnum-th semaphore.
    case GETPID:
        if (semnum < 0 || semnum >= (sem_set->sem_nsems)) { //if the index is valid
            return -EINVAL;
        }
        return sem_set->sems[semnum].sem_pid;

    //return the number of processes currently waiting for the value of the semnum-th semaphore to become 0.
    case GETZCNT:
        if (semnum < 0 || semnum >= (sem_set->sem_nsems)) { //if the index is valid
            return -EINVAL;
        }
        return sem_set->sems[semnum].sem_zcnt;

    //return the number of semaphores in the set.
    case GETNSEMS:
        return sem_set->sem_nsems;

    //not a valid argument.
    default:
        return -EINVAL;
    }

    return 0;
}

ssize_t procipc_sem_read(vfs_file_t *file, char *buf, off_t offset, size_t nbyte)
{
    if (!file) {
        pr_err("Received a NULL file.\n");
        return -ENOENT;
    }
    size_t buffer_len = 0, read_pos = 0, write_count = 0, ret = 0;
    struct semid_ds *entry = NULL;
    char buffer[BUFSIZ];

    // Prepare a buffer.
    memset(buffer, 0, BUFSIZ);
    // Prepare the header.
    ret = sprintf(buffer, "key      semid perms      nsems   uid   gid  cuid  cgid      otime      ctime\n");

    // Iterate through the list.
    if (semaphores_list.size > 0) {
        listnode_foreach(listnode, &semaphores_list)
        {
            // Get the entry.
            entry = ((struct semid_ds *)listnode->value);
            ret += sprintf(
                buffer + ret, "%8d %5d %10d %7d %5d %4d %5d %9d %10d %d\n",
                abs(entry->sem_perm.key),
                entry->semid,
                entry->sem_perm.mode,
                entry->sem_nsems,
                entry->sem_perm.uid,
                entry->sem_perm.gid,
                entry->sem_perm.cuid,
                entry->sem_perm.cgid,
                entry->sem_otime,
                entry->sem_ctime);
        }
    }
    sprintf(buffer + ret, "\n");

    // Perform read.
    buffer_len = strlen(buffer);
    read_pos   = offset;
    if (read_pos < buffer_len) {
        while ((write_count < nbyte) && (read_pos < buffer_len)) {
            buf[write_count] = buffer[read_pos];
            // Move the pointers.
            ++read_pos, ++write_count;
        }
    }
    return write_count;
}

///! @endcond
