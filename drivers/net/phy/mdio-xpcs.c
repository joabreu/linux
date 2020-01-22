// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 Synopsys, Inc. and/or its affiliates.
 * Synopsys DesignWare XPCS helpers
 *
 * Author: Jose Abreu <Jose.Abreu@synopsys.com>
 */

#include <linux/delay.h>
#include <linux/mdio.h>
#include <linux/mdio-xpcs.h>
#include <linux/phylink.h>

#define SYNOPSYS_XPCS_USXGMII_ID	0x7996ced0
#define SYNOPSYS_XPCS_USXGMII_MASK	0xffffffff

/* Vendor regs access */
#define DW_VENDOR			BIT(15)

/* VR_XS_PCS */
#define DW_USXGMII_RST			BIT(10)
#define DW_USXGMII_EN			BIT(9)
#define DW_VR_XS_PCS_DIG_STS		0x0010
#define DW_RXFIFO_ERR			GENMASK(6, 5)

/* SR_MII */
#define DW_USXGMII_FULL			BIT(8)
#define DW_USXGMII_SS_MASK		(BIT(13) | BIT(6) | BIT(5))
#define DW_USXGMII_10000		(BIT(13) | BIT(6))
#define DW_USXGMII_5000			(BIT(13) | BIT(5))
#define DW_USXGMII_2500			(BIT(5))
#define DW_USXGMII_1000			(BIT(6))
#define DW_USXGMII_100			(BIT(13))
#define DW_USXGMII_10			(0)

/* SR_AN */
#define DW_SR_AN_ADV1			0x10
#define DW_SR_AN_ADV2			0x11
#define DW_SR_AN_ADV3			0x12
#define DW_SR_AN_LP_ABL1		0x13
#define DW_SR_AN_LP_ABL2		0x14
#define DW_SR_AN_LP_ABL3		0x15

/* Clause 73 Defines */
/* AN_LP_ABL1 */
#define DW_C73_PAUSE			BIT(10)
#define DW_C73_ASYM_PAUSE		BIT(11)
#define DW_C73_AN_ADV_SF		0x1
/* AN_LP_ABL2 */
#define DW_C73_1000KX			BIT(5)
#define DW_C73_10000KX4			BIT(6)
#define DW_C73_10000KR			BIT(7)
/* AN_LP_ABL3 */
#define DW_C73_2500KX			BIT(0)
#define DW_C73_5000KR			BIT(1)

static const int xpcs_usxgmii_features[] = {
	ETHTOOL_LINK_MODE_Pause_BIT,
	ETHTOOL_LINK_MODE_Asym_Pause_BIT,
	ETHTOOL_LINK_MODE_Autoneg_BIT,
	ETHTOOL_LINK_MODE_1000baseKX_Full_BIT,
	ETHTOOL_LINK_MODE_10000baseKX4_Full_BIT,
	ETHTOOL_LINK_MODE_10000baseKR_Full_BIT,
	ETHTOOL_LINK_MODE_2500baseX_Full_BIT,
	__ETHTOOL_LINK_MODE_MASK_NBITS,
};

static const phy_interface_t xpcs_usxgmii_interfaces[] = {
	PHY_INTERFACE_MODE_USXGMII,
	PHY_INTERFACE_MODE_MAX,
};

static struct xpcs_id {
	u32 id;
	u32 mask;
	const int *supported;
	const phy_interface_t *interface;
} xpcs_id_list[] = {
	{
		.id = SYNOPSYS_XPCS_USXGMII_ID,
		.mask = SYNOPSYS_XPCS_USXGMII_MASK,
		.supported = xpcs_usxgmii_features,
		.interface = xpcs_usxgmii_interfaces,
	},
};

static int xpcs_read(struct phylink_config *config, int dev, u32 reg)
{
	u32 reg_addr = MII_ADDR_C45 | dev << 16 | reg;

	return mdiobus_read(config->pcs_bus, config->pcs_addr, reg_addr);
}

static int xpcs_write(struct phylink_config *config, int dev, u32 reg, u16 val)
{
	u32 reg_addr = MII_ADDR_C45 | dev << 16 | reg;

	return mdiobus_write(config->pcs_bus, config->pcs_addr, reg_addr, val);
}

static int xpcs_read_vendor(struct phylink_config *config, int dev, u32 reg)
{
	return xpcs_read(config, dev, DW_VENDOR | reg);
}

static int xpcs_write_vendor(struct phylink_config *config, int dev, int reg,
			     u16 val)
{
	return xpcs_write(config, dev, DW_VENDOR | reg, val);
}

static int xpcs_read_vpcs(struct phylink_config *config, int reg)
{
	return xpcs_read_vendor(config, MDIO_MMD_PCS, reg);
}

static int xpcs_write_vpcs(struct phylink_config *config, int reg, u16 val)
{
	return xpcs_write_vendor(config, MDIO_MMD_PCS, reg, val);
}

static int xpcs_poll_reset(struct phylink_config *config, int dev)
{
	/* Poll until the reset bit clears (50ms per retry == 0.6 sec) */
	unsigned int retries = 12;
	int ret;

	do {
		msleep(50);
		ret = xpcs_read(config, dev, MDIO_CTRL1);
		if (ret < 0)
			return ret;
	} while (ret & MDIO_CTRL1_RESET && --retries);

	return (ret & MDIO_CTRL1_RESET) ? -ETIMEDOUT : 0;
}

static int xpcs_soft_reset(struct phylink_config *config, int dev)
{
	int ret;

	ret = xpcs_write(config, dev, MDIO_CTRL1, MDIO_CTRL1_RESET);
	if (ret < 0)
		return ret;

	return xpcs_poll_reset(config, dev);
}

#define xpcs_warn(__config, __state, __args...) \
({ \
	if ((__state)->link) \
		dev_warn((__config)->dev, ##__args); \
})

static int xpcs_read_fault(struct phylink_config *config,
			   struct phylink_link_state *state)
{
	int ret;

	ret = xpcs_read(config, MDIO_MMD_PCS, MDIO_STAT1);
	if (ret < 0)
		return ret;

	if (ret & MDIO_STAT1_FAULT) {
		xpcs_warn(config, state, "Link fault condition detected!\n");
		return -EFAULT;
	}

	ret = xpcs_read(config, MDIO_MMD_PCS, MDIO_STAT2);
	if (ret < 0)
		return ret;

	if (ret & MDIO_STAT2_RXFAULT) {
		xpcs_warn(config, state, "Receiver fault detected!\n");
		return -EFAULT;
	}
	if (ret & MDIO_STAT2_TXFAULT) {
		xpcs_warn(config, state, "Transmitter fault detected!\n");
		return -EFAULT;
	}

	ret = xpcs_read_vendor(config, MDIO_MMD_PCS, DW_VR_XS_PCS_DIG_STS);
	if (ret < 0)
		return ret;

	if (ret & DW_RXFIFO_ERR) {
		xpcs_warn(config, state, "FIFO fault condition detected!\n");
		return -EFAULT;
	}

	ret = xpcs_read(config, MDIO_MMD_PCS, MDIO_PCS_10GBRT_STAT1);
	if (ret < 0)
		return ret;

	if (!(ret & MDIO_PCS_10GBRT_STAT1_BLKLK))
		xpcs_warn(config, state, "Link is not locked!\n");

	ret = xpcs_read(config, MDIO_MMD_PCS, MDIO_PCS_10GBRT_STAT2);
	if (ret < 0)
		return ret;

	if (ret & MDIO_PCS_10GBRT_STAT2_ERR)
		xpcs_warn(config, state, "Link has errors!\n");

	return 0;
}

static int xpcs_read_link(struct phylink_config *config)
{
	bool link = true;
	int ret;

	ret = xpcs_read(config, MDIO_MMD_PCS, MDIO_STAT1);
	if (ret < 0)
		return ret;

	if (!(ret & MDIO_STAT1_LSTATUS))
		link = false;

	ret = xpcs_read(config, MDIO_MMD_AN, MDIO_STAT1);
	if (ret < 0)
		return ret;

	if (!(ret & MDIO_STAT1_LSTATUS))
		link = false;

	return link;
}

static int xpcs_get_max_usxgmii_speed(const unsigned long *supported)
{
	int max = SPEED_UNKNOWN;

	if (phylink_test(supported, 1000baseKX_Full))
		max = SPEED_1000;
	if (phylink_test(supported, 2500baseX_Full))
		max = SPEED_2500;
	if (phylink_test(supported, 10000baseKX4_Full))
		max = SPEED_10000;
	if (phylink_test(supported, 10000baseKR_Full))
		max = SPEED_10000;

	return max;
}

static int xpcs_config_usxgmii(struct phylink_config *config, int speed)
{
	int ret, speed_sel;

	switch (speed) {
	case SPEED_10:
		speed_sel = DW_USXGMII_10;
		break;
	case SPEED_100:
		speed_sel = DW_USXGMII_100;
		break;
	case SPEED_1000:
		speed_sel = DW_USXGMII_1000;
		break;
	case SPEED_2500:
		speed_sel = DW_USXGMII_2500;
		break;
	case SPEED_5000:
		speed_sel = DW_USXGMII_5000;
		break;
	case SPEED_10000:
		speed_sel = DW_USXGMII_10000;
		break;
	default:
		/* Nothing to do here */
		return -EINVAL;
	}

	ret = xpcs_read_vpcs(config, MDIO_CTRL1);
	if (ret < 0)
		return ret;

	ret = xpcs_write_vpcs(config, MDIO_CTRL1, ret | DW_USXGMII_EN);
	if (ret < 0)
		return ret;

	ret = xpcs_read(config, MDIO_MMD_VEND2, MDIO_CTRL1);
	if (ret < 0)
		return ret;

	ret &= ~DW_USXGMII_SS_MASK;
	ret |= speed_sel | DW_USXGMII_FULL;

	ret = xpcs_write(config, MDIO_MMD_VEND2, MDIO_CTRL1, ret);
	if (ret < 0)
		return ret;

	ret = xpcs_read_vpcs(config, MDIO_CTRL1);
	if (ret < 0)
		return ret;

	return xpcs_write_vpcs(config, MDIO_CTRL1, ret | DW_USXGMII_RST);
}

static int xpcs_config_aneg_c73(struct phylink_config *config)
{
	int ret, adv;

	/* By default, in USXGMII mode XPCS operates at 10G baud and
	 * replicates data to achieve lower speeds. Hereby, in this
	 * default configuration we need to advertise all supported
	 * modes and not only the ones we want to use.
	 */

	/* SR_AN_ADV3 */
	adv = 0;
	if (phylink_test(config->pcs_supported, 2500baseX_Full))
		adv |= DW_C73_2500KX;

	/* TODO: 5000baseKR */

	ret = xpcs_write(config, MDIO_MMD_AN, DW_SR_AN_ADV3, adv);
	if (ret < 0)
		return ret;

	/* SR_AN_ADV2 */
	adv = 0;
	if (phylink_test(config->pcs_supported, 1000baseKX_Full))
		adv |= DW_C73_1000KX;
	if (phylink_test(config->pcs_supported, 10000baseKX4_Full))
		adv |= DW_C73_10000KX4;
	if (phylink_test(config->pcs_supported, 10000baseKR_Full))
		adv |= DW_C73_10000KR;

	ret = xpcs_write(config, MDIO_MMD_AN, DW_SR_AN_ADV2, adv);
	if (ret < 0)
		return ret;

	/* SR_AN_ADV1 */
	adv = DW_C73_AN_ADV_SF;
	if (phylink_test(config->pcs_supported, Pause))
		adv |= DW_C73_PAUSE;
	if (phylink_test(config->pcs_supported, Asym_Pause))
		adv |= DW_C73_ASYM_PAUSE;

	return xpcs_write(config, MDIO_MMD_AN, DW_SR_AN_ADV1, adv);
}

static int xpcs_config_aneg(struct phylink_config *config)
{
	int ret;

	ret = xpcs_config_aneg_c73(config);
	if (ret < 0)
		return ret;

	ret = xpcs_read(config, MDIO_MMD_AN, MDIO_CTRL1);
	if (ret < 0)
		return ret;

	ret |= MDIO_AN_CTRL1_ENABLE | MDIO_AN_CTRL1_RESTART;

	return xpcs_write(config, MDIO_MMD_AN, MDIO_CTRL1, ret);
}

static int xpcs_aneg_done(struct phylink_config *config,
			  struct phylink_link_state *state)
{
	int ret;

	ret = xpcs_read(config, MDIO_MMD_AN, MDIO_STAT1);
	if (ret < 0)
		return ret;

	if (ret & MDIO_AN_STAT1_COMPLETE) {
		ret = xpcs_read(config, MDIO_MMD_AN, DW_SR_AN_LP_ABL1);
		if (ret < 0)
			return ret;

		/* Check if Aneg outcome is valid */
		if (!(ret & DW_C73_AN_ADV_SF))
			return 0;

		return 1;
	}

	return 0;
}

static int xpcs_read_lpa(struct phylink_config *config,
			 struct phylink_link_state *state)
{
	int ret;

	ret = xpcs_read(config, MDIO_MMD_AN, MDIO_STAT1);
	if (ret < 0)
		return ret;

	if (!(ret & MDIO_AN_STAT1_LPABLE)) {
		phylink_clear(state->lp_advertising, Autoneg);
		return 0;
	}

	phylink_set(state->lp_advertising, Autoneg);

	/* Clause 73 outcome */
	ret = xpcs_read(config, MDIO_MMD_AN, DW_SR_AN_LP_ABL3);
	if (ret < 0)
		return ret;

	if (ret & DW_C73_2500KX)
		phylink_set(state->lp_advertising, 2500baseX_Full);

	ret = xpcs_read(config, MDIO_MMD_AN, DW_SR_AN_LP_ABL2);
	if (ret < 0)
		return ret;

	if (ret & DW_C73_1000KX)
		phylink_set(state->lp_advertising, 1000baseKX_Full);
	if (ret & DW_C73_10000KX4)
		phylink_set(state->lp_advertising, 10000baseKX4_Full);
	if (ret & DW_C73_10000KR)
		phylink_set(state->lp_advertising, 10000baseKR_Full);

	ret = xpcs_read(config, MDIO_MMD_AN, DW_SR_AN_LP_ABL1);
	if (ret < 0)
		return ret;

	if (ret & DW_C73_PAUSE)
		phylink_set(state->lp_advertising, Pause);
	if (ret & DW_C73_ASYM_PAUSE)
		phylink_set(state->lp_advertising, Asym_Pause);

	linkmode_and(state->lp_advertising, state->lp_advertising,
		     state->advertising);
	return 0;
}

static void xpcs_resolve_lpa(struct phylink_config *config,
			     struct phylink_link_state *state)
{
	int max_speed = xpcs_get_max_usxgmii_speed(state->lp_advertising);

	state->pause = MLO_PAUSE_SYM | MLO_PAUSE_ASYM;
	state->speed = max_speed;
	state->duplex = DUPLEX_FULL;
}

static u32 xpcs_get_id(struct phylink_config *config)
{
	int ret;
	u32 id;

	ret = xpcs_read(config, MDIO_MMD_PCS, MII_PHYSID1);
	if (ret < 0)
		return 0xffffffff;

	id = ret << 16;

	ret = xpcs_read(config, MDIO_MMD_PCS, MII_PHYSID2);
	if (ret < 0)
		return 0xffffffff;

	return id | ret;
}

static int xpcs_hw_probe(struct phylink_config *config,
			 phy_interface_t interface, unsigned long *supported)
{
	u32 xpcs_id = xpcs_get_id(config);
	struct xpcs_id *match = NULL;
	int i;

	for (i = 0; i < ARRAY_SIZE(xpcs_id_list); i++) {
		struct xpcs_id *entry = &xpcs_id_list[i];

		if ((xpcs_id & entry->mask) == entry->id) {
			match = entry;
			break;
		}
	}

	if (!match)
		return -ENODEV;

	for (i = 0; match->interface[i] != PHY_INTERFACE_MODE_MAX; i++) {
		if (match->interface[i] == interface)
			break;
	}

	if (match->interface[i] == PHY_INTERFACE_MODE_MAX)
		return -EINVAL;

	for (i = 0; match->supported[i] != __ETHTOOL_LINK_MODE_MASK_NBITS; i++)
		set_bit(match->supported[i], supported);

	return 0;
}

static void xpcs_validate(struct phylink_config *config,
			  unsigned long *supported,
			  struct phylink_link_state *state)
{
	linkmode_and(supported, supported, config->pcs_supported);
	linkmode_and(state->advertising, state->advertising,
		     config->pcs_supported);
}

static void xpcs_get_state(struct phylink_config *config,
			   struct phylink_link_state *state)
{
	int ret;

	/* Link needs to be read first ... */
	state->link = xpcs_read_link(config) > 0 ? 1 : 0;

	/* ... and then we check the faults. */
	ret = xpcs_read_fault(config, state);
	if (ret) {
		ret = xpcs_soft_reset(config, MDIO_MMD_PCS);
		if (ret)
			return;

		state->link = 0;

		xpcs_config_aneg(config);
		return;
	}

	if (state->link && xpcs_aneg_done(config, state)) {
		state->an_complete = true;
		xpcs_read_lpa(config, state);
		xpcs_resolve_lpa(config, state);
	}
}

static void xpcs_config(struct phylink_config *config, unsigned int mode,
			const struct phylink_link_state *state)
{
	if (state->an_enabled)
		xpcs_config_aneg(config);
}

static void xpcs_link_up(struct phylink_config *config, unsigned int mode,
			 phy_interface_t interface, int speed)
{
	xpcs_config_usxgmii(config, speed);
}

static struct phylink_pcs_ops xpcs_ops = {
	.hw_probe = xpcs_hw_probe,
	.validate = xpcs_validate,
	.get_state = xpcs_get_state,
	.config = xpcs_config,
	.link_down = NULL,
	.link_up = xpcs_link_up,
};

struct phylink_pcs_ops *mdio_xpcs_get_ops(void)
{
	return &xpcs_ops;
}
EXPORT_SYMBOL_GPL(mdio_xpcs_get_ops);

MODULE_LICENSE("GPL v2");
