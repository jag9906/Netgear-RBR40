/* Copyright (c) 2014, 2016 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/watchdog.h>
#include <soc/qcom/scm.h>
#include <linux/interrupt.h>
#include <linux/sched.h>

#define WDT_RST		0x0
#define WDT_EN		0x8
#define WDT_BARK_TIME	0x14
#define WDT_BITE_TIME	0x24

#define USE_BITE_TIME	1UL

#define SCM_CMD_SET_REGSAVE  0x2
static int in_panic;

struct qcom_wdt {
	struct watchdog_device	wdd;
	struct clk		*clk;
	unsigned long		rate;
	unsigned int		bite;
	struct notifier_block	restart_nb;
	void __iomem		*base;
	void __iomem		*wdt_reset;
	void __iomem		*wdt_enable;
	void __iomem		*wdt_bark_time;
	void __iomem		*wdt_bite_time;
};

static inline
struct qcom_wdt *to_qcom_wdt(struct watchdog_device *wdd)
{
	return container_of(wdd, struct qcom_wdt, wdd);
}

static int panic_prep_restart(struct notifier_block *this,
			      unsigned long event, void *ptr)
{
	in_panic = 1;
	return NOTIFY_DONE;
}

static struct notifier_block panic_blk = {
	.notifier_call  = panic_prep_restart,
};
static long qcom_wdt_configure_bark_dump(void *arg)
{
	long ret = -ENOMEM;
	struct {
		unsigned addr;
		int len;
	} cmd_buf;

	/* Area for context dump in secure mode */
	void *scm_regsave = (void *)__get_free_page(GFP_KERNEL);

	if (scm_regsave) {
		cmd_buf.addr = virt_to_phys(scm_regsave);
		cmd_buf.len  = PAGE_SIZE;

		ret = scm_call(SCM_SVC_UTIL, SCM_CMD_SET_REGSAVE,
			       &cmd_buf, sizeof(cmd_buf), NULL, 0);
	}

	if (ret)
		pr_err("Setting register save address failed.\n"
				"Registers won't be dumped on a dog bite\n");
	return ret;
}

static int qcom_wdt_start_secure(struct watchdog_device *wdd)
{
	struct qcom_wdt *wdt = to_qcom_wdt(wdd);

	writel(0, wdt->wdt_enable);
	writel(1, wdt->wdt_reset);

	if (wdt->bite) {
		writel((wdd->timeout - 1) * wdt->rate, wdt->wdt_bark_time);
		writel(wdd->timeout * wdt->rate, wdt->wdt_bite_time);
	} else {
		writel(wdd->timeout * wdt->rate, wdt->wdt_bark_time);
		writel(0x0FFFFFFF, wdt->wdt_bite_time);
	}

	writel(1, wdt->wdt_enable);
	return 0;
}

static int qcom_wdt_start_nonsecure(struct watchdog_device *wdd)
{
	struct qcom_wdt *wdt = to_qcom_wdt(wdd);

	writel(0, wdt->wdt_enable);
	writel(1, wdt->wdt_reset);
	writel(wdd->timeout * wdt->rate, wdt->wdt_bite_time);
	writel(1, wdt->wdt_enable);
	return 0;
}

static int qcom_wdt_stop(struct watchdog_device *wdd)
{
	struct qcom_wdt *wdt = to_qcom_wdt(wdd);

	writel(0, wdt->wdt_enable);
	return 0;
}

static int qcom_wdt_ping(struct watchdog_device *wdd)
{
	struct qcom_wdt *wdt = to_qcom_wdt(wdd);

	writel(1, wdt->wdt_reset);
	return 0;
}

static int qcom_wdt_set_timeout(struct watchdog_device *wdd,
				unsigned int timeout)
{
	wdd->timeout = timeout;
	return wdd->ops->start(wdd);
}

static const struct watchdog_ops qcom_wdt_ops_secure = {
	.start		= qcom_wdt_start_secure,
	.stop		= qcom_wdt_stop,
	.ping		= qcom_wdt_ping,
	.set_timeout	= qcom_wdt_set_timeout,
	.owner		= THIS_MODULE,
};

static const struct watchdog_ops qcom_wdt_ops_nonsecure = {
	.start		= qcom_wdt_start_nonsecure,
	.stop		= qcom_wdt_stop,
	.ping		= qcom_wdt_ping,
	.set_timeout	= qcom_wdt_set_timeout,
	.owner		= THIS_MODULE,
};

static const struct watchdog_info qcom_wdt_info = {
	.options	= WDIOF_KEEPALIVEPING
			| WDIOF_MAGICCLOSE
			| WDIOF_SETTIMEOUT,
	.identity	= KBUILD_MODNAME,
};

static const struct of_device_id qcom_wdt_of_table[] = {
	{ .compatible = "qcom,kpss-wdt-msm8960", },
	{ .compatible = "qcom,kpss-wdt-apq8064", },
	{ .compatible = "qcom,kpss-wdt-ipq8064", },
	{ .compatible = "qcom,kpss-wdt-ipq40xx", .data = (void *)USE_BITE_TIME},
	{ },
};
MODULE_DEVICE_TABLE(of, qcom_wdt_of_table);

static int qcom_wdt_restart(struct notifier_block *nb, unsigned long action,
			    void *data)
{
	struct qcom_wdt *wdt = container_of(nb, struct qcom_wdt, restart_nb);
	u32 timeout;
	printk("***********Watchdog Reboot*************\n");	

	/*
	 * Trigger watchdog bite/bark:
	 *
	 * For regular reboot case: Trigger watchdog bite:
	 * Setup BITE_TIME to be lower than BARK_TIME, and enable WDT.
	 *
	 * For panic reboot case: Trigger WDT bark
	 * So that TZ can save CPU registers:
	 * Setup BARK_TIME to be lower than BITE_TIME, and enable WDT.
	 */
	timeout = 128 * wdt->rate / 1000;

	writel(0, wdt->wdt_enable);
	writel(1, wdt->wdt_reset);
	if (in_panic) {
		writel(timeout, wdt->wdt_bark_time);
		writel(2 * timeout, wdt->wdt_bite_time);
	} else {
		writel(5 * timeout, wdt->wdt_bark_time);
		writel(timeout, wdt->wdt_bite_time);
	}
	writel(1, wdt->wdt_enable);

	/*
	 * Actually make sure the above sequence hits hardware before sleeping.
	 */
	wmb();

	mdelay(150);
	return NOTIFY_DONE;
}

static irqreturn_t wdt_bark_isr(int irq, void *wdd)
{
	struct qcom_wdt *wdt = to_qcom_wdt(wdd);
	unsigned long nanosec_rem;
	unsigned long long t = sched_clock();

	nanosec_rem = do_div(t, 1000000000);
	pr_info("Watchdog bark! Now = %lu.%06lu\n", (unsigned long) t,
							nanosec_rem / 1000);
	pr_info("Causing a watchdog bite!");
	writel(0, wdt->wdt_enable);
	writel(1, wdt->wdt_bite_time);
	mb(); /* Avoid unpredictable behaviour in concurrent executions */
	pr_info("Configuring Watchdog Timer\n");
	writel(1, wdt->wdt_reset);
	writel(1, wdt->wdt_enable);
	mb(); /* Make sure the above sequence hits hardware before Reboot. */
	pr_info("Waiting for Reboot\n");

	mdelay(1);
	pr_err("Wdog - CTL: 0x%x, BARK TIME: 0x%x, BITE TIME: 0x%x",
		readl(wdt->wdt_enable),
		readl(wdt->wdt_bark_time),
		readl(wdt->wdt_bite_time));
	return IRQ_HANDLED;
}

void register_wdt_bark_irq(int irq, struct qcom_wdt *wdt)
{
	int ret;
	ret = request_irq(irq, wdt_bark_isr, IRQF_TRIGGER_HIGH,
						"watchdog bark", wdt);
	if (ret)
		pr_err("error request_irq(irq_num:%d ) ret:%d\n",
								irq, ret);
}

static int qcom_wdt_probe(struct platform_device *pdev)
{
	const struct of_device_id *id;
	struct device_node *np;
	struct qcom_wdt *wdt;
	struct resource *res;
	int ret, irq = 0;
	uint32_t val;

	wdt = devm_kzalloc(&pdev->dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	irq = platform_get_irq_byname(pdev, "bark_irq");
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	wdt->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(wdt->base))
		return PTR_ERR(wdt->base);

	np = of_node_get(pdev->dev.of_node);

	if (of_property_read_u32(np, "wdt_res", &val))
		wdt->wdt_reset = wdt->base + WDT_RST;
	else
		wdt->wdt_reset = wdt->base + val;

	if (of_property_read_u32(np, "wdt_en", &val))
		wdt->wdt_enable = wdt->base + WDT_EN;
	else
		wdt->wdt_enable = wdt->base + val;

	if (of_property_read_u32(np, "wdt_bark_time", &val))
		wdt->wdt_bark_time = wdt->base + WDT_BARK_TIME;
	else
		wdt->wdt_bark_time = wdt->base + val;

	if (of_property_read_u32(np, "wdt_bite_time", &val))
		wdt->wdt_bite_time = wdt->base + WDT_BITE_TIME;
	else
		wdt->wdt_bite_time = wdt->base + val;

	id = of_match_device(qcom_wdt_of_table, &pdev->dev);
	if (!id)
		return -ENODEV;

	if (id->data)
		wdt->bite = 1;

	if (irq > 0)
		register_wdt_bark_irq(irq, wdt);

	wdt->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(wdt->clk)) {
		dev_err(&pdev->dev, "failed to get input clock\n");
		return PTR_ERR(wdt->clk);
	}

	ret = clk_prepare_enable(wdt->clk);
	if (ret) {
		dev_err(&pdev->dev, "failed to setup clock\n");
		return ret;
	}

	/*
	 * We use the clock rate to calculate the max timeout, so ensure it's
	 * not zero to avoid a divide-by-zero exception.
	 *
	 * WATCHDOG_CORE assumes units of seconds, if the WDT is clocked such
	 * that it would bite before a second elapses it's usefulness is
	 * limited.  Bail if this is the case.
	 */
	wdt->rate = clk_get_rate(wdt->clk);
	if (wdt->rate == 0 ||
	    wdt->rate > 0x10000000U) {
		dev_err(&pdev->dev, "invalid clock rate\n");
		ret = -EINVAL;
		goto err_clk_unprepare;
	}

	ret = work_on_cpu(0, qcom_wdt_configure_bark_dump, NULL);

	wdt->wdd.dev = &pdev->dev;
	wdt->wdd.info = &qcom_wdt_info;
	if (ret)
		wdt->wdd.ops = &qcom_wdt_ops_nonsecure;
	else
		wdt->wdd.ops = &qcom_wdt_ops_secure;
	wdt->wdd.min_timeout = 1;
	wdt->wdd.max_timeout = 0x10000000U / wdt->rate;

	/*
	 * If 'timeout-sec' unspecified in devicetree, assume a 30 second
	 * default, unless the max timeout is less than 30 seconds, then use
	 * the max instead.
	 */
	wdt->wdd.timeout = min(wdt->wdd.max_timeout, 30U);
	watchdog_init_timeout(&wdt->wdd, 0, &pdev->dev);

	ret = watchdog_register_device(&wdt->wdd);
	if (ret) {
		dev_err(&pdev->dev, "failed to register watchdog\n");
		goto err_clk_unprepare;
	}

	/*
	 * WDT restart notifier has priority 0 (use as a last resort)
	 */
	wdt->restart_nb.notifier_call = qcom_wdt_restart;
	atomic_notifier_chain_register(&panic_notifier_list, &panic_blk);
	ret = register_restart_handler(&wdt->restart_nb);
	if (ret)
		dev_err(&pdev->dev, "failed to setup restart handler\n");

	platform_set_drvdata(pdev, wdt);
	return 0;

err_clk_unprepare:
	clk_disable_unprepare(wdt->clk);
	return ret;
}

static int qcom_wdt_remove(struct platform_device *pdev)
{
	struct qcom_wdt *wdt = platform_get_drvdata(pdev);

	unregister_restart_handler(&wdt->restart_nb);
	watchdog_unregister_device(&wdt->wdd);
	clk_disable_unprepare(wdt->clk);
	return 0;
}

static struct platform_driver qcom_watchdog_driver = {
	.probe	= qcom_wdt_probe,
	.remove	= qcom_wdt_remove,
	.driver	= {
		.name		= KBUILD_MODNAME,
		.of_match_table	= qcom_wdt_of_table,
	},
};
module_platform_driver(qcom_watchdog_driver);

MODULE_DESCRIPTION("QCOM KPSS Watchdog Driver");
MODULE_LICENSE("GPL v2");
