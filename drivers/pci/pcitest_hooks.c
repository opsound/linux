// SPDX-License-Identifier: GPL-2.0
/*
 * Test-only hooks for the pciehp/vfio/PCI-reset deadlock reproducers of
 * https://github.com/opsound/pciehp-reset-lab.  Not for upstream.  Every
 * entry takes a BDF of the form DDDD:BB:SS.F where it names a device.
 *
 * /sys/kernel/debug/pcitest/hold_rescan_ms: "N" holds pci_rescan_remove_lock
 *	for N ms in the writing task, emulating a slow topology change.
 * /sys/kernel/debug/pcitest/host_reset: "BDF MS" takes pci_rescan_remove_lock,
 *	sleeps MS ms, then calls pci_bus_error_reset() on the port, emulating
 *	pci_host_reset_root_port() (drivers/pci/controller/pci-host-common.c).
 * /sys/kernel/debug/pcitest/host_reset_nolock: "BDF MS" sleeps MS ms, then
 *	calls pci_bus_error_reset() without pci_rescan_remove_lock, emulating
 *	pci_host_reset_root_port() without the lock, and aer_root_reset().
 * /sys/kernel/debug/pcitest/slot_reset: "BDF" calls the ->reset_slot() op of
 *	the hotplug slot below the port directly, under pci_cfg_access_lock()
 *	of the port but without pci_slot_mutex or device locks, to observe
 *	reset_lock exclusion alone.
 * /sys/kernel/debug/pcitest/disconnect: "BDF" marks the device and its
 *	subordinates disconnected, as pciehp_unconfigure_device() does before
 *	taking pci_rescan_remove_lock on surprise removal.  Lets a safe removal
 *	in QEMU emulate a surprise one.
 * /sys/kernel/debug/pcitest2/slot_reset: "BDF" like pcitest/slot_reset but
 *	without pci_cfg_access_lock(), so that reset_lock is the only lock the
 *	reset can block on.  Returns -ENOTTY if there is no hotplug slot.
 * /sys/kernel/debug/pcitest/aer_reset, aer_host_reset: "BDF [MS]" like err_reset/host_reset,
 *   but through pcie_do_recovery(), so drivers restore their devices
 * /sys/kernel/debug/pcitest/err_reset: "BDF [MS]" sleeps MS
 *	ms, then pci_bus_error_reset() without pci_rescan_remove_lock, like
 *	AER/DPC recovery (same as host_reset_nolock, different log line).
 * /sys/kernel/debug/pcitest/reset_bus: "BDF [MS]" sleeps MS
 *	ms, then pci_reset_bus() on the device (like VFIO_DEVICE_PCI_HOT_RESET).
 *
 * Test-only delays, present only if hooks/apply_knobs.py patched the call
 * site (the TEST ONLY commit of the lab branches):
 * /sys/kernel/debug/pcitest/add_delay_ms (u32): sleep in
 *	pci_device_add() between list_add_tail() to bus->devices and
 *	device_add(), for PCIe endpoints; logs "pcitest: add delay BDF N ms".
 * /sys/kernel/debug/pcitest/unbind_delay_ms (u32): sleep in
 *	pci_unbind_dev() (unbind-before-remove series only) after the has_vfs
 *	check of a PF without VFs; logs "pcitest: has_vfs=0, sleeping".
 */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pci_hotplug.h>
#include <linux/uaccess.h>
#include "pci.h"

/* Parse "DDDD:BB:SS.F [MS]" from user space; returns a referenced pci_dev. */
static struct pci_dev *pcitest_get_dev(const char __user *ubuf, size_t len,
				       unsigned int *ms, int *err)
{
	unsigned int dom, bus, slot, fn, dummy;
	struct pci_dev *dev;
	char buf[64];

	*err = -EINVAL;
	if (len >= sizeof(buf))
		return NULL;
	*err = -EFAULT;
	if (copy_from_user(buf, ubuf, len))
		return NULL;
	buf[len] = 0;
	*err = -EINVAL;
	if (ms)
		*ms = 0;
	if (sscanf(buf, "%x:%x:%x.%x %u", &dom, &bus, &slot, &fn, ms ? ms : &dummy) < 4)
		return NULL;
	*err = -ENODEV;
	dev = pci_get_domain_bus_and_slot(dom, bus, PCI_DEVFN(slot, fn));
	if (dev)
		*err = 0;
	return dev;
}

static ssize_t hold_write(struct file *f, const char __user *ubuf, size_t len,
			  loff_t *ppos)
{
	unsigned int ms;
	int ret = kstrtouint_from_user(ubuf, len, 0, &ms);

	if (ret)
		return ret;
	pr_info("pcitest: hold_rescan %u ms: locking\n", ms);
	pci_lock_rescan_remove();
	pr_info("pcitest: hold_rescan: locked\n");
	msleep(ms);
	pci_unlock_rescan_remove();
	pr_info("pcitest: hold_rescan: unlocked\n");
	return len;
}

static ssize_t host_reset_write(struct file *f, const char __user *ubuf,
				size_t len, loff_t *ppos)
{
	struct pci_dev *dev;
	unsigned int ms;
	int ret;

	dev = pcitest_get_dev(ubuf, len, &ms, &ret);
	if (!dev)
		return ret;
	pr_info("pcitest: host_reset %s: locking rescan\n", pci_name(dev));
	pci_lock_rescan_remove();
	pr_info("pcitest: host_reset %s: locked, sleeping %u ms\n", pci_name(dev), ms);
	msleep(ms);
	pr_info("pcitest: host_reset %s: pci_bus_error_reset\n", pci_name(dev));
	ret = pci_bus_error_reset(dev);
	pci_unlock_rescan_remove();
	pr_info("pcitest: host_reset %s: done ret=%d\n", pci_name(dev), ret);
	pci_dev_put(dev);
	return ret ? ret : len;
}

static ssize_t host_reset_nolock_write(struct file *f, const char __user *ubuf,
				       size_t len, loff_t *ppos)
{
	struct pci_dev *dev;
	unsigned int ms;
	int ret;

	dev = pcitest_get_dev(ubuf, len, &ms, &ret);
	if (!dev)
		return ret;
	msleep(ms);
	pr_info("pcitest: host_reset_nolock %s: pci_bus_error_reset\n", pci_name(dev));
	ret = pci_bus_error_reset(dev);
	pr_info("pcitest: host_reset_nolock %s: done ret=%d\n", pci_name(dev), ret);
	pci_dev_put(dev);
	return ret ? ret : len;
}

static struct hotplug_slot *pcitest_hotplug_slot(struct pci_dev *dev)
{
	struct hotplug_slot *hp = NULL;
	struct pci_slot *ps;

	mutex_lock(&pci_slot_mutex);
	if (dev->subordinate)
		list_for_each_entry(ps, &dev->subordinate->slots, list)
			if (ps->hotplug)
				hp = ps->hotplug;
	mutex_unlock(&pci_slot_mutex);
	return hp;
}

static ssize_t slot_reset_write(struct file *f, const char __user *ubuf,
				size_t len, loff_t *ppos)
{
	struct hotplug_slot *hp;
	struct pci_dev *dev;
	int ret;

	dev = pcitest_get_dev(ubuf, len, NULL, &ret);
	if (!dev)
		return ret;
	ret = -ENODEV;
	hp = pcitest_hotplug_slot(dev);
	if (hp && hp->ops->reset_slot) {
		pr_info("pcitest: slot_reset %s: start\n", pci_name(dev));
		pci_cfg_access_lock(dev);
		ret = hp->ops->reset_slot(hp, false);
		pci_cfg_access_unlock(dev);
		pr_info("pcitest: slot_reset %s: done ret=%d\n", pci_name(dev), ret);
	}
	pci_dev_put(dev);
	return ret ? ret : len;
}

static ssize_t slot_reset2_write(struct file *f, const char __user *ubuf,
				 size_t len, loff_t *ppos)
{
	struct hotplug_slot *hp;
	struct pci_dev *dev;
	int ret;

	dev = pcitest_get_dev(ubuf, len, NULL, &ret);
	if (!dev)
		return ret;
	hp = pcitest_hotplug_slot(dev);
	if (!hp || !hp->ops->reset_slot) {
		pci_dev_put(dev);
		return -ENOTTY;
	}
	pr_info("pcitest2: slot_reset %s: start\n", pci_name(dev));
	ret = hp->ops->reset_slot(hp, false);
	pr_info("pcitest2: slot_reset %s: done ret=%d\n", pci_name(dev), ret);
	pci_dev_put(dev);
	return ret ? ret : len;
}

static ssize_t disconnect_write(struct file *f, const char __user *ubuf,
				size_t len, loff_t *ppos)
{
	struct pci_dev *dev;
	int ret;

	dev = pcitest_get_dev(ubuf, len, NULL, &ret);
	if (!dev)
		return ret;
	pr_info("pcitest: marking %s disconnected\n", pci_name(dev));
	pci_dev_set_disconnected(dev, NULL);
	if (dev->subordinate)
		pci_walk_bus(dev->subordinate, pci_dev_set_disconnected, NULL);
	pci_dev_put(dev);
	return len;
}

static ssize_t err_reset_write(struct file *f, const char __user *ubuf,
			       size_t len, loff_t *ppos)
{
	struct pci_dev *dev;
	unsigned int ms;
	int ret;

	dev = pcitest_get_dev(ubuf, len, &ms, &ret);
	if (!dev)
		return ret;
	msleep(ms);
	ret = pci_bus_error_reset(dev);
	pr_info("pcitest: err_reset %s: ret=%d\n", pci_name(dev), ret);
	pci_dev_put(dev);
	return ret ? ret : len;
}

/*
 * AER-faithful variants of err_reset/host_reset: run the error recovery that
 * aer_root_reset()/pci_host_reset_root_port() are called from, so drivers get
 * error_detected/slot_reset/resume and restore their devices.
 */
static pci_ers_result_t pcitest_aer_reset_link(struct pci_dev *dev)
{
	return pci_bus_error_reset(dev) ? PCI_ERS_RESULT_DISCONNECT :
					  PCI_ERS_RESULT_RECOVERED;
}

static pci_ers_result_t pcitest_aer_host_reset_link(struct pci_dev *dev)
{
	int ret;

	pci_lock_rescan_remove();
	ret = pci_bus_error_reset(dev);
	pci_unlock_rescan_remove();
	return ret ? PCI_ERS_RESULT_DISCONNECT : PCI_ERS_RESULT_RECOVERED;
}

static ssize_t pcitest_aer_common(const char __user *ubuf, size_t len,
				  pci_ers_result_t (*reset)(struct pci_dev *),
				  const char *what)
{
	pci_ers_result_t status;
	struct pci_dev *dev;
	unsigned int ms;
	int ret;

	dev = pcitest_get_dev(ubuf, len, &ms, &ret);
	if (!dev)
		return ret;
	msleep(ms);
	status = pcie_do_recovery(dev, pci_channel_io_frozen, reset);
	pr_info("pcitest: %s %s: status=%u\n", what, pci_name(dev), status);
	pci_dev_put(dev);
	return status == PCI_ERS_RESULT_RECOVERED ? len : -EIO;
}

static ssize_t aer_reset_write(struct file *f, const char __user *ubuf,
			       size_t len, loff_t *ppos)
{
	return pcitest_aer_common(ubuf, len, pcitest_aer_reset_link, "aer_reset");
}

static ssize_t aer_host_reset_write(struct file *f, const char __user *ubuf,
				    size_t len, loff_t *ppos)
{
	return pcitest_aer_common(ubuf, len, pcitest_aer_host_reset_link,
				  "aer_host_reset");
}

static ssize_t reset_bus_write(struct file *f, const char __user *ubuf,
			       size_t len, loff_t *ppos)
{
	struct pci_dev *dev;
	unsigned int ms;
	int ret;

	dev = pcitest_get_dev(ubuf, len, &ms, &ret);
	if (!dev)
		return ret;
	msleep(ms);
	ret = pci_reset_bus(dev);
	pr_info("pcitest: reset_bus %s: ret=%d\n", pci_name(dev), ret);
	pci_dev_put(dev);
	return ret ? ret : len;
}

/* Called from the call sites apply_knobs.py inserts (TEST ONLY). */
unsigned int pcitest_add_delay_ms, pcitest_unbind_delay_ms;

void pcitest_add_delay(struct pci_dev *dev);
void pcitest_add_delay(struct pci_dev *dev)
{
	unsigned int ms = READ_ONCE(pcitest_add_delay_ms);

	if (!ms || !pci_is_pcie(dev) || pci_pcie_type(dev) != PCI_EXP_TYPE_ENDPOINT)
		return;
	pr_info("pcitest: add delay %s %u ms\n", pci_name(dev), ms);
	msleep(ms);
	pr_info("pcitest: add delay %s done, device_add\n", pci_name(dev));
}

void pcitest_unbind_delay(struct pci_dev *dev, bool has_vfs);
void pcitest_unbind_delay(struct pci_dev *dev, bool has_vfs)
{
	unsigned int ms = READ_ONCE(pcitest_unbind_delay_ms);

	if (!ms || !dev->is_physfn || has_vfs)
		return;
	pci_info(dev, "pcitest: has_vfs=0, sleeping before release\n");
	msleep(ms);
}

static const struct file_operations hold_fops = { .write = hold_write };
static const struct file_operations err_reset_fops = { .write = err_reset_write };
static const struct file_operations reset_bus_fops = { .write = reset_bus_write };
static const struct file_operations host_reset_fops = { .write = host_reset_write };
static const struct file_operations host_reset_nolock_fops = { .write = host_reset_nolock_write };
static const struct file_operations aer_reset_fops = { .write = aer_reset_write };
static const struct file_operations aer_host_reset_fops = { .write = aer_host_reset_write };
static const struct file_operations slot_reset_fops = { .write = slot_reset_write };
static const struct file_operations slot_reset2_fops = { .write = slot_reset2_write };
static const struct file_operations disconnect_fops = { .write = disconnect_write };

static int __init pcitest_hooks_init(void)
{
	struct dentry *d = debugfs_create_dir("pcitest", NULL);
	struct dentry *d2 = debugfs_create_dir("pcitest2", NULL);

	debugfs_create_file("hold_rescan_ms", 0200, d, NULL, &hold_fops);
	debugfs_create_file("host_reset", 0200, d, NULL, &host_reset_fops);
	debugfs_create_file("host_reset_nolock", 0200, d, NULL, &host_reset_nolock_fops);
	debugfs_create_file("slot_reset", 0200, d, NULL, &slot_reset_fops);
	debugfs_create_file("disconnect", 0200, d, NULL, &disconnect_fops);
	debugfs_create_file("slot_reset", 0200, d2, NULL, &slot_reset2_fops);
	debugfs_create_file("err_reset", 0200, d, NULL, &err_reset_fops);
	debugfs_create_file("aer_reset", 0200, d, NULL, &aer_reset_fops);
	debugfs_create_file("aer_host_reset", 0200, d, NULL, &aer_host_reset_fops);
	debugfs_create_file("reset_bus", 0200, d, NULL, &reset_bus_fops);
#ifdef PCITEST_KNOB_ADD_DELAY
	debugfs_create_u32("add_delay_ms", 0600, d, &pcitest_add_delay_ms);
#endif
#ifdef PCITEST_KNOB_UNBIND_DELAY
	debugfs_create_u32("unbind_delay_ms", 0600, d, &pcitest_unbind_delay_ms);
#endif
	return 0;
}
late_initcall(pcitest_hooks_init);
