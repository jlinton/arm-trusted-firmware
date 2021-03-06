/*
 * Copyright (c) 2015-2019, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>

#include <libfdt.h>

#include <platform_def.h>
#include <arch_helpers.h>
#include <bl31/interrupt_mgmt.h>
#include <common/bl_common.h>
#include <lib/libc/endian.h>
#include <lib/mmio.h>
#include <lib/xlat_tables/xlat_mmu_helpers.h>
#include <lib/xlat_tables/xlat_tables_defs.h>
#include <lib/xlat_tables/xlat_tables_v2.h>
#include <plat/common/platform.h>
#include <common/fdt_fixup.h>
#include <common/fdt_wrappers.h>
#include <libfdt.h>

#include <drivers/arm/gicv2.h>

#include <rpi_shared.h>

/*
 * Fields at the beginning of armstub8.bin.
 * While building the BL31 image, we put the stub magic into the binary.
 * The GPU firmware detects this at boot time, clears that field as a
 * confirmation and puts the kernel and DT address in the following words.
 */
extern uint32_t stub_magic;
extern uint32_t dtb_ptr32;
extern uint32_t kernel_entry32;

#define SECURE_TRIGGER  32

static const interrupt_prop_t rpi4_interrupt_props[] = {
	INTR_PROP_DESC(SECURE_TRIGGER, GIC_HIGHEST_SEC_PRIORITY, GICV2_INTR_GROUP0, GIC_INTR_CFG_LEVEL), // take over the "arm mailbox"
};

static const gicv2_driver_data_t rpi4_gic_data = {
	.gicd_base = RPI4_GIC_GICD_BASE,
	.gicc_base = RPI4_GIC_GICC_BASE,
	.interrupt_props = rpi4_interrupt_props,
	.interrupt_props_num = ARRAY_SIZE(rpi4_interrupt_props),
};

/*
 * To be filled by the code below. At the moment BL32 is not supported.
 * In the future these might be passed down from BL2.
 */
static entry_point_info_t bl32_image_ep_info;
static entry_point_info_t bl33_image_ep_info;

/*******************************************************************************
 * Return a pointer to the 'entry_point_info' structure of the next image for
 * the security state specified. BL33 corresponds to the non-secure image type
 * while BL32 corresponds to the secure image type. A NULL pointer is returned
 * if the image does not exist.
 ******************************************************************************/
entry_point_info_t *bl31_plat_get_next_image_ep_info(uint32_t type)
{
	entry_point_info_t *next_image_info;

	assert(sec_state_is_valid(type) != 0);

	next_image_info = (type == NON_SECURE)
			? &bl33_image_ep_info : &bl32_image_ep_info;

	/* None of the images can have 0x0 as the entrypoint. */
	if (next_image_info->pc) {
		return next_image_info;
	} else {
		return NULL;
	}
}

uintptr_t plat_get_ns_image_entrypoint(void)
{
#ifdef PRELOADED_BL33_BASE
	return PRELOADED_BL33_BASE;
#else
	/* Cleared by the GPU if kernel address is valid. */
	if (stub_magic == 0)
		return kernel_entry32;

	WARN("Stub magic failure, using default kernel address 0x80000\n");
	return 0x80000;
#endif
}

static uintptr_t rpi4_get_dtb_address(void)
{
#ifdef RPI3_PRELOADED_DTB_BASE
	return RPI3_PRELOADED_DTB_BASE;
#else
	/* Cleared by the GPU if DTB address is valid. */
	if (stub_magic == 0)
		return dtb_ptr32;

	WARN("Stub magic failure, DTB address unknown\n");
	return 0;
#endif
}

static void ldelay(register_t delay)
{
	__asm__ volatile (
		"1:\tcbz %0, 2f\n\t"
		"sub %0, %0, #1\n\t"
		"b 1b\n"
		"2:"
		: "=&r" (delay) : "0" (delay)
	);
}

/*******************************************************************************
 * Perform any BL31 early platform setup. Here is an opportunity to copy
 * parameters passed by the calling EL (S-EL1 in BL2 & EL3 in BL1) before
 * they are lost (potentially). This needs to be done before the MMU is
 * initialized so that the memory layout can be used while creating page
 * tables. BL2 has flushed this information to memory, so we are guaranteed
 * to pick up good data.
 ******************************************************************************/
void bl31_early_platform_setup2(u_register_t arg0, u_register_t arg1,
				u_register_t arg2, u_register_t arg3)

{
	/*
	 * LOCAL_CONTROL:
	 * Bit 9 clear: Increment by 1 (vs. 2).
	 * Bit 8 clear: Timer source is 19.2MHz crystal (vs. APB).
	 */
	mmio_write_32(RPI4_LOCAL_CONTROL_BASE_ADDRESS, 0);

	/* LOCAL_PRESCALER; divide-by (0x80000000 / register_val) == 1 */
	mmio_write_32(RPI4_LOCAL_CONTROL_PRESCALER, 0x80000000);

	/* Early GPU firmware revisions need a little break here. */
	ldelay(100000);

	/* Initialize the console to provide early debug support. */
	rpi3_console_init();

	bl33_image_ep_info.pc = plat_get_ns_image_entrypoint();
	bl33_image_ep_info.spsr = rpi3_get_spsr_for_bl33_entry();
	SET_SECURITY_STATE(bl33_image_ep_info.h.attr, NON_SECURE);

#if RPI3_DIRECT_LINUX_BOOT
# if RPI3_BL33_IN_AARCH32
	/*
	 * According to the file ``Documentation/arm/Booting`` of the Linux
	 * kernel tree, Linux expects:
	 * r0 = 0
	 * r1 = machine type number, optional in DT-only platforms (~0 if so)
	 * r2 = Physical address of the device tree blob
	 */
	VERBOSE("rpi4: Preparing to boot 32-bit Linux kernel\n");
	bl33_image_ep_info.args.arg0 = 0U;
	bl33_image_ep_info.args.arg1 = ~0U;
	bl33_image_ep_info.args.arg2 = rpi4_get_dtb_address();
# else
	/*
	 * According to the file ``Documentation/arm64/booting.txt`` of the
	 * Linux kernel tree, Linux expects the physical address of the device
	 * tree blob (DTB) in x0, while x1-x3 are reserved for future use and
	 * must be 0.
	 */
	VERBOSE("rpi4: Preparing to boot 64-bit Linux kernel\n");
	bl33_image_ep_info.args.arg0 = rpi4_get_dtb_address();
	bl33_image_ep_info.args.arg1 = 0ULL;
	bl33_image_ep_info.args.arg2 = 0ULL;
	bl33_image_ep_info.args.arg3 = 0ULL;
# endif /* RPI3_BL33_IN_AARCH32 */
#endif /* RPI3_DIRECT_LINUX_BOOT */
}

void bl31_plat_arch_setup(void)
{
	/*
	 * Is the dtb_ptr32 pointer valid? If yes, map the DTB region.
	 * We map the 2MB region the DTB start address lives in, plus
	 * the next 2MB, to have enough room for expansion.
	 */
	if (stub_magic == 0) {
		unsigned long long dtb_region = dtb_ptr32;

		dtb_region &= ~0x1fffff;	/* Align to 2 MB. */
		mmap_add_region(dtb_region, dtb_region, 4U << 20,
				MT_MEMORY | MT_RW | MT_NS);
	}
	/*
	 * Add the first page of memory, which holds the stub magic,
	 * the kernel and the DT address.
	 * This also holds the secondary CPU's entrypoints and mailboxes.
	 */
	mmap_add_region(0, 0, 4096, MT_NON_CACHEABLE | MT_RW | MT_SECURE);

	rpi3_setup_page_tables(BL31_BASE, BL31_END - BL31_BASE,
			       BL_CODE_BASE, BL_CODE_END,
			       BL_RO_DATA_BASE, BL_RO_DATA_END
#if USE_COHERENT_MEM
			       , BL_COHERENT_RAM_BASE, BL_COHERENT_RAM_END
#endif
			      );

	enable_mmu_el3(0);
}

static void rpi4_prepare_dtb(void)
{
	void *dtb = (void *)rpi4_get_dtb_address();
	uint32_t gic_int_prop[3];
	int ret, offs;

	/* Return if no device tree is detected */
	if (fdt_check_header(dtb) != 0)
		return;

	ret = fdt_open_into(dtb, dtb, 0x100000);
	if (ret < 0) {
		ERROR("Invalid Device Tree at %p: error %d\n", dtb, ret);
		return;
	}

	if (dt_add_psci_node(dtb)) {
		ERROR("Failed to add PSCI Device Tree node\n");
		return;
	}

	if (dt_add_psci_cpu_enable_methods(dtb)) {
		ERROR("Failed to add PSCI cpu enable methods in Device Tree\n");
		return;
	}

	/* Reserve memory used by Trusted Firmware. */
	if (fdt_add_reserved_memory(dtb, "atf@0", 0, 0x80000))
		WARN("Failed to add reserved memory nodes to DT.\n");

	offs = fdt_node_offset_by_compatible(dtb, 0, "arm,gic-400");
	gic_int_prop[0] = cpu_to_fdt32(1);		// PPI
	gic_int_prop[1] = cpu_to_fdt32(9);		// PPI #9
	gic_int_prop[2] = cpu_to_fdt32(0x0f04);		// all cores, level high
	fdt_setprop(dtb, offs, "interrupts", gic_int_prop, 12);

	offs = fdt_path_offset(dtb, "/chosen");
	fdt_setprop_string(dtb, offs, "stdout-path", "serial0");

	ret = fdt_pack(dtb);
	if (ret < 0)
		ERROR("Failed to pack Device Tree at %p: error %d\n", dtb, ret);

	clean_dcache_range((uintptr_t)dtb, fdt_blob_size(dtb));
	INFO("Changed device tree to advertise PSCI.\n");
}

int rpi4_vc_get_board_revision(uint32_t *revision);
int rpi4_vc_get_clock(uint32_t *clock);
int rpi4_vc_max_clock(uint32_t *clock);
int rpi4_vc_set_clock(uint32_t clock);
int rpi4_vc_set_pwm(uint32_t val);
int rpi4_vc_get_pwm(uint32_t *val);
int rpi4_vc_set_power(uint32_t device,uint32_t state, uint32_t wait);

struct pcc_region_t {
		uint32_t Signature;
		uint16_t Command;
		uint16_t Status;
		uint8_t  ComSpace[8];
} __packed;

static uint64_t generic_mb_interrupt_handler(uint32_t id,
											 uint32_t flags,
											 void *handle,
											 void *cookie)
{
	uint32_t irq, intr;
	uint32_t ClockRate;
	uint32_t mbox_val;
	/* Acknowledge IRQ */
	irq = plat_ic_acknowledge_interrupt();

	intr = plat_ic_get_interrupt_id(irq);
//	ERROR("interrupt: intr=%d\n", intr);
	console_flush();

	if (intr == SECURE_TRIGGER) {
		struct pcc_region_t *pcc_region;
		mbox_val = mmio_read_32(0xFF800000+0xc0);
		mmio_write_32(0xFF800000+0xc0, mbox_val);

		if (mbox_val & 0x10000000) { //region 1
			pcc_region = (struct pcc_region_t *)0x1f0000;

			// PCC opp // clock here 0xFE003004, adjust by ClockRate below
			// eat the DT base addr at 0x1f0000
			// a command=0 is a read, a command = 1 is a write (we shouldn't see those yet)
			if (pcc_region->Command == 0) {
				uint32_t counter = mmio_read_32(0xFE003004);

				rpi4_vc_get_clock(&ClockRate);
				le32enc(&pcc_region->ComSpace[0], counter); // Reference counter register (PPERF)
				ClockRate/=100000000; //deal with 100 mhz
				counter*=ClockRate;
				counter/=15;
				le32enc(&pcc_region->ComSpace[4], counter); // Delivered counter register (APERF)
			}
			else
				ERROR("interrupt: PCC handshake cmd=%x stat=%x (%x:%x:%x:%x:%x:%x:%x:%x)\n", pcc_region->Command, pcc_region->Status,pcc_region->ComSpace[0],pcc_region->ComSpace[1],pcc_region->ComSpace[2],pcc_region->ComSpace[3],pcc_region->ComSpace[4],pcc_region->ComSpace[5],pcc_region->ComSpace[6],pcc_region->ComSpace[7]);

			// clear any existing pcc commands
			pcc_region->Signature = 0x50434300; //channel 0
			pcc_region->Command = 0;
			pcc_region->Status = 0x1; //last command complete

		}
		if (mbox_val & 0x20000000) { //region 2
			pcc_region = (struct pcc_region_t *)0x1f0080;
			rpi4_vc_get_pwm(&ClockRate);

			ERROR("interrupt: PCC handshake cmd=%x stat=%x (%x:%x:%x:%x:%x:%x:%x:%x)\n", pcc_region->Command, pcc_region->Status,pcc_region->ComSpace[0],pcc_region->ComSpace[1],pcc_region->ComSpace[2],pcc_region->ComSpace[3],pcc_region->ComSpace[4],pcc_region->ComSpace[5],pcc_region->ComSpace[6],pcc_region->ComSpace[7]);

			// clear any existing pcc commands
			pcc_region->Signature = 0x50434301; //channel 1
			pcc_region->Command = 0;
			pcc_region->Status = 0x1; //last command complete

		}
		if (mbox_val & 0x40000000) {
			// lets just set the fan speed (0-255)
			rpi4_vc_set_pwm(mbox_val & 0xFF);
			INFO("Fan speed %d\n",mbox_val & 0xFF);
		}
		else {
			if (mbox_val > 2200) {
				mbox_val = 2200;
			} else if (mbox_val < 600) {
				mbox_val = 600;
			}

			rpi4_vc_set_clock(mbox_val*1000000);
		}
		{
			pcc_region = (struct pcc_region_t *)0x1f0000;
			pcc_region->Signature = 0x50434300; //channel 0
			pcc_region->Command = 0;
			pcc_region->Status = 0x1; //last command complete
			pcc_region = (struct pcc_region_t *)0x1f0080;
			pcc_region->Signature = 0x50434301; //channel 1
			pcc_region->Command = 0;
			pcc_region->Status = 0x1; //last command complete
		}

	}

	plat_ic_end_of_interrupt(irq);
	return 0;
}

#include <drivers/delay_timer.h>
volatile uint32_t cntr;

void bl31_platform_setup(void)
{
	uint32_t int_flag;
	uint32_t ClockRate;

	rpi4_prepare_dtb();

	/* Configure the interrupt controller */
	gicv2_driver_init(&rpi4_gic_data);
	gicv2_distif_init();
	gicv2_pcpu_distif_init();
	gicv2_cpuif_enable();

	int_flag = 0U;
	set_interrupt_rm_flag((int_flag), (NON_SECURE));
	register_interrupt_type_handler(INTR_TYPE_EL3, generic_mb_interrupt_handler, int_flag);

    rpi4_vc_set_power(4,1,1);
    rpi4_vc_set_power(5,1,1);
    rpi4_vc_set_power(6,1,1);


	rpi4_vc_get_board_revision(&ClockRate);
	INFO("board rev %x\n",ClockRate);
	rpi4_vc_get_clock(&ClockRate);
	INFO("clock rate %d\n",ClockRate);
	rpi4_vc_max_clock(&ClockRate);
	INFO("max clock rate %d\n",ClockRate);

	rpi4_vc_set_pwm(0); //go into uefi with fan off

	{
		struct pcc_region_t *pcc_region;
		pcc_region = (struct pcc_region_t *)0x1f0000;
		pcc_region->Signature = 0x50434300; //channel 0
		pcc_region->Command = 0;
		pcc_region->Status = 0x1; //last command complete
		pcc_region = (struct pcc_region_t *)0x1f0080;
		pcc_region->Signature = 0x50434301; //channel 1
		pcc_region->Command = 0;
		pcc_region->Status = 0x1; //last command complete
	}

/*
	for (int_flag=0;int_flag<255;int_flag++)
	{

		rpi4_vc_get_pwm(&ClockRate);
		INFO("current pwm rate %d\n",ClockRate);
		rpi4_vc_set_pwm(int_flag);
		rpi4_vc_get_pwm(&ClockRate);
		INFO("current pwm rate %d\n",ClockRate);
        for (cntr=0;cntr<1000000;cntr++);
	}*/
}
