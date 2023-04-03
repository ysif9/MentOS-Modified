/// @file ipc.h
/// @brief Inter-Process Communication (IPC) structures.
/// @copyright (c) 2014-2023 This file is distributed under the MIT License.
/// See LICENSE.md for details.

#pragma once


#define IPC_CREAT  01000 ///< Create key if key does not exist.
#define IPC_EXCL   02000 ///< Fail if key exists.
#define IPC_NOWAIT 04000 ///< Return error on wait.
#define IPC_RMID   0     ///< Remove identifier.
#define IPC_SET    1     ///< Set `ipc_perm' options.
#define IPC_STAT   2     ///< Get `ipc_perm' options.
#define IPC_INFO   3     ///< See ipcs.
#define IPC_PRIVATE 4    ///< assures getting a new ipc_key.

/// @brief Permission details of an IPC object.
struct ipc_perm {
    /// Key supplied to msgget(2).
    key_t __key;
    /// Effective UID of owner.
    uid_t uid;
    /// Effective GID of owner.
    gid_t gid;
    /// Effective UID of creator.
    uid_t cuid;
    /// Effective GID of creator.
    gid_t cgid;
    /// Permissions.
    unsigned short mode;
    /// Sequence number.
    unsigned short __seq;
};


/// @brief Returns a possible key
/// @param path file path
/// @param id integer
/// @return IPC key
key_t ftok( char *path, int id);

