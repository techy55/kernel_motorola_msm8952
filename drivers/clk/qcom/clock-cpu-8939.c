/*
 * Copyright (c) 2014-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/mutex.h>
#include <linux/msm-bus-board.h>
#include <linux/msm-bus.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/pm_qos.h>
#include <linux/regulator/consumer.h>
#include <linux/of.h>
#include <linux/clk/msm-clock-generic.h>
#include <linux/suspend.h>
#include <soc/qcom/clock-local2.h>

#include <dt-bindings/clock/msm-cpu-clocks-8939.h>

DEFINE_VDD_REGS_INIT(vdd_cpu_bc, 1);
DEFINE_VDD_REGS_INIT(vdd_cpu_lc, 1);
DEFINE_VDD_REGS_INIT(vdd_cpu_cci, 1);
struct platform_device *cpu_clock_8939_dev;

enum {
	A53SS_MUX_BC,
	A53SS_MUX_LC,
	A53SS_MUX_CCI,
	A53SS_MUX_NUM,
};

const char *mux_names[] = { "c1", "c0", "cci"};

struct cpu_clk_8939 {
	u32 cpu_reg_mask;
	cpumask_t cpumask;
	bool hw_low_power_ctrl;
	struct pm_qos_request req;
	struct clk c;
};

/* Perf or big Cluster */
static struct mux_div_clk a53ssmux_bc = {
	.ops = &rcg_mux_div_ops,
	.safe_freq = 400000000,
	.data = {
		.max_div = 32,
		.min_div = 2,
		.is_half_divider = true,
	},
	.c = {
		.dbg_name = "a53ssmux_bc",
		.flags = CLKFLAG_NO_RATE_CACHE,
		.ops = &clk_ops_mux_div_clk,
		CLK_INIT(a53ssmux_bc.c),
	},
	.parents = (struct clk_src[8]) {},
	.div_mask = BM(4, 0),
	.src_mask = BM(10, 8) >> 8,
	.src_shift = 8,
};

/* Power or LITTLE Cluster */
static struct mux_div_clk a53ssmux_lc = {
	.ops = &rcg_mux_div_ops,
	.safe_freq = 200000000,
	.data = {
		.max_div = 32,
		.min_div = 2,
		.is_half_divider = true,
	},
	.c = {
		.dbg_name = "a53ssmux_lc",
		.flags = CLKFLAG_NO_RATE_CACHE,
		.ops = &clk_ops_mux_div_clk,
		CLK_INIT(a53ssmux_lc.c),
	},
	.parents = (struct clk_src[8]) {},
	.div_mask = BM(4, 0),
	.src_mask = BM(10, 8) >> 8,
	.src_shift = 8,
};

static struct mux_div_clk a53ssmux_cci = {
	.ops = &rcg_mux_div_ops,
	.safe_freq = 200000000,
	.data = {
		.max_div = 32,
		.min_div = 2,
		.is_half_divider = true,
	},
	.c = {
		.dbg_name = "a53ssmux_cci",
		.ops = &clk_ops_mux_div_clk,
		CLK_INIT(a53ssmux_cci.c),
	},
	.parents = (struct clk_src[8]) {},
	.div_mask = BM(4, 0),
	.src_mask = BM(10, 8) >> 8,
	.src_shift = 8,
};

static void do_nothing(void *unused) { }
#define CPU_LATENCY_NO_L2_PC_US (250)

static inline struct cpu_clk_8939 *to_cpu_clk_8939(struct clk *c)
{
	return container_of(c, struct cpu_clk_8939, c);
}

static enum handoff cpu_clk_8939_handoff(struct clk *c)
{
	c->rate = clk_get_rate(c->parent);
	return HANDOFF_DISABLED_CLK;
}

static long cpu_clk_8939_round_rate(struct clk *c, unsigned long rate)
{
	return clk_round_rate(c->parent, rate);
}

static int cpu_clk_8939_set_rate(struct clk *c, unsigned long rate)
{
	int ret = 0;
	struct cpu_clk_8939 *cpuclk = to_cpu_clk_8939(c);
	bool hw_low_power_ctrl = cpuclk->hw_low_power_ctrl;

	if (hw_low_power_ctrl) {
		memset(&cpuclk->req, 0, sizeof(cpuclk->req));
		cpumask_copy(&cpuclk->req.cpus_affine,
				(const struct cpumask *)&cpuclk->cpumask);
		cpuclk->req.type = PM_QOS_REQ_AFFINE_CORES;
		pm_qos_add_request(&cpuclk->req, PM_QOS_CPU_DMA_LATENCY,
				CPU_LATENCY_NO_L2_PC_US);
		smp_call_function_any(&cpuclk->cpumask, do_nothing,
				NULL, 1);
	}

	ret = clk_set_rate(c->parent, rate);

	if (hw_low_power_ctrl)
		pm_qos_remove_request(&cpuclk->req);

	return ret;
}

static struct clk_ops clk_ops_cpu = {
	.set_rate = cpu_clk_8939_set_rate,
	.round_rate = cpu_clk_8939_round_rate,
	.handoff = cpu_clk_8939_handoff,
};

static struct cpu_clk_8939 a53_bc_clk = {
	.cpu_reg_mask = 0x3,
	.c = {
		.parent = &a53ssmux_bc.c,
		.ops = &clk_ops_cpu,
		.vdd_class = &vdd_cpu_bc,
		.dbg_name = "a53_bc_clk",
		CLK_INIT(a53_bc_clk.c),
	},
};

static struct cpu_clk_8939 a53_lc_clk = {
	.cpu_reg_mask = 0x103,
	.c = {
		.parent = &a53ssmux_lc.c,
		.ops = &clk_ops_cpu,
		.vdd_class = &vdd_cpu_lc,
		.dbg_name = "a53_lc_clk",
		CLK_INIT(a53_lc_clk.c),
	},
};

static struct cpu_clk_8939 cci_clk = {
	.c = {
		.parent = &a53ssmux_cci.c,
		.ops = &clk_ops_cpu,
		.vdd_class = &vdd_cpu_cci,
		.dbg_name = "cci_clk",
		CLK_INIT(cci_clk.c),
	},
};

static struct clk_lookup cpu_clocks_8939[] = {
	CLK_LIST(a53ssmux_lc),
	CLK_LIST(a53ssmux_bc),
	CLK_LIST(a53ssmux_cci),
	CLK_LIST(a53_bc_clk),
	CLK_LIST(a53_lc_clk),
	CLK_LIST(cci_clk),
};

static struct mux_div_clk *a53ssmux[] = {&a53ssmux_bc,
						&a53ssmux_lc, &a53ssmux_cci};
static struct cpu_clk_8939 *cpuclk[] = { &a53_bc_clk, &a53_lc_clk, &cci_clk};

static struct clk *logical_cpu_to_clk(int cpu)
{
	struct device_node *cpu_node = of_get_cpu_node(cpu, NULL);
	u32 reg;

	/* CPU 0/1/2/3 --> a53_bc_clk and mask = 0x103
	 * CPU 4/5/6/7 --> a53_lc_clk and mask = 0x3
	 */
	if (cpu_node && !of_property_read_u32(cpu_node, "reg", &reg)) {
		if ((reg | a53_bc_clk.cpu_reg_mask) == a53_bc_clk.cpu_reg_mask)
			return &a53_lc_clk.c;
		if ((reg | a53_lc_clk.cpu_reg_mask) == a53_lc_clk.cpu_reg_mask)
			return &a53_bc_clk.c;
	}

	return NULL;
}

static int of_get_fmax_vdd_class(struct platform_device *pdev, struct clk *c,
								char *prop_name)
{
	struct device_node *of = pdev->dev.of_node;
	int prop_len, i;
	struct clk_vdd_class *vdd = c->vdd_class;
	u32 *array;

	if (!of_find_property(of, prop_name, &prop_len)) {
		dev_err(&pdev->dev, "missing %s\n", prop_name);
		return -EINVAL;
	}

	prop_len /= sizeof(u32);
	if (prop_len % 2) {
		dev_err(&pdev->dev, "bad length %d\n", prop_len);
		return -EINVAL;
	}

	prop_len /= 2;
	vdd->level_votes = devm_kzalloc(&pdev->dev,
				prop_len * sizeof(*vdd->level_votes),
					GFP_KERNEL);
	if (!vdd->level_votes)
		return -ENOMEM;

	vdd->vdd_uv = devm_kzalloc(&pdev->dev, prop_len * sizeof(int),
					GFP_KERNEL);
	if (!vdd->vdd_uv)
		return -ENOMEM;

	c->fmax = devm_kzalloc(&pdev->dev, prop_len * sizeof(unsigned long),
					GFP_KERNEL);
	if (!c->fmax)
		return -ENOMEM;

	array = devm_kzalloc(&pdev->dev,
			prop_len * sizeof(u32) * 2, GFP_KERNEL);
	if (!array)
		return -ENOMEM;

	of_property_read_u32_array(of, prop_name, array, prop_len * 2);
	for (i = 0; i < prop_len; i++) {
		c->fmax[i] = array[2 * i];
		vdd->vdd_uv[i] = array[2 * i + 1];
	}

	devm_kfree(&pdev->dev, array);
	vdd->num_levels = prop_len;
	vdd->cur_level = prop_len;
	vdd->use_max_uV = true;
	c->num_fmax = prop_len;
	return 0;
}

static struct msm_bus_paths bw_tbl[] = {
	{
		.vectors = (struct msm_bus_vectors[]){
			{
				.src = MSM_BUS_PNOC_INT_2,
				.dst = MSM_BUS_SLAVE_MESSAGE_RAM,
				.ib  = 0,
				.ab  = 0,
			},
		},
		.num_paths = 1,
	},
	{
		.vectors = (struct msm_bus_vectors[]){
			{
				.src = MSM_BUS_PNOC_INT_2,
				.dst = MSM_BUS_SLAVE_MESSAGE_RAM,
				.ib  =  50 * 4 * 1000000UL,
				.ab  = 0,
			},
		},
		.num_paths = 1,
	},
};

static struct msm_bus_scale_pdata bus_pdata = {
	.usecase = bw_tbl,
	.active_only = 1,
	.num_usecases = ARRAY_SIZE(bw_tbl),
	.name = "cpu_pcnoc",
};

static uint32_t cpu_client;
static int cpu_pcnoc_vote(void)
{
	int ret = 0;

	cpu_client = msm_bus_scale_register_client(&bus_pdata);
	if (!cpu_client) {
		pr_warn("Unable to register bus client\n");
		return -EINVAL;
	}

	ret = msm_bus_scale_client_update_request(cpu_client, 1);
	if (ret) {
		pr_err("Bandwidth request failed (%d)\n", ret);
		msm_bus_scale_client_update_request(cpu_client, 0);
	}
	return ret;
}

static void get_speed_bin(struct platform_device *pdev, int *bin,
								int *version)
{
	struct resource *res;
	void __iomem *base, *base1, *base2;
	u32 pte_efuse, pte_efuse1, pte_efuse2;

	*bin = 0;
	*version = 0;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "efuse");
	if (!res) {
		dev_info(&pdev->dev,
			 "No speed/PVS binning available. Defaulting to 0!\n");
		return;
	}

	base = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!base) {
		dev_warn(&pdev->dev,
			 "Unable to read efuse data. Defaulting to 0!\n");
		return;
	}

	pte_efuse = readl_relaxed(base);
	devm_iounmap(&pdev->dev, base);

	*bin = (pte_efuse >> 2) & 0x7;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "efuse1");
	if (!res) {
		dev_info(&pdev->dev,
			 "No PVS version available. Defaulting to 0!\n");
		goto out;
	}

	base1 = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!base1) {
		dev_warn(&pdev->dev,
			 "Unable to read efuse1 data. Defaulting to 0!\n");
		goto out;
	}

	pte_efuse1 = readl_relaxed(base1);
	devm_iounmap(&pdev->dev, base1);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "efuse2");
	if (!res) {
		dev_info(&pdev->dev,
			 "No PVS version available. Defaulting to 0!\n");
		goto out;
	}

	base2 = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!base2) {
		dev_warn(&pdev->dev,
			 "Unable to read efuse2 data. Defaulting to 0!\n");
		goto out;
	}

	pte_efuse2 = readl_relaxed(base2);
	devm_iounmap(&pdev->dev, base2);

	*version = ((pte_efuse1 >> 29 & 0x1) | ((pte_efuse2 >> 18 & 0x3) << 1));

out:
	dev_info(&pdev->dev, "Speed bin: %d PVS Version: %d\n", *bin,
								*version);
}

static int of_get_clk_src(struct platform_device *pdev,
				struct clk_src *parents, int mux_id)
{
	struct device_node *of = pdev->dev.of_node;
	int mux_parents, i, j, index;
	struct clk *c;
	char clk_name[] = "clk-xxx-x";

	mux_parents = of_property_count_strings(of, "clock-names");
	if (mux_parents <= 0) {
		dev_err(&pdev->dev, "missing clock-names\n");
		return -EINVAL;
	}
	j = 0;

	for (i = 0; i < 8; i++) {
		snprintf(clk_name, ARRAY_SIZE(clk_name), "clk-%s-%d",
							mux_names[mux_id], i);
		index = of_property_match_string(of, "clock-names", clk_name);
		if (IS_ERR_VALUE(index))
			continue;

		parents[j].sel = i;
		parents[j].src = c = devm_clk_get(&pdev->dev, clk_name);
		if (IS_ERR(c)) {
			if (c != ERR_PTR(-EPROBE_DEFER))
				dev_err(&pdev->dev, "clk_get: %s\n fail",
						clk_name);
			return PTR_ERR(c);
		}
		j++;
	}

	return j;
}

static int cpu_parse_devicetree(struct platform_device *pdev, int mux_id)
{
	struct resource *res;
	int rc;
	char rcg_name[] = "apcs-xxx-rcg-base";
	char vdd_name[] = "vdd-xxx";
	struct regulator *regulator;

	snprintf(rcg_name, ARRAY_SIZE(rcg_name), "apcs-%s-rcg-base",
						mux_names[mux_id]);
	res = platform_get_resource_byname(pdev,
					IORESOURCE_MEM, rcg_name);
	if (!res) {
		dev_err(&pdev->dev, "missing %s\n", rcg_name);
		return -EINVAL;
	}
	a53ssmux[mux_id]->base = devm_ioremap(&pdev->dev, res->start,
							resource_size(res));
	if (!a53ssmux[mux_id]->base) {
		dev_err(&pdev->dev, "ioremap failed for %s\n", rcg_name);
		return -ENOMEM;
	}

	snprintf(vdd_name, ARRAY_SIZE(vdd_name), "vdd-%s", mux_names[mux_id]);
	regulator = devm_regulator_get(&pdev->dev, vdd_name);
	if (IS_ERR(regulator)) {
		if (PTR_ERR(regulator) != -EPROBE_DEFER)
			dev_err(&pdev->dev, "unable to get regulator\n");
		return PTR_ERR(regulator);
	}
	cpuclk[mux_id]->c.vdd_class->regulator[0] = regulator;

	rc = of_get_clk_src(pdev, a53ssmux[mux_id]->parents, mux_id);
	if (IS_ERR_VALUE(rc))
		return rc;

	a53ssmux[mux_id]->num_parents = rc;

	return 0;
}

static void config_pll(int mux_id)
{
	unsigned long rate, aux_rate;
	struct clk *aux_clk, *main_pll;

	aux_clk = a53ssmux[mux_id]->parents[0].src;
	main_pll = a53ssmux[mux_id]->parents[1].src;

	aux_rate = clk_get_rate(aux_clk);
	rate = clk_get_rate(&a53ssmux[mux_id]->c);
	clk_set_rate(&a53ssmux[mux_id]->c, aux_rate);
	clk_set_rate(main_pll, clk_round_rate(main_pll, 1));
	clk_set_rate(&a53ssmux[mux_id]->c, rate);

	return;
}

static int clock_8939_pm_event(struct notifier_block *this,
				unsigned long event, void *ptr)
{
	switch (event) {
	case PM_POST_HIBERNATION:
	case PM_POST_SUSPEND:
		clk_unprepare(&a53_lc_clk.c);
		clk_unprepare(&a53_bc_clk.c);
		clk_unprepare(&cci_clk.c);
		break;
	case PM_HIBERNATION_PREPARE:
	case PM_SUSPEND_PREPARE:
		clk_prepare(&a53_lc_clk.c);
		clk_prepare(&a53_bc_clk.c);
		clk_prepare(&cci_clk.c);
		break;
	default:
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block clock_8939_pm_notifier = {
	.notifier_call = clock_8939_pm_event,
};

static int clock_a53_probe(struct platform_device *pdev)
{
	int speed_bin, version, rc, cpu, mux_id, rate;
	char prop_name[] = "qcom,speedX-bin-vX-XXX";

	get_speed_bin(pdev, &speed_bin, &version);

	for (mux_id = 0; mux_id < A53SS_MUX_NUM; mux_id++) {
		rc = cpu_parse_devicetree(pdev, mux_id);
		if (rc)
			return rc;

		snprintf(prop_name, ARRAY_SIZE(prop_name),
					"qcom,speed%d-bin-v%d-%s",
					speed_bin, version, mux_names[mux_id]);

		rc = of_get_fmax_vdd_class(pdev, &cpuclk[mux_id]->c,
								prop_name);
		if (rc) {
			/* Fall back to most conservative PVS table */
			dev_err(&pdev->dev, "Unable to load voltage plan %s!\n",
								prop_name);

			snprintf(prop_name, ARRAY_SIZE(prop_name),
				"qcom,speed0-bin-v0-%s", mux_names[mux_id]);
			rc = of_get_fmax_vdd_class(pdev, &cpuclk[mux_id]->c,
								prop_name);
			if (rc) {
				dev_err(&pdev->dev,
					"Unable to load safe voltage plan\n");
				return rc;
			}
			dev_info(&pdev->dev, "Safe voltage plan loaded.\n");
		}
	}

	rc = of_msm_clock_register(pdev->dev.of_node,
				cpu_clocks_8939, ARRAY_SIZE(cpu_clocks_8939));
	if (rc) {
		dev_err(&pdev->dev, "msm_clock_register failed\n");
		return rc;
	}

	rate = clk_get_rate(&cci_clk.c);
	clk_set_rate(&cci_clk.c, rate);

	for (mux_id = 0; mux_id < A53SS_MUX_CCI; mux_id++) {
		/* Force a PLL reconfiguration */
		config_pll(mux_id);
	}
	/*
	 * We don't want the CPU clocks to be turned off at late init
	 * if CPUFREQ or HOTPLUG configs are disabled. So, bump up the
	 * refcount of these clocks. Any cpufreq/hotplug manager can assume
	 * that the clocks have already been prepared and enabled by the time
	 * they take over.
	 */
	get_online_cpus();
	for_each_online_cpu(cpu) {
		WARN(clk_prepare_enable(&cpuclk[cpu/4]->c),
				"Unable to turn on CPU clock");
		clk_prepare_enable(&cci_clk.c);
	}
	put_online_cpus();

	cpu_clock_8939_dev = pdev;

	for_each_possible_cpu(cpu) {
		if (logical_cpu_to_clk(cpu) == &a53_bc_clk.c)
			cpumask_set_cpu(cpu, &a53_bc_clk.cpumask);
		if (logical_cpu_to_clk(cpu) == &a53_lc_clk.c)
			cpumask_set_cpu(cpu, &a53_lc_clk.cpumask);
	}

	a53_lc_clk.hw_low_power_ctrl = true;
	a53_bc_clk.hw_low_power_ctrl = true;

	register_pm_notifier(&clock_8939_pm_notifier);

	return 0;
}

static struct of_device_id clock_a53_match_table[] = {
	{.compatible = "qcom,cpu-clock-8939"},
	{}
};

static struct platform_driver clock_a53_driver = {
	.probe = clock_a53_probe,
	.driver = {
		.name = "cpu-clock-8939",
		.of_match_table = clock_a53_match_table,
		.owner = THIS_MODULE,
	},
};

static int __init clock_a53_init(void)
{
	return platform_driver_register(&clock_a53_driver);
}
arch_initcall(clock_a53_init);

static int __init cpu_clock_init_vote(void)
{
	struct device_node *ofnode = of_find_compatible_node(NULL, NULL,
			"qcom,cpu-clock-8939");
	if (!ofnode)
		return 0;

	if (of_property_read_bool(cpu_clock_8939_dev->dev.of_node,
			"qcom,cpu-pcnoc-vote"))
		return cpu_pcnoc_vote();
	return 0;
}
module_init(cpu_clock_init_vote);

#define APCS_C0_PLL			0xb116000
#define C0_PLL_MODE			0x0
#define C0_PLL_L_VAL			0x4
#define C0_PLL_M_VAL			0x8
#define C0_PLL_N_VAL			0xC
#define C0_PLL_USER_CTL			0x10
#define C0_PLL_CONFIG_CTL		0x14

#define APCS_ALIAS0_CMD_RCGR		0xb111050
#define APCS_ALIAS0_CFG_OFF		0x4
#define APCS_ALIAS0_CORE_CBCR_OFF	0x8
#define SRC_SEL				0x4
#define SRC_DIV				0x3

static void __init configure_enable_sr2_pll(void __iomem *base)
{
	/* Disable Mode */
	writel_relaxed(0x0, base + C0_PLL_MODE);

	/* Configure L/M/N values */
	writel_relaxed(0x34, base + C0_PLL_L_VAL);
	writel_relaxed(0x0,  base + C0_PLL_M_VAL);
	writel_relaxed(0x1,  base + C0_PLL_N_VAL);

	/* Configure USER_CTL and CONFIG_CTL value */
	writel_relaxed(0x0100000f, base + C0_PLL_USER_CTL);
	writel_relaxed(0x4c015765, base + C0_PLL_CONFIG_CTL);

	/* Enable PLL now */
	writel_relaxed(0x2, base + C0_PLL_MODE);
	udelay(2);
	writel_relaxed(0x6, base + C0_PLL_MODE);
	udelay(50);
	writel_relaxed(0x7, base + C0_PLL_MODE);
	mb();
}

static int __init cpu_clock_a53_init_little(void)
{
	void __iomem  *base;
	int regval = 0, count;
	struct device_node *ofnode = of_find_compatible_node(NULL, NULL,
							"qcom,cpu-clock-8939");
	if (!ofnode)
		return 0;

	base = ioremap_nocache(APCS_C0_PLL, SZ_32);
	configure_enable_sr2_pll(base);
	iounmap(base);

	base = ioremap_nocache(APCS_ALIAS0_CMD_RCGR, SZ_8);
	regval = readl_relaxed(base);
	/* Source GPLL0 and 1/2 the rate of GPLL0 */
	regval = (SRC_SEL << 8) | SRC_DIV; /* 0x403 */
	writel_relaxed(regval, base + APCS_ALIAS0_CFG_OFF);
	mb();

	/* update bit */
	regval = readl_relaxed(base);
	regval |= BIT(0);
	writel_relaxed(regval, base);

	/* Wait for update to take effect */
	for (count = 500; count > 0; count--) {
		if (!(readl_relaxed(base)) & BIT(0))
			break;
		udelay(1);
	}

	/* Enable the branch */
	regval =  readl_relaxed(base + APCS_ALIAS0_CORE_CBCR_OFF);
	regval |= BIT(0);
	writel_relaxed(regval, base + APCS_ALIAS0_CORE_CBCR_OFF);
	mb();
	iounmap(base);

	pr_info("A53 Power clocks configured\n");

	return 0;
}
early_initcall(cpu_clock_a53_init_little);
