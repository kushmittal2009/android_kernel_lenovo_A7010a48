/*
 * xhci-plat.c - xHCI host controller driver platform Bus Glue.
 *
 * Copyright (C) 2012 Texas Instruments Incorporated - http://www.ti.com
 * Author: Sebastian Andrzej Siewior <bigeasy@linutronix.de>
 *
 * A lot of code borrowed from the Linux xHCI driver.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/usb/xhci_pdriver.h>

#include "xhci.h"

#ifdef CONFIG_USB_XHCI_MTK
#include <xhci-mtk.h>
#endif

#include "xhci-mvebu.h"
#include "xhci-rcar.h"
#ifdef CONFIG_SSUSB_MTK_XHCI
#include "xhci-ssusb-mtk.h"
#endif

static struct hc_driver __read_mostly xhci_plat_hc_driver;

static void xhci_plat_quirks(struct device *dev, struct xhci_hcd *xhci)
{
	/*
	 * As of now platform drivers don't provide MSI support so we ensure
	 * here that the generic code does not try to make a pci_dev from our
	 * dev struct in order to setup MSI
	 */
	xhci->quirks |= XHCI_PLAT;
#ifdef CONFIG_USB_XHCI_MTK
	/*
	 * MTK host controller gives a spurious successful event after a
	 * short transfer. Ignore it.
	*/
	xhci->quirks |= XHCI_SPURIOUS_SUCCESS;
	xhci->quirks |= XHCI_LPM_SUPPORT;
#endif
}

/* called during probe() after chip reset completes */
static int xhci_plat_setup(struct usb_hcd *hcd)
{
	struct device_node *of_node = hcd->self.controller->of_node;
	int ret;
#ifdef CONFIG_SSUSB_MTK_XHCI
	struct xhci_hcd *xhci;
#endif

	if (of_device_is_compatible(of_node, "renesas,xhci-r8a7790") ||
	    of_device_is_compatible(of_node, "renesas,xhci-r8a7791")) {
		ret = xhci_rcar_init_quirk(hcd);
		if (ret)
			return ret;
	}
#ifdef CONFIG_SSUSB_MTK_XHCI
	ret = xhci_gen_setup(hcd, xhci_plat_quirks);
	if (ret)
		return ret;

	if (!usb_hcd_is_primary_hcd(hcd))
		return 0;

	xhci = hcd_to_xhci(hcd);
	ret = xhci_mtk_init_quirk(xhci);
	if (ret) {
		kfree(xhci);
		return ret;
	}
	return ret;
#else
	return xhci_gen_setup(hcd, xhci_plat_quirks);
#endif


}

static int xhci_plat_start(struct usb_hcd *hcd)
{
	struct device_node *of_node = hcd->self.controller->of_node;

	if (of_device_is_compatible(of_node, "renesas,xhci-r8a7790") ||
	    of_device_is_compatible(of_node, "renesas,xhci-r8a7791"))
		xhci_rcar_start(hcd);

	return xhci_run(hcd);
}
#if defined(CONFIG_MTK_LM_MODE)
#define XHCI_DMA_BIT_MASK DMA_BIT_MASK(64)
#else
#define XHCI_DMA_BIT_MASK DMA_BIT_MASK(32)
#endif

static u64 xhci_dma_mask = XHCI_DMA_BIT_MASK;

static void xhci_hcd_release(struct device *dev)
{
	;
}

static int xhci_plat_probe(struct platform_device *pdev)
{
	struct device_node	*node = pdev->dev.of_node;
	struct usb_xhci_pdata	*pdata = dev_get_platdata(&pdev->dev);
	const struct hc_driver	*driver;
	struct xhci_hcd		*xhci;
	struct resource         *res;
	struct usb_hcd		*hcd;
	struct clk              *clk;
	int			ret;
	int			irq;

	if (usb_disabled())
		return -ENODEV;

	driver = &xhci_plat_hc_driver;

#ifdef CONFIG_USB_XHCI_MTK  /* device tree support */
	irq = platform_get_irq_byname(pdev, XHCI_DRIVER_NAME);
	if (irq < 0)
		return -ENODEV;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, XHCI_BASE_REGS_ADDR_RES_NAME);
	if (!res)
		return -ENODEV;

	pdev->dev.coherent_dma_mask = XHCI_DMA_BIT_MASK;
	pdev->dev.dma_mask = &xhci_dma_mask;
	pdev->dev.release = xhci_hcd_release;
#else
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;
#endif
	/* Initialize dma_mask and coherent_dma_mask to 32-bits */
	ret = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	if (!pdev->dev.dma_mask)
		pdev->dev.dma_mask = &pdev->dev.coherent_dma_mask;
	else
		dma_set_mask(&pdev->dev, DMA_BIT_MASK(32));

	hcd = usb_create_hcd(driver, &pdev->dev, dev_name(&pdev->dev));
	if (!hcd)
		return -ENOMEM;

	hcd->rsrc_start = res->start;
	hcd->rsrc_len = resource_size(res);

	hcd->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(hcd->regs)) {
		ret = PTR_ERR(hcd->regs);
		goto put_hcd;
	}

	/*
	 * Not all platforms have a clk so it is not an error if the
	 * clock does not exists.
	 */
	clk = devm_clk_get(&pdev->dev, NULL);
	if (!IS_ERR(clk)) {
		ret = clk_prepare_enable(clk);
		if (ret)
			goto put_hcd;
	} else if (PTR_ERR(clk) == -EPROBE_DEFER) {
		ret = -EPROBE_DEFER;
		goto put_hcd;
	}

	if (of_device_is_compatible(pdev->dev.of_node,
				    "marvell,armada-375-xhci") ||
	    of_device_is_compatible(pdev->dev.of_node,
				    "marvell,armada-380-xhci")) {
		ret = xhci_mvebu_mbus_init_quirk(pdev);
		if (ret)
			goto disable_clk;
	}

	ret = usb_add_hcd(hcd, irq, IRQF_SHARED);
	if (ret)
		goto disable_clk;

	device_wakeup_enable(hcd->self.controller);

	/* USB 2.0 roothub is stored in the platform_device now. */
	hcd = platform_get_drvdata(pdev);
	xhci = hcd_to_xhci(hcd);
	xhci->clk = clk;
	xhci->shared_hcd = usb_create_shared_hcd(driver, &pdev->dev,
			dev_name(&pdev->dev), hcd);
	if (!xhci->shared_hcd) {
		ret = -ENOMEM;
		goto dealloc_usb2_hcd;
	}

	if ((node && of_property_read_bool(node, "usb3-lpm-capable")) ||
			(pdata && pdata->usb3_lpm_capable))
		xhci->quirks |= XHCI_LPM_SUPPORT;
	/*
	 * Set the xHCI pointer before xhci_plat_setup() (aka hcd_driver.reset)
	 * is called by usb_add_hcd().
	 */
	*((struct xhci_hcd **) xhci->shared_hcd->hcd_priv) = xhci;

	if (HCC_MAX_PSA(xhci->hcc_params) >= 4)
		xhci->shared_hcd->can_do_streams = 1;

	ret = usb_add_hcd(xhci->shared_hcd, irq, IRQF_SHARED);
	if (ret)
		goto put_usb3_hcd;
#ifdef CONFIG_USB_XHCI_MTK
	mtk_xhci_vbus_on(pdev);
#endif
	return 0;

put_usb3_hcd:
	usb_put_hcd(xhci->shared_hcd);

dealloc_usb2_hcd:
	usb_remove_hcd(hcd);

disable_clk:
	if (!IS_ERR(clk))
		clk_disable_unprepare(clk);

put_hcd:
	usb_put_hcd(hcd);

	return ret;
}

static int xhci_plat_remove(struct platform_device *dev)
{
	struct usb_hcd	*hcd = platform_get_drvdata(dev);
	struct xhci_hcd	*xhci = hcd_to_xhci(hcd);
	struct clk *clk = xhci->clk;


#ifdef CONFIG_USB_XHCI_MTK
	mtk_xhci_vbus_off(dev);
#endif
	xhci->xhc_state |= XHCI_STATE_REMOVING;

	usb_remove_hcd(xhci->shared_hcd);
	usb_put_hcd(xhci->shared_hcd);

	usb_remove_hcd(hcd);
	if (!IS_ERR(clk))
		clk_disable_unprepare(clk);
	usb_put_hcd(hcd);
#ifdef CONFIG_SSUSB_MTK_XHCI
	if (xhci->quirks & XHCI_MTK_HOST)
		xhci_mtk_exit_quirk(xhci);
#endif
	kfree(xhci);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int xhci_plat_suspend(struct device *dev)
{
	struct usb_hcd	*hcd = dev_get_drvdata(dev);
	struct xhci_hcd	*xhci = hcd_to_xhci(hcd);

	/*
	 * xhci_suspend() needs `do_wakeup` to know whether host is allowed
	 * to do wakeup during suspend. Since xhci_plat_suspend is currently
	 * only designed for system suspend, device_may_wakeup() is enough
	 * to dertermine whether host is allowed to do wakeup. Need to
	 * reconsider this when xhci_plat_suspend enlarges its scope, e.g.,
	 * also applies to runtime suspend.
	 */
	return xhci_suspend(xhci, device_may_wakeup(dev));
}

static int xhci_plat_resume(struct device *dev)
{
	struct usb_hcd	*hcd = dev_get_drvdata(dev);
	struct xhci_hcd	*xhci = hcd_to_xhci(hcd);

	return xhci_resume(xhci, 0);
}

static const struct dev_pm_ops xhci_plat_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(xhci_plat_suspend, xhci_plat_resume)
};
#define DEV_PM_OPS	(&xhci_plat_pm_ops)
#else
#define DEV_PM_OPS	NULL
#endif /* CONFIG_PM */

#ifdef CONFIG_OF
static const struct of_device_id usb_xhci_of_match[] = {
	{ .compatible = "generic-xhci" },
	{ .compatible = "xhci-platform" },
	{ .compatible = "marvell,armada-375-xhci"},
	{ .compatible = "marvell,armada-380-xhci"},
	{ .compatible = "renesas,xhci-r8a7790"},
	{ .compatible = "renesas,xhci-r8a7791"},
	{ .compatible = "mediatek,usb3_xhci"},
	{ },
};
MODULE_DEVICE_TABLE(of, usb_xhci_of_match);
#endif

static struct platform_driver usb_xhci_driver = {
	.probe	= xhci_plat_probe,
	.remove	= xhci_plat_remove,
	.driver	= {
		.name = "xhci-hcd",
		.pm = DEV_PM_OPS,
		.of_match_table = of_match_ptr(usb_xhci_of_match),
	},
};
MODULE_ALIAS("platform:xhci-hcd");

#ifdef CONFIG_USB_XHCI_MTK
int xhci_register_plat(void)
{
	xhci_init_driver(&xhci_plat_hc_driver, xhci_plat_setup);
	xhci_plat_hc_driver.start = xhci_plat_start;
	return platform_driver_register(&usb_xhci_driver);
}

void xhci_unregister_plat(void)
{
	platform_driver_unregister(&usb_xhci_driver);
}
#else
static int __init xhci_plat_init(void)
{
	xhci_init_driver(&xhci_plat_hc_driver, xhci_plat_setup);
	xhci_plat_hc_driver.start = xhci_plat_start;
	return platform_driver_register(&usb_xhci_driver);
}
module_init(xhci_plat_init);

static void __exit xhci_plat_exit(void)
{
	platform_driver_unregister(&usb_xhci_driver);
}
module_exit(xhci_plat_exit);
#endif

MODULE_DESCRIPTION("xHCI Platform Host Controller Driver");
MODULE_LICENSE("GPL");
