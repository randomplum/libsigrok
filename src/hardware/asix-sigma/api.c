/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2010-2012 Håvard Espeland <gus@ping.uio.no>,
 * Copyright (C) 2010 Martin Stensgård <mastensg@ping.uio.no>
 * Copyright (C) 2010 Carl Henrik Lunde <chlunde@ping.uio.no>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include "protocol.h"

/*
 * Channel numbers seem to go from 1-16, according to this image:
 * http://tools.asix.net/img/sigma_sigmacab_pins_720.jpg
 * (the cable has two additional GND pins, and a TI and TO pin)
 */
static const char *channel_names[] = {
	"1", "2", "3", "4", "5", "6", "7", "8",
	"9", "10", "11", "12", "13", "14", "15", "16",
};

static const uint32_t scanopts[] = {
	SR_CONF_CONN,
};

static const uint32_t drvopts[] = {
	SR_CONF_LOGIC_ANALYZER,
};

static const uint32_t devopts[] = {
	SR_CONF_LIMIT_MSEC | SR_CONF_GET | SR_CONF_SET,
	SR_CONF_LIMIT_SAMPLES | SR_CONF_GET | SR_CONF_SET,
	SR_CONF_CONN | SR_CONF_GET,
	SR_CONF_SAMPLERATE | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST,
#if ASIX_SIGMA_WITH_TRIGGER
	SR_CONF_TRIGGER_MATCH | SR_CONF_LIST,
	SR_CONF_CAPTURE_RATIO | SR_CONF_GET | SR_CONF_SET,
#endif
};

#if ASIX_SIGMA_WITH_TRIGGER
static const int32_t trigger_matches[] = {
	SR_TRIGGER_ZERO,
	SR_TRIGGER_ONE,
	SR_TRIGGER_RISING,
	SR_TRIGGER_FALLING,
};
#endif

static void clear_helper(struct dev_context *devc)
{
	ftdi_deinit(&devc->ftdic);
}

static int dev_clear(const struct sr_dev_driver *di)
{
	return std_dev_clear_with_callback(di, (std_dev_clear_callback)clear_helper);
}

static gboolean bus_addr_in_devices(int bus, int addr, GSList *devs)
{
	struct sr_usb_dev_inst *usb;

	for (/* EMPTY */; devs; devs = devs->next) {
		usb = devs->data;
		if (usb->bus == bus && usb->address == addr)
			return TRUE;
	}

	return FALSE;
}

static gboolean known_vid_pid(const struct libusb_device_descriptor *des)
{
	if (des->idVendor != USB_VENDOR_ASIX)
		return FALSE;
	if (des->idProduct != USB_PRODUCT_SIGMA && des->idProduct != USB_PRODUCT_OMEGA)
		return FALSE;
	return TRUE;
}

static GSList *scan(struct sr_dev_driver *di, GSList *options)
{
	struct drv_context *drvc;
	libusb_context *usbctx;
	const char *conn;
	GSList *l, *conn_devices;
	struct sr_config *src;
	GSList *devices;
	libusb_device **devlist, *devitem;
	int bus, addr;
	struct libusb_device_descriptor des;
	struct libusb_device_handle *hdl;
	int ret;
	char conn_id[20];
	char serno_txt[16];
	char *end;
	long serno_num, serno_pre;
	enum asix_device_type dev_type;
	const char *dev_text;
	struct sr_dev_inst *sdi;
	struct dev_context *devc;
	size_t devidx, chidx;

	drvc = di->context;
	usbctx = drvc->sr_ctx->libusb_ctx;

	/* Find all devices which match an (optional) conn= spec. */
	conn = NULL;
	for (l = options; l; l = l->next) {
		src = l->data;
		switch (src->key) {
		case SR_CONF_CONN:
			conn = g_variant_get_string(src->data, NULL);
			break;
		}
	}
	conn_devices = NULL;
	if (conn)
		conn_devices = sr_usb_find(usbctx, conn);
	if (conn && !conn_devices)
		return NULL;

	/* Find all ASIX logic analyzers (which match the connection spec). */
	devices = NULL;
	libusb_get_device_list(usbctx, &devlist);
	for (devidx = 0; devlist[devidx]; devidx++) {
		devitem = devlist[devidx];

		/* Check for connection match if a user spec was given. */
		bus = libusb_get_bus_number(devitem);
		addr = libusb_get_device_address(devitem);
		if (conn && !bus_addr_in_devices(bus, addr, conn_devices))
			continue;
		snprintf(conn_id, sizeof(conn_id), "%d.%d", bus, addr);

		/*
		 * Check for known VID:PID pairs. Get the serial number,
		 * to then derive the device type from it.
		 */
		libusb_get_device_descriptor(devitem, &des);
		if (!known_vid_pid(&des))
			continue;
		if (!des.iSerialNumber) {
			sr_warn("Cannot get serial number (index 0).");
			continue;
		}
		ret = libusb_open(devitem, &hdl);
		if (ret < 0) {
			sr_warn("Cannot open USB device %04x.%04x: %s.",
				des.idVendor, des.idProduct,
				libusb_error_name(ret));
			continue;
		}
		ret = libusb_get_string_descriptor_ascii(hdl,
			des.iSerialNumber,
			(unsigned char *)serno_txt, sizeof(serno_txt));
		if (ret < 0) {
			sr_warn("Cannot get serial number (%s).",
				libusb_error_name(ret));
			libusb_close(hdl);
			continue;
		}
		libusb_close(hdl);

		/*
		 * All ASIX logic analyzers have a serial number, which
		 * reads as a hex number, and tells the device type.
		 */
		ret = sr_atol_base(serno_txt, &serno_num, &end, 16);
		if (ret != SR_OK || !end || *end) {
			sr_warn("Cannot interpret serial number %s.", serno_txt);
			continue;
		}
		dev_type = ASIX_TYPE_NONE;
		dev_text = NULL;
		serno_pre = serno_num >> 16;
		switch (serno_pre) {
		case 0xa601:
			dev_type = ASIX_TYPE_SIGMA;
			dev_text = "SIGMA";
			sr_info("Found SIGMA, serno %s.", serno_txt);
			break;
		case 0xa602:
			dev_type = ASIX_TYPE_SIGMA;
			dev_text = "SIGMA2";
			sr_info("Found SIGMA2, serno %s.", serno_txt);
			break;
		case 0xa603:
			dev_type = ASIX_TYPE_OMEGA;
			dev_text = "OMEGA";
			sr_info("Found OMEGA, serno %s.", serno_txt);
			if (!ASIX_WITH_OMEGA) {
				sr_warn("OMEGA support is not implemented yet.");
				continue;
			}
			break;
		default:
			sr_warn("Unknown serno %s, skipping.", serno_txt);
			continue;
		}

		/* Create a device instance, add it to the result set. */

		sdi = g_malloc0(sizeof(*sdi));
		devices = g_slist_append(devices, sdi);
		sdi->status = SR_ST_INITIALIZING;
		sdi->vendor = g_strdup("ASIX");
		sdi->model = g_strdup(dev_text);
		sdi->serial_num = g_strdup(serno_txt);
		sdi->connection_id = g_strdup(conn_id);
		for (chidx = 0; chidx < ARRAY_SIZE(channel_names); chidx++)
			sr_channel_new(sdi, chidx, SR_CHANNEL_LOGIC,
				TRUE, channel_names[chidx]);

		devc = g_malloc0(sizeof(*devc));
		sdi->priv = devc;
		devc->id.vid = des.idVendor;
		devc->id.pid = des.idProduct;
		devc->id.serno = serno_num;
		devc->id.prefix = serno_pre;
		devc->id.type = dev_type;
		devc->samplerate = samplerates[0];
		sr_sw_limits_init(&devc->cfg_limits);
		devc->cur_firmware = -1;
		devc->capture_ratio = 50;
		devc->use_triggers = 0;
	}
	libusb_free_device_list(devlist, 1);
	g_slist_free_full(conn_devices, (GDestroyNotify)sr_usb_dev_inst_free);

	return std_scan_complete(di, devices);
}

static int dev_open(struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	long vid, pid;
	const char *serno;
	int ret;

	devc = sdi->priv;

	if (devc->id.type == ASIX_TYPE_OMEGA && !ASIX_WITH_OMEGA) {
		sr_err("OMEGA support is not implemented yet.");
		return SR_ERR_NA;
	}
	vid = devc->id.vid;
	pid = devc->id.pid;
	serno = sdi->serial_num;

	ret = ftdi_init(&devc->ftdic);
	if (ret < 0) {
		sr_err("Cannot initialize FTDI context (%d): %s.",
			ret, ftdi_get_error_string(&devc->ftdic));
		return SR_ERR_IO;
	}
	ret = ftdi_usb_open_desc_index(&devc->ftdic, vid, pid, NULL, serno, 0);
	if (ret < 0) {
		sr_err("Cannot open device (%d): %s.",
			ret, ftdi_get_error_string(&devc->ftdic));
		return SR_ERR_IO;
	}

	return SR_OK;
}

static int dev_close(struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	int ret;

	devc = sdi->priv;

	ret = ftdi_usb_close(&devc->ftdic);
	ftdi_deinit(&devc->ftdic);

	return (ret == 0) ? SR_OK : SR_ERR;
}

static int config_get(uint32_t key, GVariant **data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	struct dev_context *devc;

	(void)cg;

	if (!sdi)
		return SR_ERR;
	devc = sdi->priv;

	switch (key) {
	case SR_CONF_CONN:
		*data = g_variant_new_string(sdi->connection_id);
		break;
	case SR_CONF_SAMPLERATE:
		*data = g_variant_new_uint64(devc->samplerate);
		break;
	case SR_CONF_LIMIT_MSEC:
	case SR_CONF_LIMIT_SAMPLES:
		return sr_sw_limits_config_get(&devc->cfg_limits, key, data);
#if ASIX_SIGMA_WITH_TRIGGER
	case SR_CONF_CAPTURE_RATIO:
		*data = g_variant_new_uint64(devc->capture_ratio);
		break;
#endif
	default:
		return SR_ERR_NA;
	}

	return SR_OK;
}

static int config_set(uint32_t key, GVariant *data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	struct dev_context *devc;
	int ret;
	uint64_t want_rate, have_rate;

	(void)cg;

	devc = sdi->priv;

	switch (key) {
	case SR_CONF_SAMPLERATE:
		want_rate = g_variant_get_uint64(data);
		ret = sigma_normalize_samplerate(want_rate, &have_rate);
		if (ret != SR_OK)
			return ret;
		if (have_rate != want_rate) {
			char *text_want, *text_have;
			text_want = sr_samplerate_string(want_rate);
			text_have = sr_samplerate_string(have_rate);
			sr_info("Adjusted samplerate %s to %s.",
				text_want, text_have);
			g_free(text_want);
			g_free(text_have);
		}
		devc->samplerate = have_rate;
		break;
	case SR_CONF_LIMIT_MSEC:
	case SR_CONF_LIMIT_SAMPLES:
		return sr_sw_limits_config_set(&devc->cfg_limits, key, data);
#if ASIX_SIGMA_WITH_TRIGGER
	case SR_CONF_CAPTURE_RATIO:
		devc->capture_ratio = g_variant_get_uint64(data);
		break;
#endif
	default:
		return SR_ERR_NA;
	}

	return SR_OK;
}

static int config_list(uint32_t key, GVariant **data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	switch (key) {
	case SR_CONF_SCAN_OPTIONS:
	case SR_CONF_DEVICE_OPTIONS:
		if (cg)
			return SR_ERR_NA;
		return STD_CONFIG_LIST(key, data, sdi, cg, scanopts, drvopts, devopts);
	case SR_CONF_SAMPLERATE:
		*data = std_gvar_samplerates(samplerates, samplerates_count);
		break;
#if ASIX_SIGMA_WITH_TRIGGER
	case SR_CONF_TRIGGER_MATCH:
		*data = std_gvar_array_i32(ARRAY_AND_SIZE(trigger_matches));
		break;
#endif
	default:
		return SR_ERR_NA;
	}

	return SR_OK;
}

static int dev_acquisition_start(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	struct clockselect_50 clockselect;
	int triggerpin, ret;
	uint8_t triggerselect;
	struct triggerinout triggerinout_conf;
	struct triggerlut lut;
	uint8_t regval;
	uint8_t clock_bytes[sizeof(clockselect)];
	size_t clock_idx;

	devc = sdi->priv;

	/*
	 * Setup the device's samplerate from the value which up to now
	 * just got checked and stored. As a byproduct this can pick and
	 * send firmware to the device, reduce the number of available
	 * logic channels, etc.
	 *
	 * Determine an acquisition timeout from optionally configured
	 * sample count or time limits. Which depends on the samplerate.
	 */
	ret = sigma_set_samplerate(sdi);
	if (ret != SR_OK)
		return ret;
	ret = sigma_set_acquire_timeout(devc);
	if (ret != SR_OK)
		return ret;

	if (sigma_convert_trigger(sdi) != SR_OK) {
		sr_err("Failed to configure triggers.");
		return SR_ERR;
	}

	/* Enter trigger programming mode. */
	sigma_set_register(WRITE_TRIGGER_SELECT2, 0x20, devc);

	triggerselect = 0;
	if (devc->samplerate >= SR_MHZ(100)) {
		/* 100 and 200 MHz mode. */
		sigma_set_register(WRITE_TRIGGER_SELECT2, 0x81, devc);

		/* Find which pin to trigger on from mask. */
		for (triggerpin = 0; triggerpin < 8; triggerpin++)
			if ((devc->trigger.risingmask | devc->trigger.fallingmask) &
			    (1 << triggerpin))
				break;

		/* Set trigger pin and light LED on trigger. */
		triggerselect = (1 << LEDSEL1) | (triggerpin & 0x7);

		/* Default rising edge. */
		if (devc->trigger.fallingmask)
			triggerselect |= 1 << 3;

	} else if (devc->samplerate <= SR_MHZ(50)) {
		/* All other modes. */
		sigma_build_basic_trigger(&lut, devc);

		sigma_write_trigger_lut(&lut, devc);

		triggerselect = (1 << LEDSEL1) | (1 << LEDSEL0);
	}

	/* Setup trigger in and out pins to default values. */
	memset(&triggerinout_conf, 0, sizeof(struct triggerinout));
	triggerinout_conf.trgout_bytrigger = 1;
	triggerinout_conf.trgout_enable = 1;

	sigma_write_register(WRITE_TRIGGER_OPTION,
			     (uint8_t *) &triggerinout_conf,
			     sizeof(struct triggerinout), devc);

	/* Go back to normal mode. */
	sigma_set_register(WRITE_TRIGGER_SELECT2, triggerselect, devc);

	/* Set clock select register. */
	clockselect.async = 0;
	clockselect.fraction = 1 - 1;		/* Divider 1. */
	clockselect.disabled_channels = 0x0000;	/* All channels enabled. */
	if (devc->samplerate == SR_MHZ(200)) {
		/* Enable 4 channels. */
		clockselect.disabled_channels = 0xf0ff;
	} else if (devc->samplerate == SR_MHZ(100)) {
		/* Enable 8 channels. */
		clockselect.disabled_channels = 0x00ff;
	} else {
		/*
		 * 50 MHz mode, or fraction thereof. The 50MHz reference
		 * can get divided by any integer in the range 1 to 256.
		 * Divider minus 1 gets written to the hardware.
		 * (The driver lists a discrete set of sample rates, but
		 * all of them fit the above description.)
		 */
		clockselect.fraction = SR_MHZ(50) / devc->samplerate - 1;
	}
	clock_idx = 0;
	clock_bytes[clock_idx++] = clockselect.async;
	clock_bytes[clock_idx++] = clockselect.fraction;
	clock_bytes[clock_idx++] = clockselect.disabled_channels & 0xff;
	clock_bytes[clock_idx++] = clockselect.disabled_channels >> 8;
	sigma_write_register(WRITE_CLOCK_SELECT, clock_bytes, clock_idx, devc);

	/* Setup maximum post trigger time. */
	sigma_set_register(WRITE_POST_TRIGGER,
			   (devc->capture_ratio * 255) / 100, devc);

	/* Start acqusition. */
	regval =  WMR_TRGRES | WMR_SDRAMWRITEEN;
#if ASIX_SIGMA_WITH_TRIGGER
	regval |= WMR_TRGEN;
#endif
	sigma_set_register(WRITE_MODE, regval, devc);

	std_session_send_df_header(sdi);

	/* Add capture source. */
	sr_session_source_add(sdi->session, -1, 0, 10, sigma_receive_data, (void *)sdi);

	devc->state.state = SIGMA_CAPTURE;

	return SR_OK;
}

static int dev_acquisition_stop(struct sr_dev_inst *sdi)
{
	struct dev_context *devc;

	devc = sdi->priv;

	/*
	 * When acquisition is currently running, keep the receive
	 * routine registered and have it stop the acquisition upon the
	 * next invocation. Else unregister the receive routine here
	 * already. The detour is required to have sample data retrieved
	 * for forced acquisition stops.
	 */
	if (devc->state.state == SIGMA_CAPTURE) {
		devc->state.state = SIGMA_STOPPING;
	} else {
		devc->state.state = SIGMA_IDLE;
		sr_session_source_remove(sdi->session, -1);
	}

	return SR_OK;
}

static struct sr_dev_driver asix_sigma_driver_info = {
	.name = "asix-sigma",
	.longname = "ASIX SIGMA/SIGMA2",
	.api_version = 1,
	.init = std_init,
	.cleanup = std_cleanup,
	.scan = scan,
	.dev_list = std_dev_list,
	.dev_clear = dev_clear,
	.config_get = config_get,
	.config_set = config_set,
	.config_list = config_list,
	.dev_open = dev_open,
	.dev_close = dev_close,
	.dev_acquisition_start = dev_acquisition_start,
	.dev_acquisition_stop = dev_acquisition_stop,
	.context = NULL,
};
SR_REGISTER_DEV_DRIVER(asix_sigma_driver_info);
