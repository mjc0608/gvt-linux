/*
 * Copyright(c) 2011-2016 Intel Corporation. All rights reserved.
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
 */

#include <linux/acpi.h>
#include "i915_drv.h"
#include "gvt.h"

/*
 * Note: Only for GVT-g virtual VBT generation, other usage must
 * not do like this.
 */
#define _INTEL_BIOS_PRIVATE
#include "intel_vbt_defs.h"

#define OPREGION_SIGNATURE "IntelGraphicsMem"
#define MBOX_VBT      (1<<3)

/* device handle */
#define DEVICE_TYPE_CRT    0x01
#define DEVICE_TYPE_EFP1   0x04
#define DEVICE_TYPE_EFP2   0x40
#define DEVICE_TYPE_EFP3   0x20
#define DEVICE_TYPE_EFP4   0x10

#define DEV_SIZE	38

struct opregion_header {
	u8 signature[16];
	u32 size;
	u32 opregion_ver;
	u8 bios_ver[32];
	u8 vbios_ver[16];
	u8 driver_ver[16];
	u32 mboxes;
	u32 driver_model;
	u32 pcon;
	u8 dver[32];
	u8 rsvd[124];
} __packed;

struct bdb_data_header {
	u8 id;
	u16 size; /* data size */
} __packed;

struct vbt {
	/* header->bdb_offset point to bdb_header offset */
	struct vbt_header header;
	struct bdb_header bdb_header;

	struct bdb_data_header general_features_header;
	struct bdb_general_features general_features;

	struct bdb_data_header general_definitions_header;
	struct bdb_general_definitions general_definitions;
	struct child_device_config child0;
	struct child_device_config child1;
	struct child_device_config child2;
	struct child_device_config child3;

	struct bdb_data_header driver_features_header;
	struct bdb_driver_features driver_features;
};

static void virt_vbt_generation(struct vbt *v)
{
	int num_child;

	memset(v, 0, sizeof(struct vbt));

	v->header.signature[0] = '$';
	v->header.signature[1] = 'V';
	v->header.signature[2] = 'B';
	v->header.signature[3] = 'T';

	/* there's features depending on version! */
	v->header.version = 155;
	v->header.header_size = sizeof(v->header);
	v->header.vbt_size = sizeof(struct vbt) - sizeof(v->header);
	v->header.bdb_offset = offsetof(struct vbt, bdb_header);

	strcpy(&v->bdb_header.signature[0], "BIOS_DATA_BLOCK");
	v->bdb_header.version = 198; /* child_dev_size = 38 */
	v->bdb_header.header_size = sizeof(v->bdb_header);

	v->bdb_header.bdb_size = sizeof(struct vbt) - sizeof(struct vbt_header)
		- sizeof(struct bdb_header);

	/* general features */
	v->general_features_header.id = BDB_GENERAL_FEATURES;
	v->general_features_header.size = sizeof(struct bdb_general_features);
	v->general_features.int_crt_support = 0;
	v->general_features.int_tv_support = 0;

	/* child device */
	num_child = 4; /* each port has one child */
	v->general_definitions_header.id = BDB_GENERAL_DEFINITIONS;
	/* size will include child devices */
	v->general_definitions_header.size =
		sizeof(struct bdb_general_definitions) + num_child * DEV_SIZE;
	v->general_definitions.child_dev_size = DEV_SIZE;

	/* portA */
	v->child0.handle = DEVICE_TYPE_EFP1;
	v->child0.device_type = DEVICE_TYPE_DP;
	v->child0.dvo_port = DVO_PORT_DPA;
	v->child0.aux_channel = DP_AUX_A;

	/* portB */
	v->child1.handle = DEVICE_TYPE_EFP2;
	v->child1.device_type = DEVICE_TYPE_DP;
	v->child1.dvo_port = DVO_PORT_DPB;
	v->child1.aux_channel = DP_AUX_B;

	/* portC */
	v->child2.handle = DEVICE_TYPE_EFP3;
	v->child2.device_type = DEVICE_TYPE_DP;
	v->child2.dvo_port = DVO_PORT_DPC;
	v->child2.aux_channel = DP_AUX_C;

	/* portD */
	v->child3.handle = DEVICE_TYPE_EFP4;
	v->child3.device_type = DEVICE_TYPE_DP;
	v->child3.dvo_port = DVO_PORT_DPD;
	v->child3.aux_channel = DP_AUX_D;

	/* driver features */
	v->driver_features_header.id = BDB_DRIVER_FEATURES;
	v->driver_features_header.size = sizeof(struct bdb_driver_features);
	v->driver_features.lvds_config = BDB_DRIVER_FEATURE_NO_LVDS;
}

static int init_vgpu_opregion(struct intel_vgpu *vgpu, u32 gpa)
{
	int i;

	if (WARN((vgpu_opregion(vgpu)->va),
			"vgpu%d: opregion has been initialized already.\n",
			vgpu->id))
		return -EINVAL;

	vgpu_opregion(vgpu)->va = vgpu->gvt->opregion.opregion_va;

	for (i = 0; i < INTEL_GVT_OPREGION_PAGES; i++)
		vgpu_opregion(vgpu)->gfn[i] = (gpa >> PAGE_SHIFT) + i;

	return 0;
}

static int map_vgpu_opregion(struct intel_vgpu *vgpu, bool map)
{
	u64 mfn;
	int i, ret;

	for (i = 0; i < INTEL_GVT_OPREGION_PAGES; i++) {
		mfn = intel_gvt_hypervisor_virt_to_mfn(vgpu_opregion(vgpu)->va
			+ i * PAGE_SIZE);
		if (mfn == INTEL_GVT_INVALID_ADDR) {
			gvt_vgpu_err("fail to get MFN from VA\n");
			return -EINVAL;
		}
		ret = intel_gvt_hypervisor_map_gfn_to_mfn(vgpu,
				vgpu_opregion(vgpu)->gfn[i],
				mfn, 1, map);
		if (ret) {
			gvt_vgpu_err("fail to map GFN to MFN, errno: %d\n",
				ret);
			return ret;
		}
	}
	return 0;
}

/**
 * intel_vgpu_clean_opregion - clean the stuff used to emulate opregion
 * @vgpu: a vGPU
 *
 */
void intel_vgpu_clean_opregion(struct intel_vgpu *vgpu)
{
	gvt_dbg_core("vgpu%d: clean vgpu opregion\n", vgpu->id);

	if (!vgpu_opregion(vgpu)->va)
		return;

	if (intel_gvt_host.hypervisor_type == INTEL_GVT_HYPERVISOR_XEN) {
		map_vgpu_opregion(vgpu, false);
		free_pages((unsigned long)vgpu_opregion(vgpu)->va,
				get_order(INTEL_GVT_OPREGION_SIZE));

		vgpu_opregion(vgpu)->va = NULL;
	}
}

/**
 * intel_vgpu_init_opregion - initialize the stuff used to emulate opregion
 * @vgpu: a vGPU
 * @gpa: guest physical address of opregion
 *
 * Returns:
 * Zero on success, negative error code if failed.
 */
int intel_vgpu_init_opregion(struct intel_vgpu *vgpu, u32 gpa)
{
	int ret;

	gvt_dbg_core("vgpu%d: init vgpu opregion\n", vgpu->id);

	if (intel_gvt_host.hypervisor_type == INTEL_GVT_HYPERVISOR_XEN) {
		gvt_dbg_core("emulate opregion from kernel\n");

		ret = init_vgpu_opregion(vgpu, gpa);
		if (ret)
			return ret;

		ret = map_vgpu_opregion(vgpu, true);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * intel_gvt_clean_opregion - clean host opergion related stuffs
 * @gvt: a GVT device
 *
 */
void intel_gvt_clean_opregion(struct intel_gvt *gvt)
{
	free_pages((unsigned long)gvt->opregion.opregion_va,
			get_order(INTEL_GVT_OPREGION_SIZE));
	gvt->opregion.opregion_va = NULL;
}

/**
 * intel_gvt_init_opregion - initialize host opergion related stuffs
 * @gvt: a GVT device
 *
 * Returns:
 * Zero on success, negative error code if failed.
 */
int intel_gvt_init_opregion(struct intel_gvt *gvt)
{
	u8 *buf;
	struct opregion_header *header;
	struct vbt v;

	gvt_dbg_core("init host opregion\n");

	gvt->opregion.opregion_va = (void *)__get_free_pages(GFP_KERNEL |
			__GFP_ZERO,
			get_order(INTEL_GVT_OPREGION_SIZE));

	if (!gvt->opregion.opregion_va) {
		gvt_err("fail to get memory for virt opregion\n");
		return -ENOMEM;
	}

	/* emulated opregion with VBT mailbox only */
	header = (struct opregion_header *)gvt->opregion.opregion_va;
	memcpy(header->signature, OPREGION_SIGNATURE,
			sizeof(OPREGION_SIGNATURE));
	header->mboxes = MBOX_VBT;

	/* for unknown reason, the value in LID field is incorrect
	 * which block the windows guest, so workaround it by force
	 * setting it to "OPEN"
	 */
	buf = (u8 *)gvt->opregion.opregion_va;
	buf[INTEL_GVT_OPREGION_CLID] = 0x3;

	/* emulated vbt from virt vbt generation */
	virt_vbt_generation(&v);
	memcpy(gvt->opregion.opregion_va + INTEL_GVT_OPREGION_VBT_OFFSET,
			&v, sizeof(struct vbt));

	return 0;
}

#define GVT_OPREGION_FUNC(scic)					\
	({							\
	 u32 __ret;						\
	 __ret = (scic & OPREGION_SCIC_FUNC_MASK) >>		\
	 OPREGION_SCIC_FUNC_SHIFT;				\
	 __ret;							\
	 })

#define GVT_OPREGION_SUBFUNC(scic)				\
	({							\
	 u32 __ret;						\
	 __ret = (scic & OPREGION_SCIC_SUBFUNC_MASK) >>		\
	 OPREGION_SCIC_SUBFUNC_SHIFT;				\
	 __ret;							\
	 })

static const char *opregion_func_name(u32 func)
{
	const char *name = NULL;

	switch (func) {
	case 0 ... 3:
	case 5:
	case 7 ... 15:
		name = "Reserved";
		break;

	case 4:
		name = "Get BIOS Data";
		break;

	case 6:
		name = "System BIOS Callbacks";
		break;

	default:
		name = "Unknown";
		break;
	}
	return name;
}

static const char *opregion_subfunc_name(u32 subfunc)
{
	const char *name = NULL;

	switch (subfunc) {
	case 0:
		name = "Supported Calls";
		break;

	case 1:
		name = "Requested Callbacks";
		break;

	case 2 ... 3:
	case 8 ... 9:
		name = "Reserved";
		break;

	case 5:
		name = "Boot Display";
		break;

	case 6:
		name = "TV-Standard/Video-Connector";
		break;

	case 7:
		name = "Internal Graphics";
		break;

	case 10:
		name = "Spread Spectrum Clocks";
		break;

	case 11:
		name = "Get AKSV";
		break;

	default:
		name = "Unknown";
		break;
	}
	return name;
};

static bool querying_capabilities(u32 scic)
{
	u32 func, subfunc;

	func = GVT_OPREGION_FUNC(scic);
	subfunc = GVT_OPREGION_SUBFUNC(scic);

	if ((func == INTEL_GVT_OPREGION_SCIC_F_GETBIOSDATA &&
		subfunc == INTEL_GVT_OPREGION_SCIC_SF_SUPPRTEDCALLS)
		|| (func == INTEL_GVT_OPREGION_SCIC_F_GETBIOSDATA &&
		 subfunc == INTEL_GVT_OPREGION_SCIC_SF_REQEUSTEDCALLBACKS)
		|| (func == INTEL_GVT_OPREGION_SCIC_F_GETBIOSCALLBACKS &&
		 subfunc == INTEL_GVT_OPREGION_SCIC_SF_SUPPRTEDCALLS)) {
		return true;
	}
	return false;
}

/**
 * intel_vgpu_emulate_opregion_request - emulating OpRegion request
 * @vgpu: a vGPU
 * @swsci: SWSCI request
 *
 * Returns:
 * Zero on success, negative error code if failed
 */
int intel_vgpu_emulate_opregion_request(struct intel_vgpu *vgpu, u32 swsci)
{
	u32 *scic, *parm;
	u32 func, subfunc;

	scic = vgpu_opregion(vgpu)->va + INTEL_GVT_OPREGION_SCIC;
	parm = vgpu_opregion(vgpu)->va + INTEL_GVT_OPREGION_PARM;

	if (!(swsci & SWSCI_SCI_SELECT)) {
		gvt_vgpu_err("requesting SMI service\n");
		return 0;
	}
	/* ignore non 0->1 trasitions */
	if ((vgpu_cfg_space(vgpu)[INTEL_GVT_PCI_SWSCI]
				& SWSCI_SCI_TRIGGER) ||
			!(swsci & SWSCI_SCI_TRIGGER)) {
		return 0;
	}

	func = GVT_OPREGION_FUNC(*scic);
	subfunc = GVT_OPREGION_SUBFUNC(*scic);
	if (!querying_capabilities(*scic)) {
		gvt_vgpu_err("requesting runtime service: func \"%s\","
				" subfunc \"%s\"\n",
				opregion_func_name(func),
				opregion_subfunc_name(subfunc));
		/*
		 * emulate exit status of function call, '0' means
		 * "failure, generic, unsupported or unknown cause"
		 */
		*scic &= ~OPREGION_SCIC_EXIT_MASK;
		return 0;
	}

	*scic = 0;
	*parm = 0;
	return 0;
}
