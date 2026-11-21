/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _KHO_ADAPTER_UAPI_H
#define _KHO_ADAPTER_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * KHO adaptation layer for CRIU.
 *
 * The interface lets a user space checkpoint/restore agent (CRIU):
 *   1. checkpoint all anonymous memory of a process into KHO, and
 *   2. restore that memory into a process after a KHO enabled kexec.
 *
 * Both operations carry a stable, user supplied key.  The key is used as
 * the KHO sub-tree name and must therefore survive the kexec transition.
 */

#define KHO_ADAPTER_KEY_LEN	32

/*
 * struct kho_adapter_req - request for CHECKPOINT/RESTORE/UNPRESERVE.
 * @pid:   thread group id of the process (as seen in the current PID ns).
 * @flags: reserved, must be zero.
 * @key:   NUL terminated, stable identifier.  Allowed characters are
 *         [A-Za-z0-9_.-], length must be 1..KHO_ADAPTER_KEY_LEN-1 and the
 *         key must not be empty.
 */
struct kho_adapter_req {
	__u32 pid;
	__u32 flags;
	char key[KHO_ADAPTER_KEY_LEN];
};

/*
 * struct kho_adapter_status - module statistics.
 * @abi_version:      ABI version implemented by the module.
 * @outgoing_keys:    number of keys currently checkpointed in this kernel.
 * @restored_keys:    number of keys already restored in this kernel.
 * @restored_pages:   number of distinct physical pages restored.
 */
struct kho_adapter_status {
	__u32 abi_version;
	__u32 outgoing_keys;
	__u32 restored_keys;
	__u32 reserved;
	__u64 restored_pages;
};

#define KHO_ADAPTER_IOC_MAGIC	'K'

/*
 * KHO_ADAPTER_IOC_CHECKPOINT - preserve the anonymous memory of @pid.
 *
 * Walks every anonymous private VMA of @pid, pins the resident pages and
 * registers them with KHO.  The virtual address / physical address pairs are
 * serialized into a KHO sub-tree named after @key.
 *
 * The target process has to be stopped by the caller for the duration of the
 * call (and should stay stopped until the kexec transition, otherwise pages
 * may be freed or modified after having been preserved).
 */
#define KHO_ADAPTER_IOC_CHECKPOINT	_IOW(KHO_ADAPTER_IOC_MAGIC, 1, \
					     struct kho_adapter_req)

/*
 * KHO_ADAPTER_IOC_RESTORE - restore anonymous memory of @pid.
 *
 * Must be called in the new kernel after a KHO enabled kexec.  It looks up
 * the sub-tree identified by @key, restores the preserved physical pages and
 * inserts them into the already existing anonymous VMAs of @pid.
 *
 * The caller (CRIU) is responsible for recreating the VMAs at the exact same
 * virtual addresses before issuing this ioctl.
 */
#define KHO_ADAPTER_IOC_RESTORE		_IOW(KHO_ADAPTER_IOC_MAGIC, 2, \
					     struct kho_adapter_req)

/*
 * KHO_ADAPTER_IOC_UNPRESERVE - cancel a checkpoint done in this kernel.
 *
 * Removes the KHO sub-tree, releases the preserved pages and frees the
 * bookkeeping.  Used when a checkpoint/restore cycle is aborted before the
 * actual kexec.
 */
#define KHO_ADAPTER_IOC_UNPRESERVE	_IOW(KHO_ADAPTER_IOC_MAGIC, 3, \
					     struct kho_adapter_req)

#define KHO_ADAPTER_IOC_STATUS		_IOR(KHO_ADAPTER_IOC_MAGIC, 4, \
					     struct kho_adapter_status)

#endif /* _KHO_ADAPTER_UAPI_H */

