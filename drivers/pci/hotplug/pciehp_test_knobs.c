// SPDX-License-Identifier: GPL-2.0
/*
 * TEST ONLY, not for upstream.  pciehp knobs from the slotmutex design's
 * test-patches/000{1,2}-TEST-ONLY-*.patch, moved out of pciehp_pci.c so
 * that only two one-line calls depend on the candidate's pciehp_pci.c
 * (inserted by the TEST ONLY commit).  Linked into pciehp, so the
 * parameters appear as /sys/module/pciehp/parameters/test_*:
 *
 * test_pre_scan_ms:  pciehp_configure_device() sleeps this long right before
 *	pci_scan_slot(), with whatever locks the candidate holds there
 *	(reset_lock and pci_rescan_remove_lock in v7.4-rc1).
 * test_post_scan_ms: likewise right after pcie_bus_configure_settings(),
 *	i.e. after the scan and resource assignment, before driver binding.
 * test_error_reset:  writing "BDF" calls pci_bus_error_reset() on the port
 *	like AER recovery, without holding any lock (same as writing "BDF 0"
 *	to /sys/kernel/debug/pcitest/host_reset_nolock).
 *
 * Both delays log "test: sleeping N ms before|after scan", which scenarios
 * wait for.
 */
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/pci.h>
#include "../pci.h"
#include "pciehp.h"

static unsigned int test_pre_scan_ms, test_post_scan_ms;
module_param(test_pre_scan_ms, uint, 0644);
module_param(test_post_scan_ms, uint, 0644);

void pciehp_test_delay(struct controller *ctrl, bool post);
void pciehp_test_delay(struct controller *ctrl, bool post)
{
	unsigned int ms = post ? READ_ONCE(test_post_scan_ms) :
				 READ_ONCE(test_pre_scan_ms);

	if (!ms)
		return;
	ctrl_info(ctrl, "test: sleeping %u ms %s scan\n", ms,
		  post ? "after" : "before");
	msleep(ms);
}

static int test_error_reset_set(const char *val, const struct kernel_param *kp)
{
	unsigned int dom, bus, slot, fn;
	struct pci_dev *dev;
	int ret;

	if (sscanf(val, "%x:%x:%x.%x", &dom, &bus, &slot, &fn) != 4)
		return -EINVAL;
	dev = pci_get_domain_bus_and_slot(dom, bus, PCI_DEVFN(slot, fn));
	if (!dev)
		return -ENODEV;
	pr_info("pciehp test: pci_bus_error_reset(%s)\n", pci_name(dev));
	ret = pci_bus_error_reset(dev);
	pr_info("pciehp test: pci_bus_error_reset(%s) = %d\n", pci_name(dev), ret);
	pci_dev_put(dev);
	return 0;
}

static const struct kernel_param_ops test_error_reset_ops = {
	.set = test_error_reset_set,
};
module_param_cb(test_error_reset, &test_error_reset_ops, NULL, 0200);
