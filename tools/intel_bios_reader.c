/*
 * Copyright � 2006 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "intel_bios.h"
#include "intel_io.h"
#include "intel_chipset.h"
#include "drmtest.h"

static uint32_t devid = -1;

/* no bother to include "edid.h" */
#define _H_ACTIVE(x) (x[2] + ((x[4] & 0xF0) << 4))
#define _H_SYNC_OFF(x) (x[8] + ((x[11] & 0xC0) << 2))
#define _H_SYNC_WIDTH(x) (x[9] + ((x[11] & 0x30) << 4))
#define _H_BLANK(x) (x[3] + ((x[4] & 0x0F) << 8))
#define _V_ACTIVE(x) (x[5] + ((x[7] & 0xF0) << 4))
#define _V_SYNC_OFF(x) ((x[10] >> 4) + ((x[11] & 0x0C) << 2))
#define _V_SYNC_WIDTH(x) ((x[10] & 0x0F) + ((x[11] & 0x03) << 4))
#define _V_BLANK(x) (x[6] + ((x[7] & 0x0F) << 8))
#define _PIXEL_CLOCK(x) (x[0] + (x[1] << 8)) * 10000

uint8_t *VBIOS;

#define INTEL_BIOS_8(_addr)	(VBIOS[_addr])
#define INTEL_BIOS_16(_addr)	(VBIOS[_addr] | \
				 (VBIOS[_addr + 1] << 8))
#define INTEL_BIOS_32(_addr)	(VBIOS[_addr] | \
				 (VBIOS[_addr + 1] << 8) | \
				 (VBIOS[_addr + 2] << 16) | \
				 (VBIOS[_addr + 3] << 24))

#define YESNO(val) ((val) ? "yes" : "no")

struct bdb_block {
	uint8_t id;
	uint16_t size;
	void *data;
};

static const char * const seq_name[] = {
	"UNDEFINED",
	"MIPI_SEQ_ASSERT_RESET",
	"MIPI_SEQ_INIT_OTP",
	"MIPI_SEQ_DISPLAY_ON",
	"MIPI_SEQ_DISPLAY_OFF",
	"MIPI_SEQ_DEASSERT_RESET",
};

struct bdb_header *bdb;
struct bdb_lvds_lfp_data_ptrs *lvds_lfp_data_ptrs;
static int tv_present;
static int lvds_present;
static int panel_type;

static struct bdb_block *find_section(int section_id, int length)
{
	struct bdb_block *block;
	unsigned char *base = (unsigned char *)bdb;
	int idx = 0;
	uint16_t total, current_size;
	unsigned char current_id;

	/* skip to first section */
	idx += bdb->header_size;
	total = bdb->bdb_size;
	if (total > length)
		total = length;

	block = malloc(sizeof(*block));
	if (!block) {
		fprintf(stderr, "out of memory\n");
		exit(-1);
	}

	/* walk the sections looking for section_id */
	while (idx + 3 < total) {
		current_id = *(base + idx);
		current_size = *(uint16_t *)(base + idx + 1);
		if (idx + current_size > total)
			return NULL;

		if (current_id == section_id) {
			block->id = current_id;
			block->size = current_size;
			block->data = base + idx + 3;
			return block;
		}

		idx += current_size + 3;
	}

	free(block);
	return NULL;
}

static void dump_general_features(const struct bdb_block *block)
{
	struct bdb_general_features *features = block->data;

	printf("\tPanel fitting: ");
	switch (features->panel_fitting) {
	case 0:
		printf("disabled\n");
		break;
	case 1:
		printf("text only\n");
		break;
	case 2:
		printf("graphics only\n");
		break;
	case 3:
		printf("text & graphics\n");
		break;
	}
	printf("\tFlexaim: %s\n", YESNO(features->flexaim));
	printf("\tMessage: %s\n", YESNO(features->msg_enable));
	printf("\tClear screen: %d\n", features->clear_screen);
	printf("\tDVO color flip required: %s\n", YESNO(features->color_flip));
	printf("\tExternal VBT: %s\n", YESNO(features->download_ext_vbt));
	printf("\tEnable SSC: %s\n", YESNO(features->enable_ssc));
	if (features->enable_ssc) {
		if (IS_VALLEYVIEW(devid) || IS_CHERRYVIEW(devid) ||
		    IS_BROXTON(devid))
			printf("\tSSC frequency: 100 MHz\n");
		else if (HAS_PCH_SPLIT(devid))
			printf("\tSSC frequency: %s\n", features->ssc_freq ?
			       "100 MHz" : "120 MHz");
		else
			printf("\tSSC frequency: %s\n", features->ssc_freq ?
			       "100 MHz (66 MHz on 855)" : "96 MHz (48 MHz on 855)");
	}
	printf("\tLFP on override: %s\n",
	       YESNO(features->enable_lfp_on_override));
	printf("\tDisable SSC on clone: %s\n",
	       YESNO(features->disable_ssc_ddt));
	printf("\tDisable smooth vision: %s\n",
	       YESNO(features->disable_smooth_vision));
	printf("\tSingle DVI for CRT/DVI: %s\n", YESNO(features->single_dvi));
	printf("\tLegacy monitor detect: %s\n",
	       YESNO(features->legacy_monitor_detect));
	printf("\tIntegrated CRT: %s\n", YESNO(features->int_crt_support));
	printf("\tIntegrated TV: %s\n", YESNO(features->int_tv_support));

	tv_present = 1;		/* should be based on whether TV DAC exists */
	lvds_present = 1;	/* should be based on IS_MOBILE() */
}

static void dump_backlight_info(const struct bdb_block *block)
{
	struct bdb_lvds_backlight *backlight = block->data;
	struct blc_struct *blc;

	if (sizeof(struct blc_struct) != backlight->blcstruct_size) {
		printf("\tBacklight struct sizes don't match (expected %zu, got %u), skipping\n",
		     sizeof(struct blc_struct), backlight->blcstruct_size);
		return;
	}

	blc = &backlight->panels[panel_type];

	printf("\tInverter type: %d\n", blc->inverter_type);
	printf("\t     polarity: %d\n", blc->inverter_polarity);
	printf("\t    GPIO pins: %d\n", blc->gpio_pins);
	printf("\t  GMBUS speed: %d\n", blc->gmbus_speed);
	printf("\t     PWM freq: %d\n", blc->pwm_freq);
	printf("\tMinimum brightness: %d\n", blc->min_brightness);
	printf("\tI2C slave addr: 0x%02x\n", blc->i2c_slave_addr);
	printf("\tI2C command: 0x%02x\n", blc->i2c_cmd);
}

static const struct {
	unsigned short type;
	const char *name;
} child_device_types[] = {
	{ DEVICE_TYPE_NONE, "none" },
	{ DEVICE_TYPE_CRT, "CRT" },
	{ DEVICE_TYPE_TV, "TV" },
	{ DEVICE_TYPE_EFP, "EFP" },
	{ DEVICE_TYPE_LFP, "LFP" },
	{ DEVICE_TYPE_CRT_DPMS, "CRT" },
	{ DEVICE_TYPE_CRT_DPMS_HOTPLUG, "CRT" },
	{ DEVICE_TYPE_TV_COMPOSITE, "TV composite" },
	{ DEVICE_TYPE_TV_MACROVISION, "TV" },
	{ DEVICE_TYPE_TV_RF_COMPOSITE, "TV" },
	{ DEVICE_TYPE_TV_SVIDEO_COMPOSITE, "TV S-Video" },
	{ DEVICE_TYPE_TV_SCART, "TV SCART" },
	{ DEVICE_TYPE_TV_CODEC_HOTPLUG_PWR, "TV" },
	{ DEVICE_TYPE_EFP_HOTPLUG_PWR, "EFP" },
	{ DEVICE_TYPE_EFP_DVI_HOTPLUG_PWR, "DVI" },
	{ DEVICE_TYPE_EFP_DVI_I, "DVI-I" },
	{ DEVICE_TYPE_EFP_DVI_D_DUAL, "DL-DVI-D" },
	{ DEVICE_TYPE_EFP_DVI_D_HDCP, "DVI-D" },
	{ DEVICE_TYPE_OPENLDI_HOTPLUG_PWR, "OpenLDI" },
	{ DEVICE_TYPE_OPENLDI_DUALPIX, "OpenLDI" },
	{ DEVICE_TYPE_LFP_PANELLINK, "PanelLink" },
	{ DEVICE_TYPE_LFP_CMOS_PWR, "CMOS LFP" },
	{ DEVICE_TYPE_LFP_LVDS_PWR, "LVDS" },
	{ DEVICE_TYPE_LFP_LVDS_DUAL, "LVDS" },
	{ DEVICE_TYPE_LFP_LVDS_DUAL_HDCP, "LVDS" },
	{ DEVICE_TYPE_INT_LFP, "LFP" },
	{ DEVICE_TYPE_INT_TV, "TV" },
	{ DEVICE_TYPE_DP, "DisplayPort" },
	{ DEVICE_TYPE_DP_HDMI_DVI, "DisplayPort/HDMI/DVI" },
	{ DEVICE_TYPE_DP_DVI, "DisplayPort/DVI" },
	{ DEVICE_TYPE_HDMI_DVI, "HDMI/DVI" },
	{ DEVICE_TYPE_DVI, "DVI" },
	{ DEVICE_TYPE_eDP, "eDP" },
};
static const int num_child_device_types =
	sizeof(child_device_types) / sizeof(child_device_types[0]);

static const char *child_device_type(unsigned short type)
{
	int i;

	for (i = 0; i < num_child_device_types; i++)
		if (child_device_types[i].type == type)
			return child_device_types[i].name;

	return "unknown";
}

static const struct {
	unsigned short type;
	const char *name;
} efp_ports[] = {
	{ DEVICE_PORT_NONE, "N/A" },
	{ DEVICE_PORT_HDMIB, "HDMI-B" },
	{ DEVICE_PORT_HDMIC, "HDMI-C" },
	{ DEVICE_PORT_HDMID, "HDMI-D" },
	{ DEVICE_PORT_DPB, "DP-B" },
	{ DEVICE_PORT_DPC, "DP-C" },
	{ DEVICE_PORT_DPD, "DP-D" },
};
static const int num_efp_ports = sizeof(efp_ports) / sizeof(efp_ports[0]);

static const char *efp_port(uint8_t type)
{
	int i;

	for (i = 0; i < num_efp_ports; i++)
		if (efp_ports[i].type == type)
			return efp_ports[i].name;

	return "unknown";
}

static const struct {
	unsigned short type;
	const char *name;
} efp_conn_info[] = {
	{ DEVICE_INFO_NONE, "N/A" },
	{ DEVICE_INFO_HDMI_CERT, "HDMI certified" },
	{ DEVICE_INFO_DP, "DisplayPort" },
	{ DEVICE_INFO_DVI, "DVI" },
};
static const int num_efp_conn_info = sizeof(efp_conn_info) / sizeof(efp_conn_info[0]);

static const char *efp_conn(uint8_t type)
{
	int i;

	for (i = 0; i < num_efp_conn_info; i++)
		if (efp_conn_info[i].type == type)
			return efp_conn_info[i].name;

	return "unknown";
}



static void dump_child_device(struct child_device_config *child)
{
	char child_id[11];

	if (!child->device_type)
		return;

	if (bdb->version < 152) {
		strncpy(child_id, (char *)child->device_id, 10);
		child_id[10] = 0;

		printf("\tChild device info:\n");
		printf("\t\tDevice type: %04x (%s)\n", child->device_type,
		       child_device_type(child->device_type));
		printf("\t\tSignature: %s\n", child_id);
		printf("\t\tAIM offset: %d\n", child->addin_offset);
		printf("\t\tDVO port: 0x%02x\n", child->dvo_port);
	} else { /* 152+ have EFP blocks here */
		struct efp_child_device_config *efp =
			(struct efp_child_device_config *)child;
		printf("\tEFP device info:\n");
		printf("\t\tDevice type: 0x%04x (%s)\n", efp->device_type,
		       child_device_type(efp->device_type));
		printf("\t\tPort: 0x%02x (%s)\n", efp->port,
		       efp_port(efp->port));
		printf("\t\tDDC pin: 0x%02x\n", efp->ddc_pin);
		printf("\t\tDock port: 0x%02x (%s)\n", efp->docked_port,
		       efp_port(efp->docked_port));
		printf("\t\tHDMI compatible? %s\n", efp->hdmi_compat ? "Yes" : "No");
		printf("\t\tInfo: %s\n", efp_conn(efp->conn_info));
		printf("\t\tAux channel: 0x%02x\n", efp->aux_chan);
		printf("\t\tDongle detect: 0x%02x\n", efp->dongle_detect);
	}
}

static void dump_general_definitions(const struct bdb_block *block)
{
	struct bdb_general_definitions *defs = block->data;
	struct child_device_config *child;
	int i;
	int child_device_num;

	printf("\tCRT DDC GMBUS addr: 0x%02x\n", defs->crt_ddc_gmbus_pin);
	printf("\tUse ACPI DPMS CRT power states: %s\n",
	       YESNO(defs->dpms_acpi));
	printf("\tSkip CRT detect at boot: %s\n",
	       YESNO(defs->skip_boot_crt_detect));
	printf("\tUse DPMS on AIM devices: %s\n", YESNO(defs->dpms_aim));
	printf("\tBoot display type: 0x%02x%02x\n", defs->boot_display[1],
	       defs->boot_display[0]);
	printf("\tTV data block present: %s\n", YESNO(tv_present));
	child_device_num = (block->size - sizeof(*defs)) / sizeof(*child);
	for (i = 0; i < child_device_num; i++)
		dump_child_device(&defs->devices[i]);
}

static void dump_child_devices(const struct bdb_block *block)
{
	struct bdb_child_devices *child_devs = block->data;
	struct child_device_config *child;
	int i;

	for (i = 0; i < DEVICE_CHILD_SIZE; i++) {
		child = &child_devs->children[i];
		/* Skip nonexistent children */
		if (!child->device_type)
			continue;
		printf("\tChild device %d\n", i);
		printf("\t\tType: 0x%04x (%s)\n", child->device_type,
		       child_device_type(child->device_type));
		printf("\t\tDVO port: 0x%02x\n", child->dvo_port);
		printf("\t\tI2C pin: 0x%02x\n", child->i2c_pin);
		printf("\t\tSlave addr: 0x%02x\n", child->slave_addr);
		printf("\t\tDDC pin: 0x%02x\n", child->ddc_pin);
		printf("\t\tDVO config: 0x%02x\n", child->dvo_cfg);
		printf("\t\tDVO wiring: 0x%02x\n", child->dvo_wiring);
	}
}

static void dump_lvds_options(const struct bdb_block *block)
{
	struct bdb_lvds_options *options = block->data;

	panel_type = options->panel_type;
	printf("\tPanel type: %d\n", panel_type);
	printf("\tLVDS EDID available: %s\n", YESNO(options->lvds_edid));
	printf("\tPixel dither: %s\n", YESNO(options->pixel_dither));
	printf("\tPFIT auto ratio: %s\n", YESNO(options->pfit_ratio_auto));
	printf("\tPFIT enhanced graphics mode: %s\n",
	       YESNO(options->pfit_gfx_mode_enhanced));
	printf("\tPFIT enhanced text mode: %s\n",
	       YESNO(options->pfit_text_mode_enhanced));
	printf("\tPFIT mode: %d\n", options->pfit_mode);
}

static void dump_lvds_ptr_data(const struct bdb_block *block)
{
	struct bdb_lvds_lfp_data_ptrs *ptrs = block->data;

	printf("\tNumber of entries: %d\n", ptrs->lvds_entries);

	/* save for use by dump_lvds_data() */
	lvds_lfp_data_ptrs = ptrs;
}

static void dump_lvds_data(const struct bdb_block *block)
{
	struct bdb_lvds_lfp_data *lvds_data = block->data;
	struct bdb_lvds_lfp_data_ptrs *ptrs = lvds_lfp_data_ptrs;
	int num_entries;
	int i;
	int hdisplay, hsyncstart, hsyncend, htotal;
	int vdisplay, vsyncstart, vsyncend, vtotal;
	float clock;
	int lfp_data_size, dvo_offset;

	if (!ptrs) {
		printf("No LVDS ptr block\n");
		return;
	}

	lfp_data_size =
	    ptrs->ptr[1].fp_timing_offset - ptrs->ptr[0].fp_timing_offset;
	dvo_offset =
	    ptrs->ptr[0].dvo_timing_offset - ptrs->ptr[0].fp_timing_offset;

	num_entries = block->size / lfp_data_size;

	printf("  Number of entries: %d (preferred block marked with '*')\n",
	       num_entries);

	for (i = 0; i < num_entries; i++) {
		uint8_t *lfp_data_ptr =
		    (uint8_t *) lvds_data->data + lfp_data_size * i;
		uint8_t *timing_data = lfp_data_ptr + dvo_offset;
		struct bdb_lvds_lfp_data_entry *lfp_data =
		    (struct bdb_lvds_lfp_data_entry *)lfp_data_ptr;
		char marker;

		if (i == panel_type)
			marker = '*';
		else
			marker = ' ';

		hdisplay = _H_ACTIVE(timing_data);
		hsyncstart = hdisplay + _H_SYNC_OFF(timing_data);
		hsyncend = hsyncstart + _H_SYNC_WIDTH(timing_data);
		htotal = hdisplay + _H_BLANK(timing_data);

		vdisplay = _V_ACTIVE(timing_data);
		vsyncstart = vdisplay + _V_SYNC_OFF(timing_data);
		vsyncend = vsyncstart + _V_SYNC_WIDTH(timing_data);
		vtotal = vdisplay + _V_BLANK(timing_data);
		clock = _PIXEL_CLOCK(timing_data) / 1000;

		printf("%c\tpanel type %02i: %dx%d clock %d\n", marker,
		       i, lfp_data->fp_timing.x_res, lfp_data->fp_timing.y_res,
		       _PIXEL_CLOCK(timing_data));
		printf("\t\tinfo:\n");
		printf("\t\t  LVDS: 0x%08lx\n",
		       (unsigned long)lfp_data->fp_timing.lvds_reg_val);
		printf("\t\t  PP_ON_DELAYS: 0x%08lx\n",
		       (unsigned long)lfp_data->fp_timing.pp_on_reg_val);
		printf("\t\t  PP_OFF_DELAYS: 0x%08lx\n",
		       (unsigned long)lfp_data->fp_timing.pp_off_reg_val);
		printf("\t\t  PP_DIVISOR: 0x%08lx\n",
		       (unsigned long)lfp_data->fp_timing.pp_cycle_reg_val);
		printf("\t\t  PFIT: 0x%08lx\n",
		       (unsigned long)lfp_data->fp_timing.pfit_reg_val);
		printf("\t\ttimings: %d %d %d %d %d %d %d %d %.2f (%s)\n",
		       hdisplay, hsyncstart, hsyncend, htotal,
		       vdisplay, vsyncstart, vsyncend, vtotal, clock,
		       (hsyncend > htotal || vsyncend > vtotal) ?
		       "BAD!" : "good");
	}
}

static void dump_driver_feature(const struct bdb_block *block)
{
	struct bdb_driver_feature *feature = block->data;

	printf("\tBoot Device Algorithm: %s\n", feature->boot_dev_algorithm ?
	       "driver default" : "os default");
	printf("\tBlock display switching when DVD active: %s\n",
	       YESNO(feature->block_display_switch));
	printf("\tAllow display switching when in Full Screen DOS: %s\n",
	       YESNO(feature->allow_display_switch));
	printf("\tHot Plug DVO: %s\n", YESNO(feature->hotplug_dvo));
	printf("\tDual View Zoom: %s\n", YESNO(feature->dual_view_zoom));
	printf("\tDriver INT 15h hook: %s\n", YESNO(feature->int15h_hook));
	printf("\tEnable Sprite in Clone Mode: %s\n",
	       YESNO(feature->sprite_in_clone));
	printf("\tUse 00000110h ID for Primary LFP: %s\n",
	       YESNO(feature->primary_lfp_id));
	printf("\tBoot Mode X: %u\n", feature->boot_mode_x);
	printf("\tBoot Mode Y: %u\n", feature->boot_mode_y);
	printf("\tBoot Mode Bpp: %u\n", feature->boot_mode_bpp);
	printf("\tBoot Mode Refresh: %u\n", feature->boot_mode_refresh);
	printf("\tEnable LFP as primary: %s\n",
	       YESNO(feature->enable_lfp_primary));
	printf("\tSelective Mode Pruning: %s\n",
	       YESNO(feature->selective_mode_pruning));
	printf("\tDual-Frequency Graphics Technology: %s\n",
	       YESNO(feature->dual_frequency));
	printf("\tDefault Render Clock Frequency: %s\n",
	       feature->render_clock_freq ? "low" : "high");
	printf("\tNT 4.0 Dual Display Clone Support: %s\n",
	       YESNO(feature->nt_clone_support));
	printf("\tDefault Power Scheme user interface: %s\n",
	       feature->power_scheme_ui ? "3rd party" : "CUI");
	printf
	    ("\tSprite Display Assignment when Overlay is Active in Clone Mode: %s\n",
	     feature->sprite_display_assign ? "primary" : "secondary");
	printf("\tDisplay Maintain Aspect Scaling via CUI: %s\n",
	       YESNO(feature->cui_aspect_scaling));
	printf("\tPreserve Aspect Ratio: %s\n",
	       YESNO(feature->preserve_aspect_ratio));
	printf("\tEnable SDVO device power down: %s\n",
	       YESNO(feature->sdvo_device_power_down));
	printf("\tCRT hotplug: %s\n", YESNO(feature->crt_hotplug));
	printf("\tLVDS config: ");
	switch (feature->lvds_config) {
	case BDB_DRIVER_NO_LVDS:
		printf("No LVDS\n");
		break;
	case BDB_DRIVER_INT_LVDS:
		printf("Integrated LVDS\n");
		break;
	case BDB_DRIVER_SDVO_LVDS:
		printf("SDVO LVDS\n");
		break;
	case BDB_DRIVER_EDP:
		printf("Embedded DisplayPort\n");
		break;
	}
	printf("\tDefine Display statically: %s\n",
	       YESNO(feature->static_display));
	printf("\tLegacy CRT max X: %d\n", feature->legacy_crt_max_x);
	printf("\tLegacy CRT max Y: %d\n", feature->legacy_crt_max_y);
	printf("\tLegacy CRT max refresh: %d\n",
	       feature->legacy_crt_max_refresh);
}

static void dump_edp(const struct bdb_block *block)
{
	struct bdb_edp *edp = block->data;
	int bpp, msa;
	int i;

	for (i = 0; i < 16; i++) {
		printf("\tPanel %d%s\n", i, panel_type == i ? " *" : "");

		printf("\t\tPower Sequence: T3 %d T7 %d T9 %d T10 %d T12 %d\n",
		       edp->power_seqs[i].t3,
		       edp->power_seqs[i].t7,
		       edp->power_seqs[i].t9,
		       edp->power_seqs[i].t10,
		       edp->power_seqs[i].t12);

		bpp = (edp->color_depth >> (i * 2)) & 3;

		printf("\t\tPanel color depth: ");
		switch (bpp) {
		case EDP_18BPP:
			printf("18 bpp\n");
			break;
		case EDP_24BPP:
			printf("24 bpp\n");
			break;
		case EDP_30BPP:
			printf("30 bpp\n");
			break;
		default:
			printf("(unknown value %d)\n", bpp);
			break;
		}

		msa = (edp->sdrrs_msa_timing_delay >> (i * 2)) & 3;
		printf("\t\teDP sDRRS MSA Delay: Lane %d\n", msa + 1);

		printf("\t\tLink params:\n");
		printf("\t\t\trate: ");
		if (edp->link_params[i].rate == EDP_RATE_1_62)
			printf("1.62G\n");
		else if (edp->link_params[i].rate == EDP_RATE_2_7)
			printf("2.7G\n");
		printf("\t\t\tlanes: ");
		switch (edp->link_params[i].lanes) {
		case EDP_LANE_1:
			printf("x1 mode\n");
			break;
		case EDP_LANE_2:
			printf("x2 mode\n");
			break;
		case EDP_LANE_4:
			printf("x4 mode\n");
			break;
		default:
			printf("(unknown value %d)\n",
			       edp->link_params[i].lanes);
			break;
		}
		printf("\t\t\tpre-emphasis: ");
		switch (edp->link_params[i].preemphasis) {
		case EDP_PREEMPHASIS_NONE:
			printf("none\n");
			break;
		case EDP_PREEMPHASIS_3_5dB:
			printf("3.5dB\n");
			break;
		case EDP_PREEMPHASIS_6dB:
			printf("6dB\n");
			break;
		case EDP_PREEMPHASIS_9_5dB:
			printf("9.5dB\n");
			break;
		default:
			printf("(unknown value %d)\n",
			       edp->link_params[i].preemphasis);
			break;
		}
		printf("\t\t\tvswing: ");
		switch (edp->link_params[i].vswing) {
		case EDP_VSWING_0_4V:
			printf("0.4V\n");
			break;
		case EDP_VSWING_0_6V:
			printf("0.6V\n");
			break;
		case EDP_VSWING_0_8V:
			printf("0.8V\n");
			break;
		case EDP_VSWING_1_2V:
			printf("1.2V\n");
			break;
		default:
			printf("(unknown value %d)\n",
			       edp->link_params[i].vswing);
			break;
		}
	}
}

static void
print_detail_timing_data(struct lvds_dvo_timing2 *dvo_timing)
{
	int display, sync_start, sync_end, total;

	display = (dvo_timing->hactive_hi << 8) | dvo_timing->hactive_lo;
	sync_start = display +
		((dvo_timing->hsync_off_hi << 8) | dvo_timing->hsync_off_lo);
	sync_end = sync_start + dvo_timing->hsync_pulse_width;
	total = display +
		((dvo_timing->hblank_hi << 8) | dvo_timing->hblank_lo);
	printf("\thdisplay: %d\n", display);
	printf("\thsync [%d, %d] %s\n", sync_start, sync_end,
	       dvo_timing->hsync_positive ? "+sync" : "-sync");
	printf("\thtotal: %d\n", total);

	display = (dvo_timing->vactive_hi << 8) | dvo_timing->vactive_lo;
	sync_start = display + dvo_timing->vsync_off;
	sync_end = sync_start + dvo_timing->vsync_pulse_width;
	total = display +
		((dvo_timing->vblank_hi << 8) | dvo_timing->vblank_lo);
	printf("\tvdisplay: %d\n", display);
	printf("\tvsync [%d, %d] %s\n", sync_start, sync_end,
	       dvo_timing->vsync_positive ? "+sync" : "-sync");
	printf("\tvtotal: %d\n", total);

	printf("\tclock: %d\n", dvo_timing->clock * 10);
}

static void dump_sdvo_panel_dtds(const struct bdb_block *block)
{
	struct lvds_dvo_timing2 *dvo_timing = block->data;
	int n, count;

	count = block->size / sizeof(struct lvds_dvo_timing2);
	for (n = 0; n < count; n++) {
		printf("%d:\n", n);
		print_detail_timing_data(dvo_timing++);
	}
}

static void dump_sdvo_lvds_options(const struct bdb_block *block)
{
	struct bdb_sdvo_lvds_options *options = block->data;

	printf("\tbacklight: %d\n", options->panel_backlight);
	printf("\th40 type: %d\n", options->h40_set_panel_type);
	printf("\ttype: %d\n", options->panel_type);
	printf("\tssc_clk_freq: %d\n", options->ssc_clk_freq);
	printf("\tals_low_trip: %d\n", options->als_low_trip);
	printf("\tals_high_trip: %d\n", options->als_high_trip);
	/*
	u8 sclalarcoeff_tab_row_num;
	u8 sclalarcoeff_tab_row_size;
	u8 coefficient[8];
	*/
	printf("\tmisc[0]: %x\n", options->panel_misc_bits_1);
	printf("\tmisc[1]: %x\n", options->panel_misc_bits_2);
	printf("\tmisc[2]: %x\n", options->panel_misc_bits_3);
	printf("\tmisc[3]: %x\n", options->panel_misc_bits_4);
}

static void dump_mipi_config(const struct bdb_block *block)
{
	struct bdb_mipi_config *start = block->data;
	struct mipi_config *config;
	struct mipi_pps_data *pps;

	config = &start->config[panel_type];
	pps = &start->pps[panel_type];

	printf("\tGeneral Param\n");
	printf("\t\t BTA disable: %s\n", config->bta ? "Disabled" : "Enabled");

	printf("\t\t Video Mode Color Format: ");
	if (config->videomode_color_format == 0)
		printf("Not supported\n");
	else if (config->videomode_color_format == 1)
		printf("RGB565\n");
	else if (config->videomode_color_format == 2)
		printf("RGB666\n");
	else if (config->videomode_color_format == 3)
		printf("RGB666 Loosely Packed\n");
	else if (config->videomode_color_format == 4)
		printf("RGB888\n");
	printf("\t\t PPS GPIO Pins: %s \n", config->pwm_blc ? "Using SOC" : "Using PMIC");
	printf("\t\t CABC Support: %s\n", config->cabc ? "supported" : "not supported");
	//insert video mode type
	printf("\t\t Mode: %s\n", config->cmd_mode ? "COMMAND" : "VIDEO");
	printf("\t\t Dithering: %s\n", config->dithering ? "done in Display Controller" : "done in Panel Controller");

	printf("\tPort Desc\n");
	//insert pixel overlap count
	printf("\t\t Lane Count: %d\n", config->lane_cnt + 1);
	printf("\t\t Dual Link Support: ");
	if (config->dual_link == 0)
		printf("not supported\n");
	else if (config->dual_link == 1)
		printf("Front Back mode\n");
	else
		printf("Pixel Alternative Mode\n");

	printf("\tDphy Flags\n");
	printf("\t\t Clock Stop: %s\n", config->clk_stop ? "ENABLED" : "DISABLED");
	printf("\t\t EOT disabled: %s\n\n", config->eot_disabled ? "EOT not to be sent" : "EOT to be sent");

	printf("\tHSTxTimeOut: 0x%x\n", config->hs_tx_timeout);
	printf("\tLPRXTimeOut: 0x%x\n", config->lp_rx_timeout);
	printf("\tTurnAroundTimeOut: 0x%x\n", config->turn_around_timeout);
	printf("\tDeviceResetTimer: 0x%x\n", config->device_reset_timer);
	printf("\tMasterinitTimer: 0x%x\n", config->master_init_timer);
	printf("\tDBIBandwidthTimer: 0x%x\n", config->dbi_bw_timer);
	printf("\tLpByteClkValue: 0x%x\n\n", config->lp_byte_clk_val);

	printf("\tDphy Params\n");
	printf("\t\tExit to zero Count: 0x%x\n", config->exit_zero_cnt);
	printf("\t\tTrail Count: 0x%X\n", config->trail_cnt);
	printf("\t\tClk zero count: 0x%x\n", config->clk_zero_cnt);
	printf("\t\tPrepare count:0x%x\n\n", config->prepare_cnt);

	printf("\tClockLaneSwitchingCount: 0x%x\n", config->clk_lane_switch_cnt);
	printf("\tHighToLowSwitchingCount: 0x%x\n\n", config->hl_switch_cnt);

	printf("\tTimings based on Dphy spec\n");
	printf("\t\tTClkMiss: 0x%x\n", config->tclk_miss);
	printf("\t\tTClkPost: 0x%x\n", config->tclk_post);
	printf("\t\tTClkPre: 0x%x\n", config->tclk_pre);
	printf("\t\tTClkPrepare: 0x%x\n", config->tclk_prepare);
	printf("\t\tTClkSettle: 0x%x\n", config->tclk_settle);
	printf("\t\tTClkTermEnable: 0x%x\n\n", config->tclk_term_enable);

	printf("\tTClkTrail: 0x%x\n", config->tclk_trail);
	printf("\tTClkPrepareTClkZero: 0x%x\n", config->tclk_prepare_clkzero);
	printf("\tTHSExit: 0x%x\n", config->ths_exit);
	printf("\tTHsPrepare: 0x%x\n", config->ths_prepare);
	printf("\tTHsPrepareTHsZero: 0x%x\n", config->ths_prepare_hszero);
	printf("\tTHSSettle: 0x%x\n", config->ths_settle);
	printf("\tTHSSkip: 0x%x\n", config->ths_skip);
	printf("\tTHsTrail: 0x%x\n", config->ths_trail);
	printf("\tTInit: 0x%x\n", config->tinit);
	printf("\tTLPX: 0x%x\n", config->tlpx);

	printf("\tMIPI PPS\n");
	printf("\t\tPanel power ON delay: %d\n", pps->panel_on_delay);
	printf("\t\tPanel power on to Baklight enable delay: %d\n", pps->bl_enable_delay);
	printf("\t\tBacklight disable to Panel power OFF delay: %d\n", pps->bl_disable_delay);
	printf("\t\tPanel power OFF delay: %d\n", pps->panel_off_delay);
	printf("\t\tPanel power cycle delay: %d\n", pps->panel_power_cycle_delay);
}

static uint8_t *mipi_dump_send_packet(uint8_t *data)
{
	uint8_t type, byte, count;
	uint16_t len;

	byte = *data++;
	/* get packet type and increment the pointer */
	type = *data++;

	len = *((uint16_t *) data);
	data += 2;
	printf("\t\t SEND COMMAND: ");
	printf("0x%x 0x%x 0x%x", byte, type, len);
	for (count = 0; count < len; count++)
		printf(" 0x%x",*(data+count));
	printf("\n");
	data += len;
	return data;
}

static uint8_t *mipi_dump_delay(uint8_t *data)
{
	printf("\t\t Delay : 0x%x 0x%x 0x%x 0x%x\n", data[0], data[1], data[2], data[3]);
	data += 4;
	return data;
}

static uint8_t *mipi_dump_gpio(uint8_t *data)
{
	uint8_t gpio, action;

	printf("\t\t GPIO value:");
	gpio = *data++;

	/* pull up/down */
	action = *data++;
	printf(" 0x%x 0x%x\n", gpio, action);
	return data;
}

typedef uint8_t * (*fn_mipi_elem_dump)(uint8_t *data);

static const fn_mipi_elem_dump dump_elem[] = {
	NULL, /* reserved */
	mipi_dump_send_packet,
	mipi_dump_delay,
	mipi_dump_gpio,
	NULL, /* status read; later */
};

static void dump_sequence(uint8_t *sequence)
{
	uint8_t *data = sequence;
	fn_mipi_elem_dump mipi_elem_dump;
	int index_no;

	if (!sequence)
		return;

	printf("\tSequence Name: %s\n", seq_name[*data]);

	/* go to the first element of the sequence */
	data++;

	/* parse each byte till we reach end of sequence byte - 0x00 */
	while (1) {
		index_no = *data;
		mipi_elem_dump = dump_elem[index_no];
		if (!mipi_elem_dump) {
			printf("Error: Unsupported MIPI element, skipping sequence execution\n");
			return;
		}
		/* goto element payload */
		data++;

		/* execute the element specifc rotines */
		data = mipi_elem_dump(data);

		/*
		 * After processing the element, data should point to
		 * next element or end of sequence
		 * check if have we reached end of sequence
		 */

		if (*data == 0x00)
			break;
	}
}

static uint8_t *goto_next_sequence(uint8_t *data, int *size)
{
	uint16_t len;
	int tmp = *size;

	if (--tmp < 0)
		return NULL;

	/* goto first element */
	data++;
	while (1) {
		switch (*data) {
		case MIPI_SEQ_ELEM_SEND_PKT:
			/*
			 * skip by this element payload size
			 * skip elem id, command flag and data type
			 */
			tmp -= 5;
			if (tmp < 0)
				return NULL;

			data += 3;
			len = *((uint16_t *)data);

			tmp -= len;
			if (tmp < 0)
				return NULL;

			/* skip by len */
			data = data + 2 + len;
			break;
		case MIPI_SEQ_ELEM_DELAY:
			/* skip by elem id, and delay is 4 bytes */
			tmp -= 5;
			if (tmp < 0)
				return NULL;

			data += 5;
			break;
		case MIPI_SEQ_ELEM_GPIO:
			tmp -= 3;
			if (tmp < 0)
				return NULL;

			data += 3;
			break;
		default:
			printf("Unknown element\n");
			return NULL;
                }

		/* end of sequence ? */
		if (*data == 0)
			break;
        }

	/* goto next sequence or end of block byte */
	if (--tmp < 0)
		return NULL;

        data++;

	/* update amount of data left for the sequence block to be parsed */
	*size = tmp;
	return data;
}

static uint16_t get_blocksize(void *p)
{
	uint16_t *block_ptr, block_size;

	block_ptr = (uint16_t *)((char *)p - 2);
	block_size = *block_ptr;
	return block_size;
}

static void dump_mipi_sequence(const struct bdb_block *block)
{
	struct bdb_mipi_sequence *sequence = block->data;
	uint8_t *data, *seq_data;
	int i, panel_id, seq_size;
	uint16_t block_size;

	/* Check if we have sequence block as well */
	if (!sequence) {
		printf("No MIPI Sequence found\n");
		return;
	}

	block_size = get_blocksize(sequence);

	/*
	 * parse the sequence block for individual sequences
	 */
	seq_data = &sequence->data[0];

	/*
	 * sequence block is variable length and hence we need to parse and
	 * get the sequence data for specific panel id
	 */
	for (i = 0; i < MAX_MIPI_CONFIGURATIONS; i++) {
		panel_id = *seq_data;
		seq_size = *((uint16_t *) (seq_data + 1));
		if (panel_id == panel_type)
			break;

		/* skip the sequence including seq header of 3 bytes */
		seq_data = seq_data + 3 + seq_size;
		if ((seq_data - &sequence->data[0]) > block_size) {
			printf("Sequence start is beyond sequence block size, corrupted sequence block\n");
			return;
		}
	}

	if (i == MAX_MIPI_CONFIGURATIONS) {
		printf("Sequence block detected but no valid configuration\n");
		return;
	}

	/* check if found sequence is completely within the sequence block
	 * just being paranoid */
	if (seq_size > block_size) {
		printf("Corrupted sequence/size, bailing out\n");
		return;
        }

	/* skip the panel id(1 byte) and seq size(2 bytes) */
	data = (uint8_t *) calloc(1, seq_size);
	if (data)
		memmove(data, seq_data + 3, seq_size);
	else {
		printf("Memory not allocated for sequence data\n");
		return;
	}
	/*
	 * loop into the sequence data and split into multiple sequneces
	 * There are only 5 types of sequences as of now
	 */

	/* two consecutive 0x00 indicate end of all sequences */
        while (1) {
		int seq_id = *data;
		if (MIPI_SEQ_MAX > seq_id && seq_id > MIPI_SEQ_UNDEFINED)
			dump_sequence(data);
		 else {
			printf("Error:undefined sequence\n");
			goto err;
                }

		/* partial parsing to skip elements */
		data = goto_next_sequence(data, &seq_size);

		if (data == NULL) {
			printf("Sequence elements going beyond block itself. Sequence block parsing failed\n");
			goto err;
                }

		if (*data == 0)
			break; /* end of sequence reached */
	}
        return;

err:
	free(data);
	data = NULL;
}


static int
get_device_id(unsigned char *bios)
{
    int device;
    int offset = (bios[0x19] << 8) + bios[0x18];

    if (bios[offset] != 'P' ||
	bios[offset+1] != 'C' ||
	bios[offset+2] != 'I' ||
	bios[offset+3] != 'R')
	return -1;

    device = (bios[offset+7] << 8) + bios[offset+6];

    return device;
}

struct dumper {
	uint8_t id;
	const char *name;
	void (*dump)(const struct bdb_block *block);
};

struct dumper dumpers[] = {
	{
		.id = BDB_GENERAL_FEATURES,
		.name = "General features block",
		.dump = dump_general_features,
	},
	{
		.id = BDB_GENERAL_DEFINITIONS,
		.name = "General definitions block",
		.dump = dump_general_definitions,
	},
	{
		.id = BDB_CHILD_DEVICE_TABLE,
		.name = "Child devices block",
		.dump = dump_child_devices,
	},
	{
		.id = BDB_LVDS_OPTIONS,
		.name = "LVDS options block",
		.dump = dump_lvds_options,
	},
	{
		.id = BDB_LVDS_LFP_DATA_PTRS,
		.name = "LVDS timing pointer data",
		.dump = dump_lvds_ptr_data,
	},
	{
		.id = BDB_LVDS_LFP_DATA,
		.name = "LVDS panel data block",
		.dump = dump_lvds_data,
	},
	{
		.id = BDB_LVDS_BACKLIGHT,
		.name = "Backlight info block",
		.dump = dump_backlight_info,
	},
	{
		.id = BDB_SDVO_LVDS_OPTIONS,
		.name = "SDVO LVDS options block",
		.dump = dump_sdvo_lvds_options,
	},
	{
		.id = BDB_SDVO_PANEL_DTDS,
		.name = "SDVO panel dtds",
		.dump = dump_sdvo_panel_dtds,
	},
	{
		.id = BDB_DRIVER_FEATURES,
		.name = "Driver feature data block",
		.dump = dump_driver_feature,
	},
	{
		.id = BDB_EDP,
		.name = "eDP block",
		.dump = dump_edp,
	},
	{
		.id = BDB_MIPI_CONFIG,
		.name = "MIPI configuration block",
		.dump = dump_mipi_config,
	},
	{
		.id = BDB_MIPI_SEQUENCE,
		.name = "MIPI sequence block",
		.dump = dump_mipi_sequence,
	},
};

static void hex_dump(const struct bdb_block *block)
{
	int i;
	uint8_t *p = block->data;

	for (i = 0; i < block->size; i++) {
		if (i % 16 == 0)
			printf("\t%04x: ", i);
		printf("%02x", p[i]);
		if (i % 16 == 15) {
			if (i + 1 < block->size)
				printf("\n");
		} else if (i % 8 == 7) {
			printf("  ");
		} else {
			printf(" ");
		}
	}
	printf("\n\n");
}

static void dump_section(int section_id, int size)
{
	struct dumper *dumper = NULL;
	const struct bdb_block *block;
	static int done[256];
	int i;

	if (done[section_id])
		return;
	done[section_id] = 1;

	block = find_section(section_id, size);
	if (!block)
		return;

	for (i = 0; i < ARRAY_SIZE(dumpers); i++) {
		if (block->id == dumpers[i].id) {
			dumper = &dumpers[i];
			break;
		}
	}

	if (dumper && dumper->name)
		printf("BDB block %d - %s:\n", block->id, dumper->name);
	else
		printf("BDB block %d:\n", block->id);

	hex_dump(block);
	if (dumper && dumper->dump)
		dumper->dump(block);
	printf("\n");
}

int main(int argc, char **argv)
{
	int fd;
	struct vbt_header *vbt = NULL;
	int vbt_off, bdb_off, i;
	const char *filename = "bios";
	struct stat finfo;
	int size;
	struct bdb_block *block;
	char signature[17];
	char *devid_string;

	if (argc != 2) {
		printf("usage: %s <rom file>\n", argv[0]);
		return 1;
	}

	if ((devid_string = getenv("DEVICE")))
	    devid = strtoul(devid_string, NULL, 0);

	filename = argv[1];

	fd = open(filename, O_RDONLY);
	if (fd == -1) {
		printf("Couldn't open \"%s\": %s\n", filename, strerror(errno));
		return 1;
	}

	if (stat(filename, &finfo)) {
		printf("failed to stat \"%s\": %s\n", filename,
		       strerror(errno));
		return 1;
	}
	size = finfo.st_size;

	if (size == 0) {
		int len = 0, ret;
		size = 8192;
		VBIOS = malloc (size);
		while ((ret = read(fd, VBIOS + len, size - len))) {
			if (ret < 0) {
				printf("failed to read \"%s\": %s\n", filename,
				       strerror(errno));
				return 1;
			}

			len += ret;
			if (len == size) {
				size *= 2;
				VBIOS = realloc(VBIOS, size);
			}
		}
	} else {
		VBIOS = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
		if (VBIOS == MAP_FAILED) {
			printf("failed to map \"%s\": %s\n", filename, strerror(errno));
			return 1;
		}
	}

	/* Scour memory looking for the VBT signature */
	for (i = 0; i + 4 < size; i++) {
		if (!memcmp(VBIOS + i, "$VBT", 4)) {
			vbt_off = i;
			vbt = (struct vbt_header *)(VBIOS + i);
			break;
		}
	}

	if (!vbt) {
		printf("VBT signature missing\n");
		return 1;
	}

	printf("VBT vers: %d.%d\n", vbt->version / 100, vbt->version % 100);

	bdb_off = vbt_off + vbt->bdb_offset;
	if (bdb_off >= size - sizeof(struct bdb_header)) {
		printf("Invalid VBT found, BDB points beyond end of data block\n");
		return 1;
	}

	bdb = (struct bdb_header *)(VBIOS + bdb_off);
	strncpy(signature, (char *)bdb->signature, 16);
	signature[16] = 0;
	printf("BDB sig: %s\n", signature);
	printf("BDB vers: %d\n", bdb->version);

	printf("Available sections: ");

	for (i = 0; i < 256; i++) {
		block = find_section(i, size);
		if (!block)
			continue;
		printf("%d ", i);
		free(block);
	}
	printf("\n");

	if (devid == -1)
	    devid = get_device_id(VBIOS);
	if (devid == -1)
	    printf("Warning: could not find PCI device ID!\n");

	dump_section(BDB_GENERAL_FEATURES, size);
	dump_section(BDB_GENERAL_DEFINITIONS, size);
	dump_section(BDB_CHILD_DEVICE_TABLE, size);
	dump_section(BDB_LVDS_OPTIONS, size);
	dump_section(BDB_LVDS_LFP_DATA_PTRS, size);
	dump_section(BDB_LVDS_LFP_DATA, size);
	dump_section(BDB_LVDS_BACKLIGHT, size);

	dump_section(BDB_SDVO_LVDS_OPTIONS, size);
	dump_section(BDB_SDVO_PANEL_DTDS, size);

	dump_section(BDB_DRIVER_FEATURES, size);
	dump_section(BDB_EDP, size);
	dump_section(BDB_MIPI_CONFIG, size);
	dump_section(BDB_MIPI_SEQUENCE, size);

	for (i = 0; i < 256; i++)
		dump_section(i, size);

	return 0;
}
