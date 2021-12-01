// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copied from arch/arm64/kernel/cpufeature.c
 *
 * Copyright (C) 2015 ARM Ltd.
 * Copyright (C) 2017 SiFive
 */

#include <linux/bitmap.h>
#include <linux/ctype.h>
#include <linux/of.h>
#include <asm/processor.h>
#include <asm/hwcap.h>
#include <asm/smp.h>
#include <asm/switch_to.h>

#define NUM_ALPHA_EXTS ('z' - 'a' + 1)

unsigned long elf_hwcap __read_mostly;

/* Host ISA bitmap */
static DECLARE_BITMAP(riscv_isa, RISCV_ISA_EXT_MAX) __read_mostly;

#ifdef CONFIG_FPU
__ro_after_init DEFINE_STATIC_KEY_FALSE(cpu_hwcap_fpu);
#endif

/**
 * riscv_isa_extension_base() - Get base extension word
 *
 * @isa_bitmap: ISA bitmap to use
 * Return: base extension word as unsigned long value
 *
 * NOTE: If isa_bitmap is NULL then Host ISA bitmap will be used.
 */
unsigned long riscv_isa_extension_base(const unsigned long *isa_bitmap)
{
	if (!isa_bitmap)
		return riscv_isa[0];
	return isa_bitmap[0];
}
EXPORT_SYMBOL_GPL(riscv_isa_extension_base);

/**
 * __riscv_isa_extension_available() - Check whether given extension
 * is available or not
 *
 * @isa_bitmap: ISA bitmap to use
 * @bit: bit position of the desired extension
 * Return: true or false
 *
 * NOTE: If isa_bitmap is NULL then Host ISA bitmap will be used.
 */
bool __riscv_isa_extension_available(const unsigned long *isa_bitmap, int bit)
{
	const unsigned long *bmap = (isa_bitmap) ? isa_bitmap : riscv_isa;

	if (bit >= RISCV_ISA_EXT_MAX)
		return false;

	return test_bit(bit, bmap) ? true : false;
}
EXPORT_SYMBOL_GPL(__riscv_isa_extension_available);

static inline int _decimal_part_to_uint(const char *s, unsigned int *res)
{
	unsigned int value = 0, d;

	if (!isdigit(*s))
		return -EINVAL;
	do {
		d = *s - '0';
		if (value > (UINT_MAX - d) / 10)
			return -ERANGE;
		value = value * 10 + d;
	} while (isdigit(*++s));
	*res = value;
	return 0;
}

void __init riscv_fill_hwcap(void)
{
	struct device_node *node;
	const char *isa;
	char print_str[NUM_ALPHA_EXTS + 1];
	int i, j;
	static unsigned long isa2hwcap[256] = {0};

	isa2hwcap['i'] = isa2hwcap['I'] = COMPAT_HWCAP_ISA_I;
	isa2hwcap['m'] = isa2hwcap['M'] = COMPAT_HWCAP_ISA_M;
	isa2hwcap['a'] = isa2hwcap['A'] = COMPAT_HWCAP_ISA_A;
	isa2hwcap['f'] = isa2hwcap['F'] = COMPAT_HWCAP_ISA_F;
	isa2hwcap['d'] = isa2hwcap['D'] = COMPAT_HWCAP_ISA_D;
	isa2hwcap['c'] = isa2hwcap['C'] = COMPAT_HWCAP_ISA_C;

	elf_hwcap = 0;

	bitmap_zero(riscv_isa, RISCV_ISA_EXT_MAX);

	for_each_of_cpu_node(node) {
		unsigned long this_hwcap = 0;
		unsigned long this_isa = 0;

		if (riscv_of_processor_hartid(node) < 0)
			continue;

		if (of_property_read_string(node, "riscv,isa", &isa)) {
			pr_warn("Unable to find \"riscv,isa\" devicetree entry\n");
			continue;
		}

#if IS_ENABLED(CONFIG_32BIT)
		if (!strncmp(isa, "rv32", 4))
			isa += 4;
#elif IS_ENABLED(CONFIG_64BIT)
		if (!strncmp(isa, "rv64", 4))
			isa += 4;
#endif
		for (; *isa; ++isa) {
			const char *ext = isa++;
			const char *ext_end = isa;
			unsigned int ext_major = UINT_MAX; /* default */
			unsigned int ext_minor = 0;
			bool ext_long, ext_vpair,
			     ext_err = false, ext_err_ver = false;

			switch (*ext) {
			case 's':
			case 'x':
			case 'z':
				ext_long = true;
				/* Multi-letter extension must be delimited */
				for (; *isa && *isa != '_'; ++isa)
					if (unlikely(!islower(*isa)
						     && !isdigit(*isa)))
						ext_err = true;
				/* Parse backwards */
				ext_end = isa;
				if (unlikely(ext_err))
					break;
				if (!isdigit(ext_end[-1]))
					break;
				while (isdigit(*--ext_end))
					;
				ext_vpair = (ext_end[0] == 'p')
					    && isdigit(ext_end[-1]);
				if (_decimal_part_to_uint(ext_end + 1,
							  &ext_major))
					ext_err_ver = true;
				if (!ext_vpair) {
					++ext_end;
					break;
				}
				ext_minor = ext_major;
				while (isdigit(*--ext_end))
					;
				if (_decimal_part_to_uint(++ext_end, &ext_major)
				    || ext_major == UINT_MAX)
					ext_err_ver = true;
				break;
			default:
				ext_long = false;
				if (unlikely(!islower(*ext))) {
					ext_err = true;
					break;
				}
				/* Parse forwards finding next extension */
				if (!isdigit(*isa))
					break;
				_decimal_part_to_uint(isa, &ext_major);
				if (ext_major == UINT_MAX)
					ext_err_ver = true;
				while (isdigit(*++isa))
					;
				if (*isa != 'p')
					break;
				if (!isdigit(*++isa)) {
					--isa;
					break;
				}
				if (_decimal_part_to_uint(isa, &ext_minor))
					ext_err_ver = true;
				while (isdigit(*++isa))
					;
				break;
			}
			if (*isa != '_')
				--isa;

#define MATCH_EXT(name)  (ext_end - ext == sizeof(name) - 1 \
			  && !memcmp(ext, name, sizeof(name) - 1))
			if (unlikely(ext_err))
				continue;
			this_hwcap |= isa2hwcap[(unsigned char)(*ext)];
			if (!ext_long)
				this_isa |= (1UL << (*ext - 'a'));
			if (MATCH_EXT("h"))
				pr_info("[FEATURE_TEST] H extension is supported.\n");
			if (MATCH_EXT("zba"))
				pr_info("[FEATURE_TEST] Zba extension is supported.\n");
			if (MATCH_EXT("zihintpause"))
				pr_info("[FEATURE_TEST] ZiHintPause extension is supported.\n");
			if (MATCH_EXT("zksed"))
				pr_info("[FEATURE_TEST] Zksed extension is supported.\n");
#undef MATCH_EXT
		}

		/*
		 * All "okay" hart should have same isa. Set HWCAP based on
		 * common capabilities of every "okay" hart, in case they don't
		 * have.
		 */
		if (elf_hwcap)
			elf_hwcap &= this_hwcap;
		else
			elf_hwcap = this_hwcap;

		if (riscv_isa[0])
			riscv_isa[0] &= this_isa;
		else
			riscv_isa[0] = this_isa;
	}

	/* We don't support systems with F but without D, so mask those out
	 * here. */
	if ((elf_hwcap & COMPAT_HWCAP_ISA_F) && !(elf_hwcap & COMPAT_HWCAP_ISA_D)) {
		pr_info("This kernel does not support systems with F but not D\n");
		elf_hwcap &= ~COMPAT_HWCAP_ISA_F;
	}

	memset(print_str, 0, sizeof(print_str));
	for (i = 0, j = 0; i < NUM_ALPHA_EXTS; i++)
		if (riscv_isa[0] & BIT_MASK(i))
			print_str[j++] = (char)('a' + i);
	pr_info("riscv: ISA extensions %s\n", print_str);

	memset(print_str, 0, sizeof(print_str));
	for (i = 0, j = 0; i < NUM_ALPHA_EXTS; i++)
		if (elf_hwcap & BIT_MASK(i))
			print_str[j++] = (char)('a' + i);
	pr_info("riscv: ELF capabilities %s\n", print_str);

#ifdef CONFIG_FPU
	if (elf_hwcap & (COMPAT_HWCAP_ISA_F | COMPAT_HWCAP_ISA_D))
		static_branch_enable(&cpu_hwcap_fpu);
#endif
}
