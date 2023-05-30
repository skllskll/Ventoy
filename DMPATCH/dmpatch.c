/******************************************************************************
 * dmpatch.c  ---- patch for device-mapper
 *
 * Copyright (c) 2021, longpanda <admin@ventoy.net>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 3 of the
 * License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kallsyms.h>
#include <linux/mutex.h>
#include <linux/mempool.h>
#include <linux/delay.h>
#include <linux/wait.h>
#include <linux/slab.h>

#define MAX_PATCH   4

#define magic_sig 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF

typedef int (*kprobe_reg_pf)(void *);
typedef void (*kprobe_unreg_pf)(void *);
typedef int (*printk_pf)(const char *fmt, ...);
typedef int (*set_memory_attr_pf)(unsigned long addr, int numpages);

#pragma pack(1)
typedef struct ko_param
{
    unsigned char magic[16];
    unsigned long struct_size;
    unsigned long pgsize;
    unsigned long printk_addr;
    unsigned long ro_addr;
    unsigned long rw_addr;
    unsigned long reg_kprobe_addr;
    unsigned long unreg_kprobe_addr;
    unsigned long sym_get_addr;
    unsigned long sym_get_size;
    unsigned long sym_put_addr;
    unsigned long sym_put_size;
    unsigned long kv_major;
    unsigned long ibt;
    unsigned long padding[1];
}ko_param;

#pragma pack()

static printk_pf kprintf = NULL;
static set_memory_attr_pf set_mem_ro = NULL;
static set_memory_attr_pf set_mem_rw = NULL;
static kprobe_reg_pf reg_kprobe = NULL;
static kprobe_unreg_pf unreg_kprobe = NULL;

static volatile ko_param g_ko_param = 
{
    { magic_sig },
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

#if defined(CONFIG_X86_64)
#define PATCH_OP_POS1    3
#define CODE_MATCH1(code, i) \
    (code[i] == 0x40 && code[i + 1] == 0x80 && code[i + 2] == 0xce && code[i + 3] == 0x80)

#define PATCH_OP_POS2    1
#define CODE_MATCH2(code, i) \
    (code[i] == 0x0C && code[i + 1] == 0x80 && code[i + 2] == 0x89 && code[i + 3] == 0xC6)

#elif defined(CONFIG_X86_32)
#define PATCH_OP_POS1    2
#define CODE_MATCH1(code, i) \
    (code[i] == 0x80 && code[i + 1] == 0xca && code[i + 2] == 0x80 && code[i + 3] == 0xe8)

#define PATCH_OP_POS2    2
#define CODE_MATCH2(code, i) \
    (code[i] == 0x80 && code[i + 1] == 0xca && code[i + 2] == 0x80 && code[i + 3] == 0xe8)

#else
#error "unsupported arch"
#endif

#ifdef VTOY_IBT
#ifdef CONFIG_X86_64
/* Using 64-bit values saves one instruction clearing the high half of low */
#define DECLARE_ARGS(val, low, high)	unsigned long low, high
#define EAX_EDX_VAL(val, low, high)	((low) | (high) << 32)
#define EAX_EDX_RET(val, low, high)	"=a" (low), "=d" (high)
#else
#define DECLARE_ARGS(val, low, high)	unsigned long long val
#define EAX_EDX_VAL(val, low, high)	(val)
#define EAX_EDX_RET(val, low, high)	"=A" (val)
#endif

#define	EX_TYPE_WRMSR			 8
#define	EX_TYPE_RDMSR			 9
#define MSR_IA32_S_CET			0x000006a2 /* kernel mode cet */
#define CET_ENDBR_EN			(1ULL << 2)

/* Exception table entry */
#ifdef __ASSEMBLY__

#define _ASM_EXTABLE_TYPE(from, to, type)			\
	.pushsection "__ex_table","a" ;				\
	.balign 4 ;						\
	.long (from) - . ;					\
	.long (to) - . ;					\
	.long type ;						\
	.popsection

#else /* ! __ASSEMBLY__ */

#define _ASM_EXTABLE_TYPE(from, to, type)			\
	" .pushsection \"__ex_table\",\"a\"\n"			\
	" .balign 4\n"						\
	" .long (" #from ") - .\n"				\
	" .long (" #to ") - .\n"				\
	" .long " __stringify(type) " \n"			\
	" .popsection\n"

#endif /* __ASSEMBLY__ */
#endif /* VTOY_IBT */






#define vdebug(fmt, args...) if(kprintf) kprintf(KERN_ERR fmt, ##args)

static unsigned char *g_get_patch[MAX_PATCH] = { NULL };
static unsigned char *g_put_patch[MAX_PATCH] = { NULL };

static void notrace dmpatch_restore_code(unsigned char *opCode)
{
    unsigned long align;

    if (opCode)
    {
        align = (unsigned long)opCode / g_ko_param.pgsize * g_ko_param.pgsize;
        set_mem_rw(align, 1);
        *opCode = 0x80;
        set_mem_ro(align, 1);        
    }
}

static int notrace dmpatch_replace_code
(
    int style,
    unsigned long addr, 
    unsigned long size, 
    int expect, 
    const char *desc, 
    unsigned char **patch
)
{
    int i = 0;
    int cnt = 0;
    unsigned long align;
    unsigned char *opCode = (unsigned char *)addr;

    vdebug("patch for %s style[%d] 0x%lx %d\n", desc, style, addr, (int)size);

    for (i = 0; i < (int)size - 4; i++)
    {
        if (style == 1)
        {
            if (CODE_MATCH1(opCode, i) && cnt < MAX_PATCH)
            {
                patch[cnt] = opCode + i + PATCH_OP_POS1;
                cnt++;
            }
        }
        else
        {
            if (CODE_MATCH2(opCode, i) && cnt < MAX_PATCH)
            {
                patch[cnt] = opCode + i + PATCH_OP_POS2;
                cnt++;
            }
        }
    }

    if (cnt != expect || cnt >= MAX_PATCH)
    {
        vdebug("patch error: cnt=%d expect=%d\n", cnt, expect);
        return 1;
    }


    for (i = 0; i < cnt; i++)
    {
        opCode = patch[i];
        align = (unsigned long)opCode / g_ko_param.pgsize * g_ko_param.pgsize;

        set_mem_rw(align, 1);
        *opCode = 0;
        set_mem_ro(align, 1);
    }

    return 0;
}

#ifdef VTOY_IBT
static __always_inline unsigned long long dmpatch_rdmsr(unsigned int msr)
{
	DECLARE_ARGS(val, low, high);

	asm volatile("1: rdmsr\n"
		     "2:\n"
		     _ASM_EXTABLE_TYPE(1b, 2b, EX_TYPE_RDMSR)
		     : EAX_EDX_RET(val, low, high) : "c" (msr));

	return EAX_EDX_VAL(val, low, high);
}

static __always_inline void dmpatch_wrmsr(unsigned int msr, u32 low, u32 high)
{
	asm volatile("1: wrmsr\n"
		     "2:\n"
		     _ASM_EXTABLE_TYPE(1b, 2b, EX_TYPE_WRMSR)
		     : : "c" (msr), "a"(low), "d" (high) : "memory");
}

static u64 dmpatch_ibt_save(void)
{
    u64 msr = 0;
    u64 val = 0;

    msr = dmpatch_rdmsr(MSR_IA32_S_CET);
    val = msr & ~CET_ENDBR_EN;
    dmpatch_wrmsr(MSR_IA32_S_CET, (u32)(val & 0xffffffffULL), (u32)(val >> 32));

    return msr;
}

static void dmpatch_ibt_restore(u64 save)
{
	u64 msr;

    msr = dmpatch_rdmsr(MSR_IA32_S_CET);

	msr &= ~CET_ENDBR_EN;
	msr |= (save & CET_ENDBR_EN);

    dmpatch_wrmsr(MSR_IA32_S_CET, (u32)(msr & 0xffffffffULL), (u32)(msr >> 32));
}
#else
static u64 dmpatch_ibt_save(void) { return 0; }
static void dmpatch_ibt_restore(u64 save) { (void)save; }
#endif

static int notrace dmpatch_init(void)
{
    int r = 0;
    int rc = 0;
    u64 msr = 0;
    
    if (g_ko_param.ibt == 0x8888)
    {
        msr = dmpatch_ibt_save();
    }
    
    kprintf = (printk_pf)(g_ko_param.printk_addr); 

    vdebug("dmpatch_init start pagesize=%lu ...\n", g_ko_param.pgsize);
    
    if (g_ko_param.struct_size != sizeof(ko_param))
    {
        vdebug("Invalid struct size %d %d\n", (int)g_ko_param.struct_size, (int)sizeof(ko_param));
        return -EINVAL;
    }
    
    if (g_ko_param.sym_get_addr == 0 || g_ko_param.sym_put_addr == 0 || 
        g_ko_param.ro_addr == 0 || g_ko_param.rw_addr == 0)
    {
        return -EINVAL;
    }

    set_mem_ro = (set_memory_attr_pf)(g_ko_param.ro_addr);
    set_mem_rw = (set_memory_attr_pf)(g_ko_param.rw_addr);
    reg_kprobe = (kprobe_reg_pf)g_ko_param.reg_kprobe_addr;
    unreg_kprobe = (kprobe_unreg_pf)g_ko_param.unreg_kprobe_addr;

    r = dmpatch_replace_code(1, g_ko_param.sym_get_addr, g_ko_param.sym_get_size, 2, "dm_get_table_device", g_get_patch);
    if (r && g_ko_param.kv_major >= 5)
    {
        vdebug("new patch dm_get_table_device...\n");
        r = dmpatch_replace_code(2, g_ko_param.sym_get_addr, g_ko_param.sym_get_size, 1, "dm_get_table_device", g_get_patch);
    }
    
    if (r)
    {
        rc = -EINVAL;
        goto out;
    }
    vdebug("patch dm_get_table_device success\n");

    r = dmpatch_replace_code(1, g_ko_param.sym_put_addr, g_ko_param.sym_put_size, 1, "dm_put_table_device", g_put_patch);
    if (r)
    {
        rc = -EINVAL;
        goto out;
    }
    vdebug("patch dm_put_table_device success\n");

    vdebug("#####################################\n");
    vdebug("######## dm patch success ###########\n");
    vdebug("#####################################\n");

    if (g_ko_param.ibt == 0x8888)
    {
        dmpatch_ibt_restore(msr);
    }

out:

	return rc;
}

static void notrace dmpatch_exit(void)
{
    int i = 0;
    u64 msr;

    if (g_ko_param.ibt == 0x8888)
    {
        msr = dmpatch_ibt_save();
    }

    for (i = 0; i < MAX_PATCH; i++)
    {
        dmpatch_restore_code(g_get_patch[i]);
        dmpatch_restore_code(g_put_patch[i]);
    }

    vdebug("dmpatch_exit success\n");

    if (g_ko_param.ibt == 0x8888)
    {
        dmpatch_ibt_restore(msr);
    }
}

module_init(dmpatch_init);
module_exit(dmpatch_exit);


MODULE_DESCRIPTION("dmpatch driver");
MODULE_AUTHOR("longpanda <admin@ventoy.net>");
MODULE_LICENSE("GPL");

