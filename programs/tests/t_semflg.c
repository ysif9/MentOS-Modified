//test
#include "sys/unistd.h"
#include "sys/errno.h"
#include "sys/sem.h"
#include "sys/ipc.h"
#include "stdlib.h"
#include "fcntl.h"
#include "stdio.h"

/*
Testing IPC_NOWAIT flag.
*/

void semid_print(struct semid_ds *temp)
{
    printf("pid, IPC_KEY, Semid, semop, change: %d, %d, %d, %d, %d\n", temp->sem_perm.uid, temp->sem_perm.key, temp->semid, temp->sem_otime, temp->sem_ctime);
    for (int i = 0; i < (temp->sem_nsems); i++) {
        printf("%d semaphore:\n", i + 1);
        printf("value: %d, pid %d, process waiting %d\n", temp->sems[i].sem_val, temp->sems[i].sem_pid, temp->sems[i].sem_zcnt);
    }
}

int main()
{
    struct sembuf op[2];
    union semun arg;
    long ret, id;

    // Create the first semaphore.
    id = semget(IPC_PRIVATE, 1, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    printf("Id: %d\n", id);

    // Set the value of the semaphore in the structure.
    arg.val = 0;
    // Setting the semaphore value.
    printf("Set Value (%d): %d\n", id, arg.val);
    ret = semctl(id, 0, SETVAL, &arg);
    if (ret == -1) {
        perror("Failed to set value of semaphore.");
        return 1;
    }

    op[0].sem_num = 0;
    op[0].sem_op  = 1;
    op[0].sem_flg = IPC_NOWAIT;

    op[1].sem_num = 0;
    op[1].sem_op  = -2;
    op[1].sem_flg = IPC_NOWAIT;

    // Check the value of the semaphore.
    ret = semctl(id, 0, GETVAL, &arg);
    printf("Check Value before(%d): %d\n", id, ret);

    sleep(1);
    if ((ret = semop(id, op, 2)) == -1) {
        perror("Failed to perform semop");
        return 1;
    }

    // Check the value of the semaphore.
    ret = semctl(id, 0, GETVAL, &arg);
    printf("Check Value after(%d): %d\n", id, ret);

    return 0;
}